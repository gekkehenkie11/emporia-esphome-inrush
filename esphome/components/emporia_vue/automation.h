#pragma once

#ifdef USE_ESP32

#include "emporia_vue.h"

#include "esphome/core/automation.h"

namespace esphome {
namespace emporia_vue {

// Trigger for after statistics sensors are updated
class EmporiaVueUpdateTrigger : public Trigger<> {
 public:
  explicit EmporiaVueUpdateTrigger(EmporiaVueComponent *parent) {
    parent->add_on_update_callback([this]() { this->trigger(); });
  }
};

// Trigger after a start has been captured. Variables: summary (JSON), trace (CSV, one line per cycle)
class EmporiaVueStartCapturedTrigger : public Trigger<std::string, std::string> {
 public:
  explicit EmporiaVueStartCapturedTrigger(CycleCaptureConfig *parent) {
    parent->add_on_start_callback(
        [this](std::string summary, std::string trace) { this->trigger(summary, trace); });
  }
};

}  // namespace emporia_vue
}  // namespace esphome

#endif  // ifdef USE_ESP32
