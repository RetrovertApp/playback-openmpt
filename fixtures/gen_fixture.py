#!/usr/bin/env python3
"""Generate the self-authored playback fixture retrovert_selftest.mod.

A 4-channel ProTracker module built from scratch: one triangle-wave
sample and two 64-row patterns of an arpeggiated figure, ~15 s at
125 BPM / speed 6. Deterministic output — the committed fixture and its
sha256 in harness.toml must match what this script emits.
"""

import struct
from pathlib import Path

OUT = Path(__file__).parent / "retrovert_selftest.mod"

SAMPLE_LEN = 64  # bytes, one triangle cycle
PERIODS = {  # ProTracker periods, C-2..B-2 region
    "C-2": 428, "D-2": 381, "E-2": 339, "F-2": 320,
    "G-2": 285, "A-2": 254, "B-2": 226, "C-3": 214,
}


def sample_data():
    # Signed 8-bit triangle wave, full scale, one cycle per SAMPLE_LEN bytes.
    out = bytearray()
    q = SAMPLE_LEN // 4
    for i in range(SAMPLE_LEN):
        phase = i % SAMPLE_LEN
        if phase < q:
            v = phase * 127 // q
        elif phase < 3 * q:
            v = 127 - (phase - q) * 254 // (2 * q)
        else:
            v = -127 + (phase - 3 * q) * 127 // q
        out.append(v & 0xFF)
    return bytes(out)


def note(period, sample=1, effect=0, param=0):
    return struct.pack(
        ">BBBB",
        ((sample & 0xF0) | (period >> 8)) & 0xFF,
        period & 0xFF,
        ((sample & 0x0F) << 4) | effect,
        param,
    )


EMPTY = b"\x00\x00\x00\x00"


def pattern(base_notes):
    rows = []
    for row in range(64):
        cells = []
        for ch in range(4):
            if row % 8 == ch * 2:
                name = base_notes[(row // 8 + ch) % len(base_notes)]
                cells.append(note(PERIODS[name]))
            else:
                cells.append(EMPTY)
        rows.append(b"".join(cells))
    return b"".join(rows)


def main():
    data = bytearray()
    data += b"retrovert_selftest".ljust(20, b"\x00")

    # Sample 1 descriptor; 30 empty descriptors follow.
    data += b"selftest triangle".ljust(22, b"\x00")
    data += struct.pack(">H", SAMPLE_LEN // 2)  # length in words
    data += bytes([0])  # finetune
    data += bytes([48])  # volume
    data += struct.pack(">HH", 0, SAMPLE_LEN // 2)  # loop start/length: loop all
    for _ in range(30):
        data += b"\x00" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1)

    data += bytes([2])  # song length: two pattern-table entries
    data += bytes([127])  # historical NoiseTracker byte
    data += bytes([0, 1] + [0] * 126)  # pattern order table
    data += b"M.K."

    data += pattern(["C-2", "E-2", "G-2", "C-3"])
    data += pattern(["A-2", "C-3", "E-2", "F-2"])
    data += sample_data()

    OUT.write_bytes(bytes(data))
    print(f"wrote {OUT} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
