#pragma once

#ifdef USE_ESP32

#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"

#include "cycle_capture.h"

namespace esphome {
namespace emporia_vue {

struct __attribute__((__packed__)) ReadingPowerEntry {
  int32_t phase_black;
  int32_t phase_red;
  int32_t phase_blue;
};

struct __attribute__((__packed__)) SensorReading {
  bool is_unread;
  uint8_t checksum;
  uint8_t unknown;
  uint8_t sequence_num;

  ReadingPowerEntry power[19];

  uint16_t voltage[3];
  uint16_t frequency;
  uint16_t degrees[2];

  uint16_t current[19];

  uint16_t end;
};

class PhaseConfig;
class CTClampConfig;
class CycleCaptureConfig;

class EmporiaVueComponent : public PollingComponent, public i2c::I2CDevice {
 public:
  void dump_config() override;

  float get_setup_priority() const override { return esphome::setup_priority::HARDWARE; }

  void set_phases(std::vector<PhaseConfig *> phases) { this->phases_ = std::move(phases); }
  void set_ct_clamps(std::vector<CTClampConfig *> ct_clamps) { this->ct_clamps_ = std::move(ct_clamps); }
  void set_cycle_capture(CycleCaptureConfig *cycle_capture) { this->cycle_capture_ = cycle_capture; }

  void setup() override;
  void loop() override;
  void update() override;

  void add_on_update_callback(std::function<void()> &&callback);

 protected:
  void cycle_capture_poll_();
  void cycle_capture_publish_start_();

  uint8_t last_sequence_num_ = 0;
  std::vector<PhaseConfig *> phases_;
  std::vector<CTClampConfig *> ct_clamps_;
  CallbackManager<void()> callback_;
  CycleCaptureConfig *cycle_capture_{nullptr};
};

enum PhaseInputWire : uint8_t {
  BLACK = 0,
  RED = 1,
  BLUE = 2,
};

class PhaseConfig {
 public:
  void set_input_wire(PhaseInputWire input_wire) { this->input_wire_ = input_wire; }
  PhaseInputWire get_input_wire() const { return this->input_wire_; }
  void set_calibration(float calibration) { this->calibration_ = calibration; }
  float get_calibration() const { return this->calibration_; }
  void set_voltage_sensor(sensor::Sensor *voltage_sensor) { this->voltage_sensor_ = voltage_sensor; }
  sensor::Sensor *get_voltage_sensor() const { return this->voltage_sensor_; }
  void set_frequency_sensor(sensor::Sensor *frequency_sensor) { this->frequency_sensor_ = frequency_sensor; }
  sensor::Sensor *get_frequency_sensor() const { return this->frequency_sensor_; }
  void set_phase_angle_sensor(sensor::Sensor *phase_angle_sensor) { this->phase_angle_sensor_ = phase_angle_sensor; }
  sensor::Sensor *get_phase_angle_sensor() const { return this->phase_angle_sensor_; }

  void update_from_reading(const SensorReading &sensor_reading);

  int32_t extract_power_for_phase(const ReadingPowerEntry &power_entry);

 protected:
  PhaseInputWire input_wire_;
  float calibration_;
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *frequency_sensor_{nullptr};
  sensor::Sensor *phase_angle_sensor_{nullptr};
};

enum CTInputPort : uint8_t {
#if defined(EMPORIA_VUE_VARIANT_VUE3)
  A = 2,
  B = 1,
  C = 0,
#else
  A = 0,
  B = 1,
  C = 2,
#endif
  ONE = 3,
  TWO = 4,
  THREE = 5,
  FOUR = 6,
  FIVE = 7,
  SIX = 8,
  SEVEN = 9,
  EIGHT = 10,
  NINE = 11,
  TEN = 12,
  ELEVEN = 13,
  TWELVE = 14,
  THIRTEEN = 15,
  FOURTEEN = 16,
  FIFTEEN = 17,
  SIXTEEN = 18,
};

class CTClampConfig : public sensor::Sensor {
 public:
  void set_phase(PhaseConfig *phase) { this->phase_ = phase; };
  const PhaseConfig *get_phase() const { return this->phase_; }
  void set_input_port(CTInputPort input_port) { this->input_port_ = input_port; };
  CTInputPort get_input_port() const { return this->input_port_; }
  void set_power_sensor(sensor::Sensor *power_sensor) { this->power_sensor_ = power_sensor; }
  sensor::Sensor *get_power_sensor() const { return this->power_sensor_; }
  void set_current_sensor(sensor::Sensor *current_sensor) { this->current_sensor_ = current_sensor; }
  sensor::Sensor *get_current_sensor() const { return this->current_sensor_; }

  void update_from_reading(const SensorReading &sensor_reading);
  float get_calibrated_power(int32_t raw_power) const;

