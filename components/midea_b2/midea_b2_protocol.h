#pragma once

// The ACiQ / Midea 0xB2 IR protocol, both directions. Deliberately free of ESPHome headers so
// tests/test_protocol.cpp can build it on a PC against the captures in tests/captures.json.
//
// A press is up to three 48-bit frames, each behind a 4590/4590 us header:
//
//   STATE    B2 <fan band|1F> <temp gray|mode>   x2, every byte followed by its complement
//   TRAILER  D5 <fan %> <half> <unit> 00 <sum>   once, plain bytes, checksum = sum of first 5
//   OFF      B2 7B E0                            x2, no trailer
//   COMMAND  B9 F5 <code>                        x2, no trailer (turbo, swing)
//
// The trailer carries what the state frame cannot: the exact fan percentage (the state frame
// has a 4-value band), the half degree (the wire is Celsius on a 0.5 grid), and the display
// unit. Leave it off and the unit's display falls back to Celsius.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::midea_b2::protocol {

static const uint32_t CARRIER_HZ = 38000;
static const uint32_t HEADER_MARK = 4590;
static const uint32_t HEADER_SPACE = 4590;
static const uint32_t BIT_MARK = 541;
static const uint32_t ZERO_SPACE = 541;
static const uint32_t ONE_SPACE = 1613;
static const uint32_t GAP = 4590;

// The wire carries 17-30 C. The handset shows 60 F, but 60-62 F all transmit 17 C.
static const float MIN_TEMP_C = 17.0f;
static const float MAX_TEMP_C = 30.0f;

enum class Mode : uint8_t { OFF, COOL, HEAT, DRY, AUTO, FAN_ONLY };

enum class Command : uint8_t { TURBO_ON = 0x01, TURBO_OFF = 0x02, SWING_ON = 0x04, SWING_OFF = 0x05 };

static const uint8_t FAN_AUTO = 0;  // in State::fan; 1-100 is a literal percentage

struct State {
  Mode mode{Mode::OFF};
  float temp_c{22.0f};  // on the 0.5 grid once encoded; ignored for OFF and FAN_ONLY
  uint8_t fan{FAN_AUTO};
  bool fahrenheit{true};  // the unit's DISPLAY; the wire is Celsius either way
};

using Timings = std::vector<int32_t>;  // marks positive, spaces negative, microseconds

/// Clamp to 17-30 C and round to the half degree the wire can carry.
float snap_celsius(float temp_c);

/// A complete press: state x2 plus the trailer, or OFF x2. Returns false only for a fan
/// percentage outside 1-100.
bool encode_state(const State &state, Timings &out);
void encode_command(Command command, Timings &out);

struct Decoded {
  bool has_state{false};  // a valid B2 frame (OFF included)
  bool has_trailer{false};
  bool has_command{false};
  State state{};
  // Without a trailer the fan is only known to a band, and the half degree is unknown.
  Command command{Command::TURBO_OFF};
};

/// Parse every valid frame in one receive buffer. Frames that fail their complement or
/// checksum test are skipped, never guessed at. Returns false when nothing valid was found.
bool decode(const int32_t *timings, size_t count, Decoded &out);

}  // namespace esphome::midea_b2::protocol
