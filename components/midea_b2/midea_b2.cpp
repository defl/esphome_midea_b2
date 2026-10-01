#include "midea_b2.h"

#include <cmath>
#include <cstdlib>

#include "esphome/core/log.h"

namespace esphome::midea_b2 {

static const char *const TAG = "midea_b2.climate";

// The steps the handset's fan button walks through. The wire takes any 1-100, but a climate
// entity can only report a mode it lists, so a received percentage lands on the nearest rung.
static const char *const FAN_RUNGS[] = {"20", "40", "60", "80", "100"};
// The handset's name for it, rather than ESPHome's BOOST.
static const char *const PRESET_TURBO = "turbo";
static const char *const CUSTOM_PRESETS[] = {PRESET_TURBO};

MideaB2Climate::MideaB2Climate()
    : climate_ir::ClimateIR(protocol::MIN_TEMP_C, protocol::MAX_TEMP_C, 0.5f, true, true,
                            {climate::CLIMATE_FAN_AUTO}, {climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL},
                            {climate::CLIMATE_PRESET_NONE}) {
  this->set_supported_custom_fan_modes(FAN_RUNGS);
  this->set_supported_custom_presets(CUSTOM_PRESETS);
}

climate::ClimateTraits MideaB2Climate::traits() {
  auto traits = climate_ir::ClimateIR::traits();
  // The unit's AUTO picks heat or cool itself with one setpoint: HA's "auto", not "heat_cool".
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_COOL, climate::CLIMATE_MODE_HEAT,
                              climate::CLIMATE_MODE_DRY, climate::CLIMATE_MODE_FAN_ONLY, climate::CLIMATE_MODE_AUTO});
  return traits;
}

void MideaB2Climate::control(const climate::ClimateCall &call) {
  bool state_changed = false;
  if (call.get_mode().has_value()) {
    this->mode = *call.get_mode();
    state_changed = true;
  }
  if (call.get_target_temperature().has_value()) {
    this->target_temperature = *call.get_target_temperature();
    state_changed = true;
  }
  if (call.get_fan_mode().has_value()) {
    this->set_fan_mode_(*call.get_fan_mode());
    state_changed = true;
  } else if (call.has_custom_fan_mode()) {
    this->set_custom_fan_mode_(call.get_custom_fan_mode());
    state_changed = true;
  }

  // While off, a new setpoint or fan speed is only remembered: sending it would switch the
  // unit on, because every state frame except OFF is an on frame.
  if (state_changed && (call.get_mode().has_value() || this->mode != climate::CLIMATE_MODE_OFF))
    this->transmit_state();

  // Swing and turbo are separate command frames, not bits in the state frame.
  protocol::Timings timings;
  if (call.get_swing_mode().has_value()) {
    this->swing_mode = *call.get_swing_mode();
    protocol::encode_command(this->swing_mode == climate::CLIMATE_SWING_OFF ? protocol::Command::SWING_OFF
                                                                             : protocol::Command::SWING_ON,
                             timings);
    this->send_(timings);
  }
  if (call.has_custom_preset()) {
    this->set_custom_preset_(call.get_custom_preset());
    protocol::encode_command(protocol::Command::TURBO_ON, timings);
    this->send_(timings);
  } else if (call.get_preset().has_value()) {
    this->set_preset_(*call.get_preset());
    protocol::encode_command(protocol::Command::TURBO_OFF, timings);
    this->send_(timings);
  }
  this->publish_state();
}

uint8_t MideaB2Climate::fan_percent_() const {
  if (!this->has_custom_fan_mode())
    return protocol::FAN_AUTO;
  return uint8_t(std::atoi(this->get_custom_fan_mode().c_str()));
}

