"""Search fixed 1200-baud clock phases in a recorded SDRuno NFM WAV."""

from __future__ import annotations

import argparse
import wave
from pathlib import Path

import numpy as np

from analyze_field_capture import SYNC, correctable_bits, hamming32


def score_bits(bits: np.ndarray, phase: int, mode: str) -> list[dict[str, int | bool | str]]:
    reports: list[dict[str, int | bool | str]] = []
    register = 0
    transitions = np.zeros(len(bits), dtype=np.int8)
    transitions[1:] = bits[1:] != bits[:-1]
    alternating = np.convolve(transitions, np.ones(64, dtype=np.int16), mode="same")
    for index, bit in enumerate(bits):
        register = ((register << 1) | int(bit)) & 0xFFFFFFFF
        if index < 64 or alternating[index - 16] < 52:
            continue
        normal = hamming32(register, SYNC)
        inverse = hamming32(register, (~SYNC) & 0xFFFFFFFF)
        distance = min(normal, inverse)
        if distance > 8:
            continue
        inverted = inverse < normal
        payload = bits[index + 1:index + 1 + 512]
        valid = invalid = corrected = 0
        for offset in range(0, len(payload) - 31, 32):
            word = 0
            for payload_bit in payload[offset:offset + 32]:
                word = ((word << 1) | (int(payload_bit) ^ int(inverted))) & 0xFFFFFFFF
            errors = correctable_bits(word)
            if errors is None:
                invalid += 1
            else:
                valid += 1
                corrected += errors
        reports.append({
            "mode": mode,
            "phase": phase,
            "bit_index": index,
            "sync_distance": distance,
            "inverted": inverted,
            "valid_words": valid,
            "invalid_words": invalid,
            "corrected_bits": corrected,
        })
    return reports


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wav", type=Path)
    parser.add_argument("--seconds", type=float, default=15.0)
    args = parser.parse_args()

    with wave.open(str(args.wav), "rb") as stream:
        channels = stream.getnchannels()
        sample_rate = stream.getframerate()
        width = stream.getsampwidth()
        frame_count = min(stream.getnframes(), int(args.seconds * sample_rate))
        raw = stream.readframes(frame_count)
    if width != 2 or sample_rate % 1200:
        raise SystemExit("This field search expects 16-bit PCM and an integer 1200-baud sample ratio")

    audio = np.frombuffer(raw, dtype="<i2").reshape(-1, channels)[:, 0].astype(np.float64) / 32768.0
    samples_per_bit = sample_rate // 1200
    all_reports: list[dict[str, int | bool | str]] = []
    for phase in range(samples_per_bit):
        available = (len(audio) - phase) // samples_per_bit
        blocks = audio[phase:phase + available * samples_per_bit].reshape(-1, samples_per_bit)
        center_bits = blocks[:, samples_per_bit // 2] >= 0.0
        integrate_bits = blocks[:, samples_per_bit // 4:3 * samples_per_bit // 4].mean(axis=1) >= 0.0
        all_reports.extend(score_bits(center_bits, phase, "center"))
        all_reports.extend(score_bits(integrate_bits, phase, "integrate-middle-half"))

    all_reports.sort(key=lambda report: (
        -int(report["valid_words"]),
        int(report["sync_distance"]),
        int(report["corrected_bits"]),
    ))
    for report in all_reports[:20]:
        print(report)


if __name__ == "__main__":
    main()
