#include "midea_b2_protocol.h"

#include <cmath>

namespace esphome::midea_b2::protocol {

namespace {

// Standard Midea Gray code for 17-30 C.
const uint8_t TEMP_GRAY[14] = {0x0, 0x1, 0x3, 0x2, 0x6, 0x7, 0x5, 0x4, 0xC, 0xD, 0x9, 0x8, 0xA, 0xB};

const uint8_t STATE_PREFIX = 0xB2;
const uint8_t POWER_OFF[3] = {0xB2, 0x7B, 0xE0};
const uint8_t COMMAND_PREFIX[2] = {0xB9, 0xF5};

// byte1 = (band << 5) | 0x1F. A band, not a speed: 1% and 20% both send 111, 80% and 100%
// both send 001. Only the trailer tells the ends of a band apart.
const uint8_t BYTE1_LOW = 0x1F;
const uint8_t FAN_BITS_AUTO = 0b101;
const uint8_t FAN_BITS_LOCKED = 0b000;  // AUTO and DRY: the unit ignores the fan button

const uint8_t MODE_COOL = 0x0;
const uint8_t MODE_DRY = 0x4;
const uint8_t MODE_AUTO = 0x8;
const uint8_t MODE_HEAT = 0xC;
// FAN-only reuses DRY's mode nibble with a sentinel where the temperature would be.
const uint8_t FAN_ONLY_TEMP = 0xE;

const uint8_t TRAILER_PREFIX = 0xD5;
const uint8_t TRAILER_FAN_AUTO = 0x66;
const uint8_t TRAILER_FAN_LOCKED = 0x65;
const uint8_t HALF_DEGREE_FLAG = 0x20;
const uint8_t FAHRENHEIT_FLAG = 0x01;

// Decode windows. Captures put headers at 4390-4630 and gaps up to 5290; a demodulating
// receiver stretches marks and shrinks spaces by ~100 us on top of that.
const int32_t HEADER_MIN = 3500;
const int32_t HEADER_MAX = 6500;
const int32_t BIT_MARK_MIN = 250;
const int32_t BIT_MARK_MAX = 900;
const int32_t SPACE_MIN = 250;
const int32_t SPACE_MAX = 2500;
const int32_t ONE_THRESHOLD = (int32_t(ZERO_SPACE) + int32_t(ONE_SPACE)) / 2;

bool locks_fan(Mode mode) { return mode == Mode::AUTO || mode == Mode::DRY; }

uint8_t fan_bits(uint8_t fan) {
  if (fan == FAN_AUTO)
    return FAN_BITS_AUTO;
  if (fan <= 20)
    return 0b111;
  if (fan <= 40)
    return 0b100;
  if (fan <= 60)
    return 0b010;
  return 0b001;
}

void append_frame(const uint8_t *bytes, size_t count, Timings &out) {
  if (!out.empty())
    out.push_back(-int32_t(GAP));
  out.push_back(HEADER_MARK);
  out.push_back(-int32_t(HEADER_SPACE));
  for (size_t i = 0; i < count; i++) {
    for (int shift = 7; shift >= 0; shift--) {
      out.push_back(BIT_MARK);
      out.push_back(-int32_t((bytes[i] >> shift) & 1 ? ONE_SPACE : ZERO_SPACE));
    }
  }
  out.push_back(BIT_MARK);
}

void append_paired(const uint8_t payload[3], Timings &out) {
  const uint8_t wire[6] = {payload[0], uint8_t(~payload[0]), payload[1], uint8_t(~payload[1]),
                           payload[2], uint8_t(~payload[2])};
  append_frame(wire, 6, out);
}

bool in_range(int32_t value, int32_t low, int32_t high) { return value >= low && value <= high; }

// Reads one 48-bit frame starting at a header. Advances `index` past it either way.
bool read_frame(const int32_t *t, size_t count, size_t &index, uint8_t bytes[6]) {
  if (index + 1 >= count || !in_range(t[index], HEADER_MIN, HEADER_MAX) ||
      !in_range(-t[index + 1], HEADER_MIN, HEADER_MAX)) {
    index++;
    return false;
  }
  index += 2;
  for (int b = 0; b < 6; b++) {
    uint8_t value = 0;
    for (int bit = 0; bit < 8; bit++) {
      if (index + 1 >= count || !in_range(t[index], BIT_MARK_MIN, BIT_MARK_MAX) ||
          !in_range(-t[index + 1], SPACE_MIN, SPACE_MAX))
        return false;
      value = uint8_t(value << 1) | (-t[index + 1] > ONE_THRESHOLD ? 1 : 0);
      index += 2;
    }
    bytes[b] = value;
  }
  // The closing mark; the receiver's idle cut-off may swallow the space after it.
  return index < count && in_range(t[index], BIT_MARK_MIN, BIT_MARK_MAX);
}

bool complement_paired(const uint8_t b[6]) {
  return uint8_t(b[0] ^ b[1]) == 0xFF && uint8_t(b[2] ^ b[3]) == 0xFF && uint8_t(b[4] ^ b[5]) == 0xFF;
}

bool decode_state(uint8_t byte1, uint8_t byte2, State &state) {
  if (byte1 == POWER_OFF[1] && byte2 == POWER_OFF[2]) {
    state.mode = Mode::OFF;
    return true;
  }
  if ((byte1 & 0x1F) != BYTE1_LOW)
    return false;

  const uint8_t bits = byte1 >> 5;
  const uint8_t temp = byte2 >> 4;
  const uint8_t mode = byte2 & 0x0F;

  if (mode == MODE_DRY && temp == FAN_ONLY_TEMP) {
    state.mode = Mode::FAN_ONLY;
  } else {
    switch (mode) {
      case MODE_COOL:
        state.mode = Mode::COOL;
        break;
      case MODE_DRY:
        state.mode = Mode::DRY;
        break;
      case MODE_AUTO:
        state.mode = Mode::AUTO;
        break;
      case MODE_HEAT:
        state.mode = Mode::HEAT;
        break;
      default:
        return false;
    }
    int index = -1;
    for (int i = 0; i < 14; i++) {
      if (TEMP_GRAY[i] == temp)
        index = i;
    }
    if (index < 0)
      return false;
    state.temp_c = MIN_TEMP_C + float(index);
  }

  // The band's top end; the trailer, when present, replaces it with the exact value.
  switch (bits) {
    case FAN_BITS_AUTO:
    case FAN_BITS_LOCKED:
      state.fan = FAN_AUTO;
      break;
    case 0b111:
      state.fan = 20;
      break;
    case 0b100:
      state.fan = 40;
      break;
    case 0b010:
      state.fan = 60;
      break;
    case 0b001:
      state.fan = 100;
      break;
    default:
      return false;
  }
  return true;
}

}  // namespace

float snap_celsius(float temp_c) {
  if (std::isnan(temp_c))
    temp_c = 22.0f;
  const float snapped = std::round(temp_c * 2.0f) / 2.0f;
  if (snapped < MIN_TEMP_C)
    return MIN_TEMP_C;
  if (snapped > MAX_TEMP_C)
    return MAX_TEMP_C;
  return snapped;
}

bool encode_state(const State &state, Timings &out) {
  out.clear();
  if (state.mode == Mode::OFF) {
    append_paired(POWER_OFF, out);
    append_paired(POWER_OFF, out);
    return true;
  }
  if (state.fan > 100)
    return false;

  const float temp = snap_celsius(state.temp_c);
  const int whole = int(temp);
  const bool half = temp - float(whole) >= 0.5f;

  uint8_t payload[3] = {STATE_PREFIX, 0, 0};
  if (state.mode == Mode::FAN_ONLY) {
    payload[1] = uint8_t(fan_bits(state.fan) << 5) | BYTE1_LOW;
    payload[2] = uint8_t(FAN_ONLY_TEMP << 4) | MODE_DRY;
  } else {
    uint8_t mode = MODE_COOL;
    switch (state.mode) {
      case Mode::HEAT:
        mode = MODE_HEAT;
        break;
      case Mode::DRY:
        mode = MODE_DRY;
        break;
      case Mode::AUTO:
        mode = MODE_AUTO;
        break;
      default:
        break;
    }
    const uint8_t bits = locks_fan(state.mode) ? FAN_BITS_LOCKED : fan_bits(state.fan);
    payload[1] = uint8_t(bits << 5) | BYTE1_LOW;
    payload[2] = uint8_t(TEMP_GRAY[whole - int(MIN_TEMP_C)] << 4) | mode;
  }

  uint8_t fan_byte = state.fan;
  if (locks_fan(state.mode)) {
    fan_byte = TRAILER_FAN_LOCKED;
  } else if (state.fan == FAN_AUTO) {
    fan_byte = TRAILER_FAN_AUTO;
  }
  uint8_t trailer[6] = {TRAILER_PREFIX, fan_byte, uint8_t(half ? HALF_DEGREE_FLAG : 0),
                        uint8_t(state.fahrenheit ? FAHRENHEIT_FLAG : 0), 0x00, 0};
  for (int i = 0; i < 5; i++)
    trailer[5] = uint8_t(trailer[5] + trailer[i]);

  append_paired(payload, out);
  append_paired(payload, out);
  append_frame(trailer, 6, out);
  return true;
}

void encode_command(Command command, Timings &out) {
  out.clear();
  const uint8_t payload[3] = {COMMAND_PREFIX[0], COMMAND_PREFIX[1], uint8_t(command)};
  append_paired(payload, out);
  append_paired(payload, out);
}

bool decode(const int32_t *timings, size_t count, Decoded &out) {
  out = Decoded{};
  State state{};
  bool locked = false;
  uint8_t trailer[6] = {0};

  size_t index = 0;
  while (index < count) {
    uint8_t b[6];
    if (!read_frame(timings, count, index, b))
      continue;

    if (b[0] == TRAILER_PREFIX) {
      uint8_t sum = 0;
      for (int i = 0; i < 5; i++)
        sum = uint8_t(sum + b[i]);
      if (sum != b[5])
        continue;
      for (int i = 0; i < 6; i++)
        trailer[i] = b[i];
      out.has_trailer = true;
      continue;
    }
    if (!complement_paired(b))
      continue;
    if (b[0] == STATE_PREFIX) {
      State decoded{};
      if (decode_state(b[2], b[4], decoded)) {
        state = decoded;
        locked = (b[2] >> 5) == FAN_BITS_LOCKED;
        out.has_state = true;
      }
    } else if (b[0] == COMMAND_PREFIX[0] && b[2] == COMMAND_PREFIX[1]) {
      switch (b[4]) {
        case uint8_t(Command::TURBO_ON):
        case uint8_t(Command::TURBO_OFF):
        case uint8_t(Command::SWING_ON):
        case uint8_t(Command::SWING_OFF):
          out.command = Command(b[4]);
          out.has_command = true;
          break;
        default:
          break;  // ECO/GEAR toggles between two codes; no frame says which mode results
      }
    }
  }

  if (out.has_state && out.has_trailer && state.mode != Mode::OFF) {
    const uint8_t fan = trailer[1];
    if (fan >= 1 && fan <= 100 && !locked)
      state.fan = fan;
    if ((trailer[2] & HALF_DEGREE_FLAG) && state.mode != Mode::FAN_ONLY)
      state.temp_c += 0.5f;
    state.fahrenheit = (trailer[3] & FAHRENHEIT_FLAG) != 0;
  }
  out.state = state;
  return out.has_state || out.has_command;
}

}  // namespace esphome::midea_b2::protocol
