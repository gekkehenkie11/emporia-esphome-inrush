#include "cycle_capture.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace esphome {
namespace emporia_vue {

static const char *const WIRE_NAMES[3] = {"BLACK", "RED", "BLUE"};

bool cycle_record_checksum_ok(const CycleRecord &r) {
  const uint8_t *b = reinterpret_cast<const uint8_t *>(&r);
  uint8_t s = 0;
  for (size_t k = 0; k < 31; k++)
    s += b[k];
  return static_cast<uint8_t>(~s) == r.checksum;
}

CycleStatus parse_cycle_status(const uint8_t *buf, size_t len) {
  CycleStatus st;
  if (len < CYC_STATUS_LEN || buf[0] != 0xA5 || buf[1] != 0x5A)
    return st;
  st.valid = true;
  st.version = buf[2];
  st.record_size = buf[3];
  st.available = buf[4];
  st.lost = buf[5];
  st.frame_rate = static_cast<uint16_t>(buf[6] | (buf[7] << 8));
  st.aux_port[0] = buf[8];
  st.aux_port[1] = buf[9];
  st.ring_size = buf[10];
  return st;
}

float StartSummary::max_peak_rms() const { return std::fmax(peak_rms[0], std::fmax(peak_rms[1], peak_rms[2])); }
float StartSummary::max_peak_instant() const {
  return std::fmax(peak_instant[0], std::fmax(peak_instant[1], peak_instant[2]));
}
float StartSummary::max_inrush_100ms() const {
  return std::fmax(inrush_100ms[0], std::fmax(inrush_100ms[1], inrush_100ms[2]));
}
float StartSummary::max_running() const { return std::fmax(running[0], std::fmax(running[1], running[2])); }
float StartSummary::max_start_rms() const { return std::fmax(start_rms[0], std::fmax(start_rms[1], start_rms[2])); }
float StartSummary::min_voltage(const CycleScaling &s) const {
  float m = NAN;
  for (int w = 0; w < 3; w++)
    if (s.voltage_per_raw[w] > 0)
      m = std::isnan(m) ? v_min[w] : std::fmin(m, v_min[w]);
  return m;
}
float StartSummary::max_sag(const CycleScaling &s) const {
  float m = NAN;
  for (int w = 0; w < 3; w++)
    if (s.voltage_per_raw[w] > 0)
      m = std::isnan(m) ? sag_pct[w] : std::fmax(m, sag_pct[w]);
  return m;
}

CycleSample StartAnalyzer::convert_(const CycleRecord &r) const {
  CycleSample s;
  s.seq = r.seq;
  s.dt = (r.nsamples > 0 && this->scaling_.frame_rate > 0) ? r.nsamples / this->scaling_.frame_rate : 1.0f / 60.0f;
  for (int k = 0; k < 3; k++) {
    s.i[k] = r.rms_i[k] * this->scaling_.current_per_raw[k];
    // raw RMS is 10x ADC counts, the peak is in ADC counts
    s.ipk[k] = r.peak_i[k] * 10.0f * this->scaling_.current_per_raw[k];
    s.v[k] = r.rms_v[k] * this->scaling_.voltage_per_raw[k];
  }
  for (int a = 0; a < 2; a++)
    s.aux[a] = r.rms_aux[a] * this->scaling_.aux_per_raw[a];
  s.clip = r.clip;
  s.flags = r.flags;
  return s;
}

float StartAnalyzer::max_i_(const CycleSample &s) const { return std::fmax(s.i[0], std::fmax(s.i[1], s.i[2])); }

bool StartAnalyzer::add(const CycleRecord &r) {
  this->records_++;
  if (this->have_last_seq_ && !(r.flags & CYCF_FIRST)) {
    uint16_t missing = static_cast<uint16_t>(r.seq - this->last_seq_ - 1);
    if (missing > 0 && missing < 10000) {
      this->gaps_total_ += missing;
      if (this->capturing_)
        this->trace_gaps_ += missing;
    }
  }
  this->have_last_seq_ = true;
  this->last_seq_ = r.seq;

  CycleSample s = this->convert_(r);
  float imax = this->max_i_(s);

  // live statistics
  if (imax > this->live_max_i_)
    this->live_max_i_ = imax;
  for (int w = 0; w < 3; w++) {
    if (this->scaling_.voltage_per_raw[w] <= 0)
      continue;
    if (this->live_min_v_ < 0 || s.v[w] < this->live_min_v_)
      this->live_min_v_ = s.v[w];
  }
  if (!(s.flags & (CYCF_NO_ZEROCROSS | CYCF_MERGED | CYCF_FIRST)) && s.dt > 0) {
    float f = 1.0f / s.dt;
    if (this->live_min_f_ < 0 || f < this->live_min_f_)
      this->live_min_f_ = f;
    if (f > this->live_max_f_)
      this->live_max_f_ = f;
  }

  if (this->capturing_) {
    this->trace_.push_back(r);
    this->post_time_ += s.dt;
    if (this->post_time_ >= this->post_s_) {
      this->finish_();
      return true;
    }
    return false;
  }

  float rearm = this->rearm_current_ >= 0 ? this->rearm_current_ : this->trigger_current_ * 0.5f;
  if (imax < rearm) {
    this->armed_time_ += s.dt;
    if (this->armed_time_ >= 0.2f)
      this->armed_ = true;
  } else {
    this->armed_time_ = 0;
  }

  // keep up to 1 s more than pre_s_: the real onset can be before the trigger (part-winding first
  // step or a current-limited start below the trigger level)
  size_t pre_cap = static_cast<size_t>(std::ceil((this->pre_s_ + 1.0f) * 70.0f)) + 1;
  if (this->armed_ && imax >= this->trigger_current_) {
    // start of a capture: recent cycles in order, oldest first
    std::vector<CycleRecord> ordered;
    size_t n = this->pre_.size();
    ordered.reserve(n + 1);
    for (size_t k = 0; k < n; k++)
      ordered.push_back(this->pre_[(this->pre_pos_ + k) % n]);
    ordered.push_back(r);
    // onset = first cycle of the uninterrupted run above the re-arm level that ends in the trigger
    size_t onset = ordered.size() - 1;
    while (onset > 0 && this->max_i_(this->convert_(ordered[onset - 1])) >= rearm)
      onset--;
    // keep pre_s_ before the onset
    float t = 0;
    size_t first = onset;
    while (first > 0 && t + this->convert_(ordered[first - 1]).dt <= this->pre_s_ + 1e-4f) {
      t += this->convert_(ordered[first - 1]).dt;
      first--;
    }
    this->trace_.assign(ordered.begin() + first, ordered.end());
    this->summary_.trigger_index = static_cast<uint16_t>(onset - first);
    this->post_time_ = s.dt;
    this->trace_gaps_ = 0;
    this->capturing_ = true;
    this->pre_.clear();
    this->pre_pos_ = 0;
    return false;
  }

  if (this->pre_.size() < pre_cap) {
    this->pre_.push_back(r);
    this->pre_pos_ = 0;
  } else {
    this->pre_[this->pre_pos_] = r;
    this->pre_pos_ = (this->pre_pos_ + 1) % this->pre_.size();
  }
  return false;
}

std::vector<float> StartAnalyzer::times_(const std::vector<CycleRecord> &tr, size_t onset) const {
  // start time of each cycle relative to the start of the onset cycle
  std::vector<float> t(tr.size(), 0.0f);
  for (size_t k = onset + 1; k < tr.size(); k++)
    t[k] = t[k - 1] + this->convert_(tr[k - 1]).dt;
  for (size_t k = onset; k-- > 0;)
    t[k] = t[k + 1] - this->convert_(tr[k]).dt;
  return t;
}

void StartAnalyzer::finish_() {
  const std::vector<CycleRecord> &raw = this->trace_;
  auto S = [&](size_t k) { return this->convert_(raw[k]); };
  StartSummary sm;
  sm.number = ++this->number_;
  sm.cycles = static_cast<uint16_t>(raw.size());
  sm.trigger_index = this->summary_.trigger_index;
  const size_t trig = sm.trigger_index;
  const size_t n = raw.size();
  sm.gaps = this->trace_gaps_;
  std::vector<float> t = this->times_(raw, trig);

  for (int m = 0; m < 3; m++) {
    // first 100 ms from the onset
    double acc = 0, tacc = 0;
    for (size_t k = trig; k < n && tacc < 0.1 - 1e-6; k++) {
      CycleSample c = S(k);
      double w = std::fmin(static_cast<double>(c.dt), 0.1 - tacc);
      acc += static_cast<double>(c.i[m]) * c.i[m] * w;
      tacc += w;
    }
    sm.inrush_100ms[m] = tacc > 0 ? static_cast<float>(std::sqrt(acc / tacc)) : 0;

    for (size_t k = trig; k < n; k++) {
      CycleSample c = S(k);
      sm.peak_rms[m] = std::fmax(sm.peak_rms[m], c.i[m]);
      sm.peak_instant[m] = std::fmax(sm.peak_instant[m], c.ipk[m]);
    }

    // running current: RMS over the last 0.5 s
    acc = 0;
    tacc = 0;
    for (size_t k = n; k-- > trig && tacc < 0.5;) {
      CycleSample c = S(k);
      acc += static_cast<double>(c.i[m]) * c.i[m] * c.dt;
      tacc += c.dt;
    }
    sm.running[m] = tacc > 0 ? static_cast<float>(std::sqrt(acc / tacc)) : 0;
  }

  // start duration: until the current stays below the settle level
  float rearm = this->rearm_current_ >= 0 ? this->rearm_current_ : this->trigger_current_ * 0.5f;
  float thr = std::fmax(this->settle_factor_ * sm.max_running(), rearm);
  size_t last_high = trig;
  for (size_t k = trig; k < n; k++)
    if (this->max_i_(S(k)) > thr)
      last_high = k;
  sm.duration = t[last_high] + S(last_high).dt;
  sm.settled = last_high + 1 < n;

  // RMS over the whole start, onset up to and including the last cycle above the settle level
  for (int m = 0; m < 3; m++) {
    double acc = 0, tacc = 0;
    for (size_t k = trig; k <= last_high; k++) {
      CycleSample c = S(k);
      acc += static_cast<double>(c.i[m]) * c.i[m] * c.dt;
      tacc += c.dt;
    }
    sm.start_rms[m] = tacc > 0 ? static_cast<float>(std::sqrt(acc / tacc)) : 0;
  }

  // voltage and frequency
  for (int w = 0; w < 3; w++) {
    double acc = 0, tacc = 0;
    for (size_t k = 0; k < trig; k++) {
      CycleSample c = S(k);
      acc += c.v[w] * c.dt;
      tacc += c.dt;
    }
    sm.v_before[w] = tacc > 0 ? static_cast<float>(acc / tacc) : S(trig).v[w];
    sm.v_min[w] = S(trig).v[w];
    for (size_t k = trig; k < n; k++)
      sm.v_min[w] = std::fmin(sm.v_min[w], S(k).v[w]);
    sm.sag_pct[w] = sm.v_before[w] > 0 ? 100.0f * (1.0f - sm.v_min[w] / sm.v_before[w]) : 0;
  }
  double fs = 0;
  int fn = 0;
  for (size_t k = 0; k < n; k++) {
    CycleSample c = S(k);
    if ((c.flags & (CYCF_NO_ZEROCROSS | CYCF_MERGED | CYCF_FIRST)) || c.dt <= 0)
      continue;
    float f = 1.0f / c.dt;
    if (k < trig) {
      fs += f;
      fn++;
    } else {
      sm.f_min = (sm.f_min == 0) ? f : std::fmin(sm.f_min, f);
      sm.f_max = std::fmax(sm.f_max, f);
    }
  }
  sm.f_before = fn > 0 ? static_cast<float>(fs / fn) : 0;

  for (size_t k = trig; k < n; k++) {
    sm.clipped_mask |= raw[k].clip & 0x07;
    if (raw[k].flags & CYCF_MERGED)
      sm.merged++;
  }

  this->summary_ = sm;
  this->last_trace_.swap(this->trace_);
  this->trace_.clear();
  this->trace_.shrink_to_fit();
  this->capturing_ = false;
  this->armed_ = false;
  this->armed_time_ = 0;
}

static void append(std::string &s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void append(std::string &s, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0)
    s.append(buf, n < static_cast<int>(sizeof(buf)) ? n : static_cast<int>(sizeof(buf)) - 1);
}

std::string StartAnalyzer::summary_json() const {
  const StartSummary &sm = this->summary_;
  const CycleScaling &sc = this->scaling_;
  std::string s = "{";
  append(s, "\"start\":%u,\"cycles\":%u", static_cast<unsigned>(sm.number), static_cast<unsigned>(sm.cycles));

  auto per_phase = [&](const char *key, const float *vals, const char *fmt) {
    append(s, ",\"%s\":{", key);
    bool first = true;
    for (char letter : {'A', 'B', 'C'}) {
      for (int k = 0; k < 3; k++) {
        if (sc.mains_name[k] != letter)
          continue;
        append(s, first ? "\"%c\":" : ",\"%c\":", letter);
        append(s, fmt, vals[k]);
        first = false;
      }
    }
    s += "}";
  };
  auto per_wire = [&](const char *key, const float *vals, const char *fmt) {
    append(s, ",\"%s\":{", key);
    bool first = true;
    for (int w = 0; w < 3; w++) {
      if (sc.voltage_per_raw[w] <= 0)
        continue;
      append(s, first ? "\"%s\":" : ",\"%s\":", WIRE_NAMES[w]);
      append(s, fmt, vals[w]);
      first = false;
    }
    s += "}";
  };

  per_phase("peak_a", sm.peak_rms, "%.1f");
  per_phase("inrush_100ms_a", sm.inrush_100ms, "%.1f");
  per_phase("peak_instant_a", sm.peak_instant, "%.1f");
  per_phase("start_rms_a", sm.start_rms, "%.1f");
  per_phase("running_a", sm.running, "%.1f");
  append(s, ",\"duration_s\":%.3f,\"settled\":%s", sm.duration, sm.settled ? "true" : "false");
  per_wire("v_before", sm.v_before, "%.1f");
  per_wire("v_min", sm.v_min, "%.1f");
  per_wire("sag_pct", sm.sag_pct, "%.1f");
  append(s, ",\"f_before\":%.2f,\"f_min\":%.2f,\"f_max\":%.2f", sm.f_before, sm.f_min, sm.f_max);
  s += ",\"clipped\":\"";
  for (char letter : {'A', 'B', 'C'})
    for (int k = 0; k < 3; k++)
      if (sc.mains_name[k] == letter && (sm.clipped_mask & (1 << k)))
        s += letter;
  s += "\"";
  append(s, ",\"gaps\":%u,\"merged\":%u}", static_cast<unsigned>(sm.gaps), static_cast<unsigned>(sm.merged));
  return s;
}

std::string StartAnalyzer::trace_line(size_t index) const {
  const CycleScaling &sc = this->scaling_;
  const std::vector<CycleRecord> &tr = this->last_trace_;
  std::string l;
  if (tr.empty() || index > tr.size())
    return l;
  int order[3];
  int n = 0;
  for (char letter : {'A', 'B', 'C'})
    for (int k = 0; k < 3; k++)
      if (sc.mains_name[k] == letter)
        order[n++] = k;
  bool aux = sc.aux_per_raw[0] > 0 || sc.aux_per_raw[1] > 0;

  if (index == 0) {
    l = "t_ms";
    for (int q = 0; q < n; q++)
      append(l, ",i_%c", sc.mains_name[order[q]]);
    for (int q = 0; q < n; q++)
      append(l, ",pk_%c", sc.mains_name[order[q]]);
    for (int w = 0; w < 3; w++)
      if (sc.voltage_per_raw[w] > 0)
        append(l, ",v_%s", WIRE_NAMES[w]);
    l += ",hz,clip,flags";
    if (aux)
      l += ",aux0,aux1";
    return l;
  }

  const size_t k = index - 1;
  const size_t trig = this->summary_.trigger_index;
  // time of cycle k relative to the onset, from the frame counts
  int32_t frames = 0;
  if (k >= trig) {
    for (size_t q = trig; q < k; q++)
      frames += tr[q].nsamples;
  } else {
    for (size_t q = k; q < trig; q++)
      frames -= tr[q].nsamples;
  }
  float t = sc.frame_rate > 0 ? frames / sc.frame_rate : 0;
  CycleSample c = this->convert_(tr[k]);
  append(l, "%d", static_cast<int>(std::lround(t * 1000.0f)));
  for (int q = 0; q < n; q++)
    append(l, ",%.1f", c.i[order[q]]);
  for (int q = 0; q < n; q++)
    append(l, ",%.0f", c.ipk[order[q]]);
  for (int w = 0; w < 3; w++)
    if (sc.voltage_per_raw[w] > 0)
      append(l, ",%.1f", c.v[w]);
  append(l, ",%.2f,%u,%u", c.dt > 0 ? 1.0f / c.dt : 0.0f, static_cast<unsigned>(c.clip),
         static_cast<unsigned>(c.flags));
  if (aux)
    append(l, ",%.2f,%.2f", c.aux[0], c.aux[1]);
  return l;
}

std::string StartAnalyzer::trace_csv() const {
  std::string s;
  const size_t n = this->trace_line_count();
  s.reserve(n * 72);
  for (size_t k = 0; k < n; k++) {
    s += this->trace_line(k);
    s += '\n';
  }
  return s;
}

static float take(float &v) {
  float r = v < 0 ? NAN : v;
  v = -1;
  return r;
}
float StartAnalyzer::take_max_current() { return take(this->live_max_i_); }
float StartAnalyzer::take_min_voltage() { return take(this->live_min_v_); }
float StartAnalyzer::take_min_frequency() { return take(this->live_min_f_); }
float StartAnalyzer::take_max_frequency() { return take(this->live_max_f_); }

}  // namespace emporia_vue
}  // namespace esphome
