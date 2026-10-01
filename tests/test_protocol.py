"""Check the C++ protocol against every capture from the physical handset.

Builds tests/protocol_cli.cpp with the host compiler ($CXX, default g++) and runs it:

  encode   every labelled state must reproduce its capture bit for bit, trailer included
  decode   every capture must decode back to its label; ECO/GEAR must decode to nothing

Run:  python -m pytest tests
"""

from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess

import pytest

HERE = pathlib.Path(__file__).parent
CAPTURES = json.loads((HERE / "captures.json").read_text(encoding="utf-8"))["captures"]

# cool_60's trailer carries byte3 bit 4 (0x11, not 0x01). 62 F also clamps to 17 C and sent
# 0x01, so clamping is not the trigger. One sample, unexplained, deliberately not generated.
KNOWN_ENCODE_MISMATCH = {"cool_60"}

LOCKED_FAN_MODES = {"auto", "dry"}


@pytest.fixture(scope="session")
def cli(tmp_path_factory) -> pathlib.Path:
    cxx = os.environ.get("CXX") or shutil.which("g++")
    if not cxx:
        pytest.fail("no C++ compiler: set CXX or put g++ on PATH")
    exe = tmp_path_factory.mktemp("build") / "protocol_cli.exe"
    subprocess.run(
        [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-O1",
            "-o",
            str(exe),
            str(HERE / "protocol_cli.cpp"),
            str(HERE.parent / "components/midea_b2/midea_b2_protocol.cpp"),
        ],
        check=True,
    )
    return exe


def run(cli: pathlib.Path, *args: str, stdin: str = "") -> str:
    result = subprocess.run(
        [str(cli), *args], input=stdin, capture_output=True, text=True, check=True
    )
    return result.stdout.strip()


def bitstream(timings: list[int]) -> list[int]:
    """Every data bit, headers and gaps skipped, frames concatenated."""
    values = [abs(v) for v in timings]
    bits: list[int] = []
    index = 0
    while index < len(values) - 1:
        mark, space = values[index], values[index + 1]
        if mark > 3000:
            index += 2 if space > 3000 else 1
            continue
        bits.append(1 if space > 1000 else 0)
        index += 2
    return bits


def celsius(temp_f: float) -> float:
    return (temp_f - 32) * 5 / 9


def expected_fan(mode: str, fan: str) -> int:
    return 0 if fan == "auto" or mode in LOCKED_FAN_MODES else int(fan)


def snap(temp_c: float) -> float:
    return max(17.0, min(30.0, round(temp_c * 2) / 2))


STATE_CAPTURES = [c for c in CAPTURES if "state" in c]
COMMAND_CAPTURES = [c for c in CAPTURES if "command" in c]
OTHER_CAPTURES = [c for c in CAPTURES if "state" not in c and "command" not in c]


def ids(captures):
    return [c["label"] for c in captures]


@pytest.mark.parametrize("capture", STATE_CAPTURES, ids=ids(STATE_CAPTURES))
def test_encode_state(cli, capture):
    s = capture["state"]
    temp_c = celsius(s["temp_f"]) if s["temp_f"] is not None else 22.0
    mine = run(
        cli,
        "encode",
        s["mode"],
        f"{temp_c:.6f}",
        str(0 if s["fan"] == "auto" else int(s["fan"])),
        "1" if s["fahrenheit"] else "0",
    )
    same = bitstream([int(v) for v in mine.split()]) == bitstream(capture["timings"])
    if capture["label"] in KNOWN_ENCODE_MISMATCH:
        assert not same, "known mismatch now matches: drop it from KNOWN_ENCODE_MISMATCH"
    else:
        assert same


@pytest.mark.parametrize("capture", COMMAND_CAPTURES, ids=ids(COMMAND_CAPTURES))
def test_encode_command(cli, capture):
    mine = run(cli, "command", capture["command"])
    assert bitstream([int(v) for v in mine.split()]) == bitstream(capture["timings"])


def decode(cli, timings: list[int]) -> dict[str, str]:
    line = run(cli, "decode", stdin=" ".join(map(str, timings)))
    return dict(field.split("=") for field in line.split())


@pytest.mark.parametrize("capture", STATE_CAPTURES, ids=ids(STATE_CAPTURES))
def test_decode_state(cli, capture):
    s = capture["state"]
    d = decode(cli, capture["timings"])
    assert d["state"] == "1"
    assert d["mode"] == s["mode"]
    if s["mode"] == "off":
        assert d["trailer"] == "0"
        return
    assert d["trailer"] == "1"
    assert int(d["fan"]) == expected_fan(s["mode"], s["fan"])
    assert d["fahrenheit"] == ("1" if s["fahrenheit"] else "0")
    if s["mode"] != "fan_only":
        assert float(d["temp_c"]) == snap(celsius(s["temp_f"]))


@pytest.mark.parametrize("capture", COMMAND_CAPTURES, ids=ids(COMMAND_CAPTURES))
def test_decode_command(cli, capture):
    d = decode(cli, capture["timings"])
    codes = {"turbo_on": 1, "turbo_off": 2, "swing_on": 4, "swing_off": 5}
    assert (d["state"], d["command"]) == ("0", "1")
    assert int(d["cmd"]) == codes[capture["command"]]


@pytest.mark.parametrize("capture", OTHER_CAPTURES, ids=ids(OTHER_CAPTURES))
def test_decode_ignores_eco_gear(cli, capture):
    d = decode(cli, capture["timings"])
    assert (d["state"], d["command"]) == ("0", "0")


@pytest.mark.parametrize("capture", STATE_CAPTURES, ids=ids(STATE_CAPTURES))
def test_decode_rejects_a_flipped_bit(cli, capture):
    """One corrupted bit in each state frame must not produce a different state."""
    timings = list(capture["timings"])
    for start in (0, 100):  # the two state frames; 99 entries each plus the gap
        index = start + 2 + 1  # first data bit's space
        timings[index] = -1613 if abs(timings[index]) < 1000 else -541
    d = decode(cli, timings)
    assert d["state"] == "0"
