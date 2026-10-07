# Midea 0xB2 IR climate for ESPHome

ESPHome external component that drives an **ACiQ / Blueridge / Midea mini-split over IR** —
the variant with the `RG10R(M2S)/BGEFU1` handset — and **reads that handset back**, so the
climate entity follows the unit when someone uses the remote.

**Status: working.** In daily use on one ACiQ unit since 2026-10-07, driven from a KinCony
KC868-AGv3; the protocol is verified against 35 captures from that unit's handset.

> **Not ESPHome's `midea_ir`.** That component speaks a 6-byte, `0xA1`-typed frame with a
> checksum. This unit speaks a 3-byte, complement-paired, `0xB2` frame, and ignores `midea_ir`
> completely. They share only the header timing, which makes the mismatch easy to miss.

**Sibling project:** [zha-midea-ir](https://github.com/defl/zha-midea-ir) is a Home Assistant
integration for the same protocol through a Tuya TS1201 Zigbee blaster. It is a separate,
independent implementation; nothing is shared between the two.

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/defl/esphome_midea_b2
      ref: main
    components: [midea_b2]
    refresh: 0s
```

## Configuration

```yaml
remote_transmitter:
  id: ir_tx
  pin: GPIO47
  carrier_duty_percent: 50%
  # A state change and a swing/turbo command go out back to back; never overlap them.
  non_blocking: false

remote_receiver:
  id: ir_rx
  pin:
    number: GPIO1
    inverted: true
  idle: 10ms          # must exceed the ~5 ms gap between frames; see below

climate:
  - platform: midea_b2
    name: "AC"
    transmitter_id: ir_tx
    receiver_id: ir_rx          # optional: follow the handset
    sensor: room_temperature    # optional: current temperature
    fahrenheit_display: true
```

| Option | Default | |
|---|---|---|
| `transmitter_id` | required | a `remote_transmitter` with an IR LED |
| `receiver_id` | — | a `remote_receiver`; without it the entity only knows what it sent |
| `sensor` / `humidity_sensor` | — | current temperature / humidity, in °C |
| `fahrenheit_display` | `true` | what the unit's own display shows; the wire is Celsius either way |

Plus every standard [climate](https://esphome.io/components/climate/) option.
[example.yaml](example.yaml) is a complete device file for a KinCony KC868-AGv3.

**Set the receiver's `idle` above 5 ms.** A press is up to three frames separated by gaps of
4.4–5.3 ms. With a shorter `idle` the trailer frame arrives in a buffer of its own, and the
fan percentage, half degree and display unit it carries are lost.

**In a °F Home Assistant, add a `visual:` block.** The component's setpoint step is 0.5 °C.
Home Assistant converts the temperatures to °F but applies the step as is, so the entity
offers 0.5 °F steps and a 62.5 °F floor. These settings give whole-degree steps over the
63–86 °F range the handset shows:

```yaml
climate:
  - platform: midea_b2
    # ...
    visual:
      min_temperature: 17.2222   # 63 °F
      max_temperature: 30        # 86 °F
      temperature_step:
        target_temperature: 1
        current_temperature: 0.1
```

## What the entity offers

| | |
|---|---|
| Modes | off, cool, heat, dry, auto, fan_only |
| Temperature | 17–30 °C on a 0.5 °C grid (63–86 °F) |
| Fan | auto, 20, 40, 60, 80, 100 (%) |
| Swing | off, vertical |
| Preset | none, turbo |

Changing the setpoint or fan while the unit is off only updates the entity. Sending it would
turn the unit on, because every state frame except OFF is an on frame.

## The protocol

A press is up to three 48-bit frames, MSB first, each behind a 4590/4590 µs header. Bits are a
541 µs mark followed by a 541 µs (0) or 1613 µs (1) space. Carrier 38 kHz.

```
STATE    B2 <band|1F> <temp|mode>      x2, every byte followed by its complement
TRAILER  D5 <fan%> <half> <unit> 00 <sum>   once, plain bytes; sum of the first five
OFF      B2 7B E0                      x2, no trailer
COMMAND  B9 F5 <code>                  x2, no trailer — turbo 01/02, swing 04/05
```

- **Temperature** is a Midea Gray code in the high nibble of the third byte, whole °C, 17–30.
  The handset shows 60 °F, but 60–62 °F all transmit 17 °C.
- **Mode** is the low nibble: cool `0`, dry `4`, auto `8`, heat `C`. Fan-only is dry's nibble
  with a sentinel `E` where the temperature would be.
- **Fan band** is the top three bits of the second byte: auto `101`, ≤20 % `111`, ≤40 % `100`,
  ≤60 % `010`, above `001`. Auto and dry send `000`, because the unit locks the fan in those
  modes.
- **The trailer is not optional.** It is the only place the exact fan percentage (byte 1;
  `0x66` auto, `0x65` locked), the half degree (byte 2, `0x20`) and the display unit (byte 3,
  bit 0 set = °F) are carried. Without it, the unit's display falls back to Celsius.
- **ECO/GEAR is not supported.** Its button toggles between two codes (`B9 F5 24/25`) and no
  frame says which mode results, so a control for it would misreport what it does. The
  decoder ignores those frames.

**Decoding** checks every frame: B2 and B9 frames must pair each byte with its complement, and
the trailer must match its checksum. A frame that fails is dropped, never guessed at. If the
handset is set to °F, the received setpoint is published as the whole degree it shows, not as
the half-Celsius step it transmits.

## Tests

`tests/` holds every capture from the handset (`captures.json`) and checks the protocol code
against them on a PC:

```
CXX=g++ python -m pytest tests
```

- **encode** — every labelled state reproduces its capture bit for bit, trailer included.
- **decode** — every capture decodes back to its label, ECO/GEAR decodes to nothing, and a
  single corrupted bit never yields a state.

One capture, `cool_60`, is a known encode mismatch: its trailer sets byte 3 bit 4 (`0x11`).
62 °F also clamps to 17 °C and sent `0x01`, so clamping is not the trigger. With only one
sample, it is left unexplained rather than guessed at.

## Disclaimer

This project is not affiliated with, endorsed by or supported by Midea, ACiQ, Blueridge, Tuya
or KinCony. Their names appear only to identify the equipment this component was tested with.
Everything here was derived from observing a handset the author owns. The software is provided
"as is", without warranty of any kind; see [LICENSE](LICENSE).

## License

This project uses the same dual license as ESPHome:

- **MIT** — Python code and all other parts
- **GPL-3.0** — C++/runtime code (`.h`, `.cpp` files)

See [LICENSE](LICENSE) for the full text.
