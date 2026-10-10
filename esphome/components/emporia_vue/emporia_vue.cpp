#include "emporia_vue.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace emporia_vue {

static const char *const TAG = "emporia_vue";

#if defined(EMPORIA_VUE_VARIANT_VUE3)
static constexpr float EMPORIA_VUE_FREQUENCY_CONSTANT = 19610.0f;
#else
static constexpr float EMPORIA_VUE_FREQUENCY_CONSTANT = 25310.0f;
#endif

void EmporiaVueComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Emporia Vue");
#if defined(EMPORIA_VUE_VARIANT_VUE3)
  ESP_LOGCONFIG(TAG, "  Variant: Vue 3");
#else
  ESP_LOGCONFIG(TAG, "  Variant: Vue 2");
#endif
  LOG_I2C_DEVICE(this);

  for (auto *phase : this->phases_) {
    std::string wire;
    switch (phase->get_input_wire()) {
      case PhaseInputWire::BLACK:
        wire = "BLACK";
        break;
      case PhaseInputWire::RED:
        wire = "RED";
        break;
      case PhaseInputWire::BLUE:
        wire = "BLUE";
        break;
    }
    ESP_LOGCONFIG(TAG, "  Phase");
    ESP_LOGCONFIG(TAG, "    Wire: %s", wire.c_str());
    ESP_LOGCONFIG(TAG, "    Calibration: %f", phase->get_calibration());
    LOG_SENSOR("    ", "Voltage", phase->get_voltage_sensor());
  }

  if (this->cycle_capture_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Cycle capture: poll %u ms, aux ports %u/%u", (unsigned) this->cycle_capture_->poll_interval_ms_,
                  this->cycle_capture_->aux_port_[0], this->cycle_capture_->aux_port_[1]);
  }

  for (auto *ct_clamp : this->ct_clamps_) {
    ESP_LOGCONFIG(TAG, "  CT Clamp");
    ESP_LOGCONFIG(TAG, "    Phase Calibration: %f", ct_clamp->get_phase()->get_calibration());
    ESP_LOGCONFIG(TAG, "    CT Port Index: %d", ct_clamp->get_input_port());
    LOG_SENSOR("    ", "Power", ct_clamp->get_power_sensor());
    LOG_SENSOR("    ", "Current", ct_clamp->get_current_sensor());
  }
}

void EmporiaVueComponent::setup() {
  auto *cc = this->cycle_capture_;
  if (cc == nullptr)
    return;
  // same scaling as the regular sensors, so the numbers can be compared directly
  CycleScaling sc;
  for (int k = 0; k < 3; k++)
    sc.current_per_raw[k] = 775.0f / 42624.0f * cc->mains_multiplier_;
  for (int a = 0; a < 2; a++)
    sc.aux_per_raw[a] = cc->aux_port_[a] != 0xFF ? 775.0f / 170496.0f * cc->aux_multiplier_ : 0.0f;
  for (auto *phase : this->phases_)
    sc.voltage_per_raw[phase->get_input_wire()] = phase->get_calibration();
#if defined(EMPORIA_VUE_VARIANT_VUE3)
  // port A reads current[2], C reads current[0]
  sc.mains_name[0] = 'C';
  sc.mains_name[1] = 'B';
  sc.mains_name[2] = 'A';
#endif
  cc->analyzer_.set_scaling(sc);
}

void EmporiaVueComponent::loop() {
  if (this->cycle_capture_ != nullptr && !this->cycle_capture_->disabled_)
    this->cycle_capture_poll_();
}

