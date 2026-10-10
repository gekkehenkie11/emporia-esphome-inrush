#pragma once

// Per-cycle capture for the Emporia Vue with the gekkehenkie11 SAMD09 firmware (cycle-capture branch).
//
// The SAMD09 produces one 32-byte record per mains cycle: RMS of the 3 voltages and 3 mains
// currents over that cycle, the peak sample of the mains currents, up to two selectable 50A
// CT inputs and clip flags. This file holds the protocol definitions and the start analysis.
// It has no ESPHome dependencies so it can be unit tested on a PC.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace emporia_vue {

// I2C commands understood by the cycle-capture firmware (slave 0x64)
static const uint8_t CYC_CMD_STATUS = 0xA0;  // then read CYC_STATUS_LEN bytes
static const uint8_t CYC_CMD_CYCLES = 0xA1;  // then read n * sizeof(CycleRecord) bytes
static const uint8_t CYC_CMD_CONFIG = 0xA2;  // followed by 2 bytes: aux port index 3..18, 0xFF = off
static const size_t CYC_STATUS_LEN = 12;

// Record flags
static const uint8_t CYCF_NO_ZEROCROSS = 0x01;  // closed by timeout, no zero crossing on voltage 1
static const uint8_t CYCF_AUX0_INVALID = 0x02;
static const uint8_t CYCF_FIRST = 0x04;  // first record after a SAMD09 reset
static const uint8_t CYCF_AUX1_INVALID = 0x08;
static const uint8_t CYCF_MERGED = 0x10;        // SAMD main loop was late, record spans more than one cycle
static const uint8_t CYCF_SPAN_INVALID = 0x20;  // should not happen

struct __attribute__((__packed__)) CycleRecord {
  uint16_t seq;
  uint16_t nsamples;   // ADC frames in this cycle
  uint16_t rms_v[3];   // by wire (BLACK, RED, BLUE), same scale as SensorReading.voltage
  uint16_t rms_i[3];   // mains, same scale and index as SensorReading.current[0..2]
  uint16_t peak_i[3];  // largest |sample| in ADC counts
  uint16_t rms_aux[2];
  uint8_t aux_port[2];
  uint8_t clip;  // bit0-2: mains current 0-2 reached the ADC limit
  uint8_t flags;
  uint8_t reserved;
  uint8_t checksum;
};
static_assert(sizeof(CycleRecord) == 32, "CycleRecord must be 32 bytes");

bool cycle_record_checksum_ok(const CycleRecord &r);

struct CycleStatus {
  bool valid{false};  // magic bytes present: firmware supports cycle capture
  uint8_t version{0};
  uint8_t record_size{0};
  uint8_t available{0};
  uint8_t lost{0};  // free running counter
  uint16_t frame_rate{0};
  uint8_t aux_port[2]{0xFF, 0xFF};
  uint8_t ring_size{0};
};

CycleStatus parse_cycle_status(const uint8_t *buf, size_t len);

// Scaling from firmware units to A / V. Same constants as the regular 0.5 s sensors.
struct CycleScaling {
  float current_per_raw[3]{0, 0, 0};  // by internal mains index (= SensorReading.current index)
  float aux_per_raw[2]{0, 0};
  float voltage_per_raw[3]{0, 0, 0};  // by wire, 0 = wire not configured
  float frame_rate{25974.0f};
  char mains_name[3]{'A', 'B', 'C'};  // port letter for internal mains index 0..2
};

struct CycleSample {
  uint16_t seq;
  float dt;      // s, duration of this cycle
  float i[3];    // A RMS, by internal mains index
  float ipk[3];  // A peak
  float v[3];    // V RMS, by wire
  float aux[2];  // A RMS
  uint8_t clip;
  uint8_t flags;
};

