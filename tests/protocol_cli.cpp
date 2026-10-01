// Host-side driver for tests/test_protocol.py. Not part of the component.
//
//   protocol_cli encode <off|cool|heat|dry|auto|fan_only> <temp_c> <fan 0-100> <fahrenheit 0|1>
//   protocol_cli command <turbo_on|turbo_off|swing_on|swing_off>
//   protocol_cli decode            (timings on stdin)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "../components/midea_b2/midea_b2_protocol.h"

using namespace esphome::midea_b2::protocol;

static const char *const MODE_NAMES[] = {"off", "cool", "heat", "dry", "auto", "fan_only"};

static void print(const Timings &t) {
  for (size_t i = 0; i < t.size(); i++)
    std::printf(i ? " %d" : "%d", int(t[i]));
  std::printf("\n");
}

int main(int argc, char **argv) {
  if (argc >= 6 && std::strcmp(argv[1], "encode") == 0) {
    State state;
    bool known = false;
    for (int i = 0; i < 6; i++) {
      if (std::strcmp(argv[2], MODE_NAMES[i]) == 0) {
        state.mode = Mode(i);
        known = true;
      }
    }
    if (!known)
      return 2;
    state.temp_c = float(std::atof(argv[3]));
    state.fan = uint8_t(std::atoi(argv[4]));
    state.fahrenheit = std::atoi(argv[5]) != 0;
    Timings t;
    if (!encode_state(state, t))
      return 3;
    print(t);
    return 0;
  }
  if (argc >= 3 && std::strcmp(argv[1], "command") == 0) {
    Command command;
    if (std::strcmp(argv[2], "turbo_on") == 0)
      command = Command::TURBO_ON;
    else if (std::strcmp(argv[2], "turbo_off") == 0)
      command = Command::TURBO_OFF;
    else if (std::strcmp(argv[2], "swing_on") == 0)
      command = Command::SWING_ON;
    else if (std::strcmp(argv[2], "swing_off") == 0)
      command = Command::SWING_OFF;
    else
      return 2;
    Timings t;
    encode_command(command, t);
    print(t);
    return 0;
  }
  if (argc >= 2 && std::strcmp(argv[1], "decode") == 0) {
    Timings t;
    int value;
    while (std::cin >> value)
      t.push_back(value);
    Decoded d;
    decode(t.data(), t.size(), d);
    std::printf("state=%d trailer=%d command=%d mode=%s temp_c=%.1f fan=%d fahrenheit=%d cmd=%d\n",
                int(d.has_state), int(d.has_trailer), int(d.has_command), MODE_NAMES[int(d.state.mode)],
                double(d.state.temp_c), int(d.state.fan), int(d.state.fahrenheit), int(d.command));
    return 0;
  }
  return 2;
}