void EmporiaVueComponent::cycle_capture_poll_() {
  auto *cc = this->cycle_capture_;
  const uint32_t now = millis();

  // the trace of the last start goes to the log a few lines per loop, so the loop never blocks long
  if (cc->pending_count_ > 0) {
    for (int k = 0; k < 4 && cc->pending_pos_ < cc->pending_count_; k++, cc->pending_pos_++)
      ESP_LOGI(TAG, "start %u trace: %s", (unsigned) cc->pending_number_,
               cc->analyzer_.trace_line(cc->pending_pos_).c_str());
    if (cc->pending_pos_ >= cc->pending_count_) {
      cc->pending_count_ = 0;
      cc->pending_pos_ = 0;
      if (!cc->analyzer_.capturing())
        cc->analyzer_.release_trace();
    }
  }

  if (now - cc->last_poll_ < cc->poll_interval_ms_)
    return;
  cc->last_poll_ = now;

  uint8_t cmd = CYC_CMD_STATUS;
  uint8_t st[CYC_STATUS_LEN];
  if (this->write(&cmd, 1) != i2c::ERROR_OK || this->read(st, sizeof(st)) != i2c::ERROR_OK) {
    if (now - cc->last_error_log_ > 10000) {
      cc->last_error_log_ = now;
      ESP_LOGW(TAG, "Cycle capture: I2C error reading status");
    }
    return;
  }
  CycleStatus status = parse_cycle_status(st, sizeof(st));
  if (!status.valid) {
    if (!cc->initialized_ && ++cc->probe_failures_ >= 40) {
      ESP_LOGE(TAG, "Cycle capture: the SAMD09 does not answer the status command. Flash the cycle-capture "
                    "SAMD09 firmware. Cycle capture is disabled, the regular sensors keep working.");
      cc->disabled_ = true;
    }
    return;
  }
  if (!cc->initialized_) {
    cc->initialized_ = true;
    ESP_LOGI(TAG, "Cycle capture: SAMD09 firmware v%u, %u Hz frame rate, ring of %u records", status.version,
             status.frame_rate, status.ring_size);
    if (status.record_size != sizeof(CycleRecord)) {
      ESP_LOGE(TAG, "Cycle capture: record size %u, expected %u. Disabled.", status.record_size,
               (unsigned) sizeof(CycleRecord));
      cc->disabled_ = true;
      return;
    }
    CycleScaling sc = cc->analyzer_.get_scaling();
    sc.frame_rate = cc->fixed_frame_rate_ > 0 ? cc->fixed_frame_rate_ : status.frame_rate;
    cc->analyzer_.set_scaling(sc);
  }

  // select the aux inputs (again after a SAMD09 reset)
  if (status.aux_port[0] != cc->aux_port_[0] || status.aux_port[1] != cc->aux_port_[1]) {
    uint8_t cfg[3] = {CYC_CMD_CONFIG, cc->aux_port_[0], cc->aux_port_[1]};
    this->write(cfg, sizeof(cfg));
  }

  if (cc->have_lost_) {
    uint8_t lost = status.lost - cc->last_lost_;
    if (lost > 0)
      ESP_LOGW(TAG, "Cycle capture: %u cycle records lost, the ESP did not read in time", lost);
    cc->lost_total_ += lost;
  }
  cc->last_lost_ = status.lost;
  cc->have_lost_ = true;

  int remaining = status.available;
  uint32_t frames_read = 0;
  for (int transactions = 0; remaining > 0 && transactions < 4; transactions++) {
    const int chunk = remaining > 7 ? 7 : remaining;
    uint8_t buf[7 * sizeof(CycleRecord)];
    cmd = CYC_CMD_CYCLES;
    if (this->write(&cmd, 1) != i2c::ERROR_OK ||
        this->read(buf, chunk * sizeof(CycleRecord)) != i2c::ERROR_OK) {
      ESP_LOGW(TAG, "Cycle capture: I2C error reading records");
      return;
    }
    for (int r = 0; r < chunk; r++) {
      CycleRecord rec;
      std::memcpy(&rec, buf + r * sizeof(CycleRecord), sizeof(CycleRecord));
      if (!cycle_record_checksum_ok(rec)) {
        cc->bad_checksums_++;
        ESP_LOGV(TAG, "Cycle capture: record with bad checksum");
        continue;
      }
      if (rec.flags & CYCF_FIRST)
        ESP_LOGI(TAG, "Cycle capture: SAMD09 (re)started");
      frames_read += rec.nsamples;
      if (cc->analyzer_.add(rec))
        this->cycle_capture_publish_start_();
    }
    remaining -= chunk;
  }

  // The SAMD09 runs on its internal DFLL (no crystal), so its frame rate is only known to a few percent.
  // Count the frames it reports against the ESP32 clock. Windows with lost records are discarded.
  if (cc->fixed_frame_rate_ <= 0 && frames_read > 0) {
    if (cc->cal_start_ms_ == 0 || cc->analyzer_.gaps() != cc->cal_gaps_ref_ || cc->lost_total_ != cc->cal_lost_ref_) {
      cc->cal_start_ms_ = now;  // (re)start the window; the records of this poll are before it
      cc->cal_frames_ = 0;
      cc->cal_gaps_ref_ = cc->analyzer_.gaps();
      cc->cal_lost_ref_ = cc->lost_total_;
    } else {
      cc->cal_frames_ += frames_read;
      const uint32_t elapsed = now - cc->cal_start_ms_;
      if (elapsed >= 60000) {
        float fr = cc->cal_frames_ * 1000.0f / elapsed;
        if (fr > 20000.0f && fr < 32000.0f) {
          CycleScaling sc = cc->analyzer_.get_scaling();
          if (cc->measured_frame_rate_ == 0 || std::fabs(fr - sc.frame_rate) / sc.frame_rate > 0.002f)
            ESP_LOGI(TAG, "Cycle capture: SAMD09 frame rate measured as %.0f frames/s (nominal %.0f)", fr,
                     (float) status.frame_rate);
          sc.frame_rate = cc->measured_frame_rate_ == 0 ? fr : 0.7f * sc.frame_rate + 0.3f * fr;
          cc->measured_frame_rate_ = sc.frame_rate;
          cc->analyzer_.set_scaling(sc);
        }
        cc->cal_start_ms_ = now;
        cc->cal_frames_ = 0;
      }
    }
  }

  if (now - cc->last_live_ >= cc->live_interval_ms_) {
    cc->last_live_ = now;
    float v;
    v = cc->analyzer_.take_max_current();
    if (cc->max_current_sensor_ != nullptr && !std::isnan(v))
      cc->max_current_sensor_->publish_state(v);
    v = cc->analyzer_.take_min_voltage();
    if (cc->min_voltage_sensor_ != nullptr && !std::isnan(v))
      cc->min_voltage_sensor_->publish_state(v);
    v = cc->analyzer_.take_min_frequency();
    if (cc->min_frequency_sensor_ != nullptr && !std::isnan(v))
      cc->min_frequency_sensor_->publish_state(v);
    v = cc->analyzer_.take_max_frequency();
    if (cc->max_frequency_sensor_ != nullptr && !std::isnan(v))
      cc->max_frequency_sensor_->publish_state(v);
  }
}

