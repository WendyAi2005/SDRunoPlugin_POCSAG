"""Compare a multimon-ng style transition PLL on a recorded field capture."""

from __future__ import annotations

import argparse
import wave
from pathlib import Path

import numpy as np

from analyze_field_capture import SYNC, correctable_bits, hamming32


def slice_bits(audio: np.ndarray, rate: int, alpha: float, subsamp: int) -> list[int]:
    dc = 0.0
    filtered = 0.0
    phase = 0.0
    step = 1200.0 * subsamp / rate
    previous = False
    have_previous = False
    bits: list[int] = []
    for sample in audio[::subsamp]:
        value = float(sample)
        dc += 0.0005 * (value - dc)
        ac = value - dc
        filtered += alpha * (ac - filtered)
        level = filtered > 0.0
        if have_previous and level != previous:
            # Same early/late transition correction used by multimon-ng.
            if phase < 0.5 - step / 2.0:
                phase += step / 8.0
            else:
                phase -= step / 8.0
            phase %= 1.0
        previous = level
        have_previous = True
        phase += step
        if phase >= 1.0:
            phase %= 1.0
            bits.append(int(level))
    return bits


def best_reports(bits: list[int]) -> list[dict[str, int | bool]]:
    register = 0
    reports: list[dict[str, int | bool]] = []
    transitions = 0
    previous = 0
    for index, bit in enumerate(bits):
        register = ((register << 1) | bit) & 0xFFFFFFFF
        transitions = ((transitions << 1) | int(index > 0 and bit != previous)) & ((1 << 64) - 1)
        previous = bit
        if index < 31:
            continue
        for inverted, target in ((False, SYNC), (True, (~SYNC) & 0xFFFFFFFF)):
            distance = hamming32(register, target)
            if distance > 6 or index + 32 * 16 >= len(bits):
                continue
            valid = invalid = corrected = 0
            for word_index in range(16):
                start = index + 1 + word_index * 32
                word = 0
                for next_bit in bits[start:start + 32]:
                    word = (word << 1) | (next_bit ^ int(inverted))
                count = correctable_bits(word)
                if count is not None:
                    valid += 1
                    corrected += count
                else:
                    invalid += 1
            reports.append({
                "bit_index": index,
                "sync_distance": distance,
                "inverted": inverted,
                "valid_words": valid,
                "invalid_words": invalid,
                "corrected_bits": corrected,
            })
    reports.sort(key=lambda r: (-int(r["valid_words"]), int(r["sync_distance"]), int(r["corrected_bits"])))
    return reports[:3]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wav", type=Path)
    args = parser.parse_args()
    with wave.open(str(args.wav), "rb") as source:
        rate = source.getframerate()
        channels = source.getnchannels()
        audio = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2").reshape(-1, channels)[:, 0].astype(np.float64) / 32768.0
    for subsamp in (1, 2, 4):
        for alpha in (1.0, 0.45, 0.25, 0.15, 0.08):
            reports = best_reports(slice_bits(audio, rate, alpha, subsamp))
            print({"subsamp": subsamp, "alpha": alpha, "best": reports})


if __name__ == "__main__":
    main()
