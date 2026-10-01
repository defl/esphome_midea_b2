#pragma once

#include "esphome/components/climate_ir/climate_ir.h"

#include "midea_b2_protocol.h"

namespace esphome::midea_b2 {

class MideaB2Climate : public climate_ir::ClimateIR {
 public:
  MideaB2Climate();

  void set_fahrenheit_display(bool fahrenheit) { this->fahrenheit_display_ = fahrenheit; }
  void dump_config() override;

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  void transmit_state() override;
  bool on_receive(remote_base::RemoteReceiveData data) override;

  void send_(const protocol::Timings &timings);
  uint8_t fan_percent_() const;

  bool fahrenheit_display_{true};
};

}  // namespace esphome::midea_b2