void EmporiaVueComponent::cycle_capture_publish_start_() {
  auto *cc = this->cycle_capture_;
  const StartSummary &sm = cc->analyzer_.summary();
  const CycleScaling &sc = cc->analyzer_.get_scaling();

  char clipped[4] = {0};
  int nc = 0;
  for (char letter : {'A', 'B', 'C'})
    for (int k = 0; k < 3; k++)
      if (sc.mains_name[k] == letter && (sm.clipped_mask & (1 << k)))
        clipped[nc++] = letter;
  ESP_LOGI(TAG,
           "Start %u: peak %.0f A (one cycle), inrush 100 ms %.0f A, instant %.0f A, running %.1f A, "
           "duration %.2f s%s, Vmin %.1f V (sag %.1f%%), f %.2f-%.2f Hz, clipped: %s, gaps %u",
           (unsigned) sm.number, sm.max_peak_rms(), sm.max_inrush_100ms(), sm.max_peak_instant(), sm.max_running(),
           sm.duration, sm.settled ? "" : " (not settled)", sm.min_voltage(sc), sm.max_sag(sc), sm.f_min, sm.f_max,
           nc ? clipped : "-", (unsigned) sm.gaps);
  if (sm.clipped_mask)
    ESP_LOGW(TAG, "Start %u: current reached the ADC limit, peak and inrush values are too low. Use CTs with a "
                  "higher rating (and mains_current_multiplier).",
             (unsigned) sm.number);

  auto pub = [](sensor::Sensor *s, float v) {
    if (s != nullptr && !std::isnan(v))
      s->publish_state(v);
  };
  pub(cc->start_peak_current_sensor_, sm.max_peak_rms());
  pub(cc->start_inrush_100ms_sensor_, sm.max_inrush_100ms());
  pub(cc->start_peak_instant_sensor_, sm.max_peak_instant());
  pub(cc->start_running_current_sensor_, sm.max_running());
  pub(cc->start_duration_sensor_, sm.duration);
  pub(cc->start_voltage_min_sensor_, sm.min_voltage(sc));
  pub(cc->start_voltage_sag_sensor_, sm.max_sag(sc));
  pub(cc->start_clipped_sensor_, sm.clipped_mask ? 1.0f : 0.0f);
  pub(cc->start_count_sensor_, (float) sm.number);

  if (cc->has_start_callback_)
    cc->start_callback_.call(cc->analyzer_.summary_json(), cc->analyzer_.trace_csv());
  if (cc->log_trace_) {
    cc->pending_pos_ = 0;
    cc->pending_count_ = cc->analyzer_.trace_line_count();
    cc->pending_number_ = sm.number;
  } else {
    cc->analyzer_.release_trace();
  }
}