struct StartSummary {
  uint32_t number{0};       // 1, 2, 3 ... since boot
  uint16_t cycles{0};       // cycles in the trace
  uint16_t trigger_index{0};  // index of the onset cycle (t = 0): first cycle of the run above the re-arm level
  float peak_rms[3]{};      // highest one-cycle RMS current, by internal mains index
  float peak_instant[3]{};  // highest single sample, A
  float inrush_100ms[3]{};  // RMS over the first 100 ms from the onset (compare: Fluke inrush, +-1 cycle)
  float running[3]{};       // mean RMS over the last 0.5 s of the trace
  float duration{0};        // s from the onset until the current stays below the settle level
  bool settled{false};      // false: still above settle level at the end of the trace
  float v_before[3]{};      // mean V RMS before the onset, by wire
  float v_min[3]{};         // lowest one-cycle V RMS from the onset on, by wire
  float sag_pct[3]{};
  float f_before{0}, f_min{0}, f_max{0};
  uint8_t clipped_mask{0};  // mains that hit the ADC limit: peak values are then too low
  uint16_t gaps{0};         // missing records inside the trace
  uint16_t merged{0};
  // convenience: maximum over phases / minimum over wires
  float max_peak_rms() const;
  float max_peak_instant() const;
  float max_inrush_100ms() const;
  float max_running() const;
  float min_voltage(const CycleScaling &s) const;
  float max_sag(const CycleScaling &s) const;
};

class StartAnalyzer {
 public:
  void set_scaling(const CycleScaling &s) { this->scaling_ = s; }
  const CycleScaling &get_scaling() const { return this->scaling_; }
  void set_trigger_current(float a) { this->trigger_current_ = a; }
  void set_rearm_current(float a) { this->rearm_current_ = a; }
  void set_pre_trigger(float s) { this->pre_s_ = s; }
  void set_post_trigger(float s) { this->post_s_ = s; }
  void set_settle_factor(float f) { this->settle_factor_ = f; }

  // Feed records in order. Returns true when a start capture just completed.
  bool add(const CycleRecord &r);

  const StartSummary &summary() const { return this->summary_; }
  std::string summary_json() const;
  // Trace of the last capture as CSV: line 0 is the header, then one line per cycle.
  // Lines are generated on request so no text is kept in memory.
  size_t trace_line_count() const { return this->last_trace_.empty() ? 0 : this->last_trace_.size() + 1; }
  std::string trace_line(size_t index) const;
  std::string trace_csv() const;
  // Free the memory of the last trace (after it was logged / sent)
  void release_trace() {
    this->last_trace_.clear();
    this->last_trace_.shrink_to_fit();
  }

  // Live statistics since the previous take_*() call (NAN when no data)
  float take_max_current();
  float take_min_voltage();
  float take_min_frequency();
  float take_max_frequency();

  uint32_t records() const { return this->records_; }
  uint32_t gaps() const { return this->gaps_total_; }
  bool capturing() const { return this->capturing_; }

 protected:
  CycleSample convert_(const CycleRecord &r) const;
  float max_i_(const CycleSample &s) const;
  void finish_();
  std::vector<float> times_(const std::vector<CycleRecord> &tr, size_t onset) const;

  CycleScaling scaling_;
  float trigger_current_{100.0f};
  float rearm_current_{-1.0f};  // < 0: trigger / 2
  float pre_s_{0.5f};
  float post_s_{5.0f};
  float settle_factor_{1.3f};

  // raw records (32 bytes each) are kept, converted when needed: a 5 s trace is ~11 kB
  std::vector<CycleRecord> pre_;  // ring of recent cycles while idle
  size_t pre_pos_{0};
  std::vector<CycleRecord> trace_;
  bool capturing_{false};
  bool armed_{false};
  float armed_time_{0};
  float post_time_{0};
  bool have_last_seq_{false};
  uint16_t last_seq_{0};
  uint32_t records_{0};
  uint32_t gaps_total_{0};
  uint16_t trace_gaps_{0};
  StartSummary summary_;
  std::vector<CycleRecord> last_trace_;
  uint32_t number_{0};

  // live stats
  float live_max_i_{-1};
  float live_min_v_{-1};
  float live_min_f_{-1};
  float live_max_f_{-1};
};

}  // namespace emporia_vue
}  // namespace esphome