 protected:
  PhaseConfig *phase_;
  CTInputPort input_port_;
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *current_sensor_{nullptr};
};

// Per-cycle capture of starts (needs the cycle-capture SAMD09 firmware)
class CycleCaptureConfig {
 public:
  void set_poll_interval(uint32_t ms) { this->poll_interval_ms_ = ms; }
  void set_live_interval(uint32_t ms) { this->live_interval_ms_ = ms; }
  void set_trigger_current(float a) { this->analyzer_.set_trigger_current(a); }
  void set_rearm_current(float a) { this->analyzer_.set_rearm_current(a); }
  void set_pre_trigger(uint32_t ms) { this->analyzer_.set_pre_trigger(ms / 1000.0f); }
  void set_post_trigger(uint32_t ms) { this->analyzer_.set_post_trigger(ms / 1000.0f); }
  void set_settle_factor(float f) { this->analyzer_.set_settle_factor(f); }
  void set_mains_current_multiplier(float m) { this->mains_multiplier_ = m; }
  void set_aux_current_multiplier(float m) { this->aux_multiplier_ = m; }
  void set_aux_input(uint8_t index, uint8_t port) { this->aux_port_[index] = port; }
  void set_log_trace(bool log_trace) { this->log_trace_ = log_trace; }
  // Fixed SAMD09 ADC frame rate. Default 0 = measure it against the ESP32 clock.
  void set_frame_rate(float fr) { this->fixed_frame_rate_ = fr; }

  void set_max_current_sensor(sensor::Sensor *s) { this->max_current_sensor_ = s; }
  void set_min_voltage_sensor(sensor::Sensor *s) { this->min_voltage_sensor_ = s; }
  void set_min_frequency_sensor(sensor::Sensor *s) { this->min_frequency_sensor_ = s; }
  void set_max_frequency_sensor(sensor::Sensor *s) { this->max_frequency_sensor_ = s; }
  void set_start_peak_current_sensor(sensor::Sensor *s) { this->start_peak_current_sensor_ = s; }
  void set_start_inrush_100ms_sensor(sensor::Sensor *s) { this->start_inrush_100ms_sensor_ = s; }
  void set_start_peak_instant_sensor(sensor::Sensor *s) { this->start_peak_instant_sensor_ = s; }
  void set_start_running_current_sensor(sensor::Sensor *s) { this->start_running_current_sensor_ = s; }
  void set_start_duration_sensor(sensor::Sensor *s) { this->start_duration_sensor_ = s; }
  void set_start_voltage_min_sensor(sensor::Sensor *s) { this->start_voltage_min_sensor_ = s; }
  void set_start_voltage_sag_sensor(sensor::Sensor *s) { this->start_voltage_sag_sensor_ = s; }
  void set_start_clipped_sensor(sensor::Sensor *s) { this->start_clipped_sensor_ = s; }
  void set_start_count_sensor(sensor::Sensor *s) { this->start_count_sensor_ = s; }

  void add_on_start_callback(std::function<void(std::string, std::string)> &&callback) {
    this->start_callback_.add(std::move(callback));
    this->has_start_callback_ = true;
  }

 protected:
  friend class EmporiaVueComponent;

  StartAnalyzer analyzer_;
  uint32_t poll_interval_ms_{50};
  uint32_t live_interval_ms_{1000};
  float mains_multiplier_{1.0f};
  float aux_multiplier_{1.0f};
  uint8_t aux_port_[2]{0xFF, 0xFF};
  bool log_trace_{true};

  sensor::Sensor *max_current_sensor_{nullptr};
  sensor::Sensor *min_voltage_sensor_{nullptr};
  sensor::Sensor *min_frequency_sensor_{nullptr};
  sensor::Sensor *max_frequency_sensor_{nullptr};
  sensor::Sensor *start_peak_current_sensor_{nullptr};
  sensor::Sensor *start_inrush_100ms_sensor_{nullptr};
  sensor::Sensor *start_peak_instant_sensor_{nullptr};
  sensor::Sensor *start_running_current_sensor_{nullptr};
  sensor::Sensor *start_duration_sensor_{nullptr};
  sensor::Sensor *start_voltage_min_sensor_{nullptr};
  sensor::Sensor *start_voltage_sag_sensor_{nullptr};
  sensor::Sensor *start_clipped_sensor_{nullptr};
  sensor::Sensor *start_count_sensor_{nullptr};

  CallbackManager<void(std::string, std::string)> start_callback_;
  bool has_start_callback_{false};

  // runtime state
  bool initialized_{false};
  bool disabled_{false};
  uint16_t probe_failures_{0};
  uint32_t last_poll_{0};
  uint32_t last_live_{0};
  uint32_t last_error_log_{0};
  uint8_t last_lost_{0};
  bool have_lost_{false};
  uint32_t bad_checksums_{0};
  float fixed_frame_rate_{0};
  // frame rate calibration: frames received over >= 60 s of ESP32 time
  uint32_t cal_start_ms_{0};
  uint32_t cal_frames_{0};
  uint32_t cal_gaps_ref_{0};
  uint32_t cal_lost_ref_{0};
  uint32_t lost_total_{0};
  float measured_frame_rate_{0};
  size_t pending_pos_{0};    // next trace line to log
  size_t pending_count_{0};  // 0 = nothing to log
  uint32_t pending_number_{0};
};

}  // namespace emporia_vue
}  // namespace esphome

#endif  // ifdef USE_ESP32