void EmporiaVueComponent::update() {
  SensorReading sensor_reading;

  i2c::ErrorCode err = read(reinterpret_cast<uint8_t *>(&sensor_reading), sizeof(sensor_reading));

  if (err != i2c::ErrorCode::ERROR_OK) {
    ESP_LOGE(TAG, "Failed to read from sensor due to I2C error %d", err);
    return;
  }

  if (sensor_reading.end != 0) {
    ESP_LOGE(TAG, "Failed to read from sensor due to a malformed reading, should end in null bytes but is %d",
             sensor_reading.end);
    return;
  }

  if (!sensor_reading.is_unread) {
    ESP_LOGV(TAG, "Ignoring sensor reading that is marked as read");
    return;
  }

  ESP_LOGV(TAG, "Received sensor reading with sequence #%d", sensor_reading.sequence_num);

  if (this->last_sequence_num_ && sensor_reading.sequence_num > this->last_sequence_num_ + 1) {
    ESP_LOGW(TAG, "Detected %d missing reading(s), data may not be accurate!",
             sensor_reading.sequence_num - this->last_sequence_num_ - 1);
  }

  for (auto *phase : this->phases_) {
    phase->update_from_reading(sensor_reading);
  }
  for (auto *ct_clamp : this->ct_clamps_) {
    ct_clamp->update_from_reading(sensor_reading);
  }

  this->last_sequence_num_ = sensor_reading.sequence_num;

  this->callback_.call();
}

void EmporiaVueComponent::add_on_update_callback(std::function<void()> &&callback) {
  this->callback_.add(std::move(callback));
}

static inline bool is_mains_port(CTInputPort port) {
  return port == CTInputPort::A || port == CTInputPort::B || port == CTInputPort::C;
}

void PhaseConfig::update_from_reading(const SensorReading &sensor_reading) {
  if (this->voltage_sensor_) {
    float calibrated_voltage = sensor_reading.voltage[this->input_wire_] * this->calibration_;
    this->voltage_sensor_->publish_state(calibrated_voltage);
  }

  uint16_t raw_frequency = sensor_reading.frequency;
  // validation that these sensors are allowed on this phase is done in the codegen stage
  if (this->frequency_sensor_) {
    // see https://github.com/emporia-vue-local/esphome/pull/88 for constant explanation
    float frequency = EMPORIA_VUE_FREQUENCY_CONSTANT / static_cast<float>(raw_frequency);
    this->frequency_sensor_->publish_state(frequency);
  }
  if (this->phase_angle_sensor_) {
    uint16_t raw_phase_angle = sensor_reading.degrees[((uint8_t) this->input_wire_) - 1];
    float phase_angle = ((float) raw_phase_angle) * 360.0f / ((float) raw_frequency);
    // this is truncated to a uint16_t on the vue 2
    this->phase_angle_sensor_->publish_state(phase_angle);
  }
}

int32_t PhaseConfig::extract_power_for_phase(const ReadingPowerEntry &power_entry) {
  switch (this->input_wire_) {
    case PhaseInputWire::BLACK:
      return power_entry.phase_black;
    case PhaseInputWire::RED:
      return power_entry.phase_red;
    case PhaseInputWire::BLUE:
      return power_entry.phase_blue;
    default:
      ESP_LOGE(TAG, "Unsupported phase input wire, this should never happen");
      return -1;
  }
}

void CTClampConfig::update_from_reading(const SensorReading &sensor_reading) {
  if (this->power_sensor_) {
    ReadingPowerEntry power_entry = sensor_reading.power[this->input_port_];
    int32_t raw_power = this->phase_->extract_power_for_phase(power_entry);
    float calibrated_power = this->get_calibrated_power(raw_power);
    this->power_sensor_->publish_state(calibrated_power);
  }
  if (this->current_sensor_) {
    uint16_t raw_current = sensor_reading.current[this->input_port_];
    double raw_current_d = (double) raw_current;
    double scalar;
    if (is_mains_port(this->input_port_)) {
      scalar = 775.0 / 42624.0;
    } else {
      scalar = 775.0 / 170496.0;
    }
    this->current_sensor_->publish_state(raw_current_d * scalar);
  }
}

float CTClampConfig::get_calibrated_power(int32_t raw_power) const {
  float calibration = this->phase_->get_calibration();

  float correction_factor = (this->input_port_ < 3) ? 5.5 : 22;

  return (raw_power * calibration) / correction_factor;
}

}  // namespace emporia_vue
}  // namespace esphome