void MideaB2Climate::transmit_state() {
  protocol::State state;
  switch (this->mode) {
    case climate::CLIMATE_MODE_COOL:
      state.mode = protocol::Mode::COOL;
      break;
    case climate::CLIMATE_MODE_HEAT:
      state.mode = protocol::Mode::HEAT;
      break;
    case climate::CLIMATE_MODE_DRY:
      state.mode = protocol::Mode::DRY;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      state.mode = protocol::Mode::FAN_ONLY;
      break;
    case climate::CLIMATE_MODE_AUTO:
    case climate::CLIMATE_MODE_HEAT_COOL:
      state.mode = protocol::Mode::AUTO;
      break;
    default:
      state.mode = protocol::Mode::OFF;
      break;
  }
  state.temp_c = this->target_temperature;
  state.fan = this->fan_percent_();
  state.fahrenheit = this->fahrenheit_display_;

  protocol::Timings timings;
  if (!protocol::encode_state(state, timings)) {
    ESP_LOGW(TAG, "Cannot encode fan %u%%", state.fan);
    return;
  }
  this->send_(timings);
}

void MideaB2Climate::send_(const protocol::Timings &timings) {
  auto transmit = this->transmitter_->transmit();
  auto *data = transmit.get_data();
  data->set_carrier_frequency(protocol::CARRIER_HZ);
  data->set_data(timings);
  transmit.perform();
}

bool MideaB2Climate::on_receive(remote_base::RemoteReceiveData data) {
  const auto &raw = data.get_raw_data();
  protocol::Decoded decoded;
  if (!protocol::decode(raw.data(), raw.size(), decoded))
    return false;

  if (decoded.has_state) {
    const auto &s = decoded.state;
    switch (s.mode) {
      case protocol::Mode::COOL:
        this->mode = climate::CLIMATE_MODE_COOL;
        break;
      case protocol::Mode::HEAT:
        this->mode = climate::CLIMATE_MODE_HEAT;
        break;
      case protocol::Mode::DRY:
        this->mode = climate::CLIMATE_MODE_DRY;
        break;
      case protocol::Mode::AUTO:
        this->mode = climate::CLIMATE_MODE_AUTO;
        break;
      case protocol::Mode::FAN_ONLY:
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case protocol::Mode::OFF:
        this->mode = climate::CLIMATE_MODE_OFF;
        break;
    }

    if (s.mode != protocol::Mode::OFF && s.mode != protocol::Mode::FAN_ONLY) {
      if (s.fahrenheit) {
        // Publish the whole degree the handset shows, not the half-Celsius step it sends:
        // 23.5 C would otherwise read 74.3 F in Home Assistant.
        const float shown_f = std::round(s.temp_c * 1.8f + 32.0f);
        this->target_temperature = (shown_f - 32.0f) / 1.8f;
      } else {
        this->target_temperature = s.temp_c;
      }
    }

    // AUTO and DRY lock the fan, so the frame says nothing about the setting to return to.
    if (s.mode == protocol::Mode::COOL || s.mode == protocol::Mode::HEAT || s.mode == protocol::Mode::FAN_ONLY) {
      if (s.fan == protocol::FAN_AUTO) {
        this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
      } else {
        int rung = int(std::lround(s.fan / 20.0f)) - 1;
        rung = rung < 0 ? 0 : (rung > 4 ? 4 : rung);
        this->set_custom_fan_mode_(FAN_RUNGS[rung]);
      }
    }
  }

  if (decoded.has_command) {
    switch (decoded.command) {
      case protocol::Command::SWING_ON:
        this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
        break;
      case protocol::Command::SWING_OFF:
        this->swing_mode = climate::CLIMATE_SWING_OFF;
        break;
      case protocol::Command::TURBO_ON:
        this->set_custom_preset_(PRESET_TURBO);
        break;
      case protocol::Command::TURBO_OFF:
        this->set_preset_(climate::CLIMATE_PRESET_NONE);
        break;
    }
  }

  ESP_LOGD(TAG, "Received: state=%s trailer=%s command=%s", YESNO(decoded.has_state), YESNO(decoded.has_trailer),
           YESNO(decoded.has_command));
  this->publish_state();
  return true;
}

void MideaB2Climate::dump_config() {
  climate_ir::ClimateIR::dump_config();
  ESP_LOGCONFIG(TAG, "  Fahrenheit display: %s", YESNO(this->fahrenheit_display_));
}

}  // namespace esphome::midea_b2
