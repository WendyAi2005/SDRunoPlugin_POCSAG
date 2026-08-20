"""Replay the field WAV through the plugin's current clock/slicer logic."""

from __future__ import annotations

import argparse
import wave
from pathlib import Path

import numpy as np


SYNC = 0x7CD215D8
POLYNOMIAL = 0x769


def hamming32(a: int, b: int) -> int:
    return ((a ^ b) & 0xFFFFFFFF).bit_count()


def valid_codeword(word: int) -> bool:
    if word.bit_count() & 1:
        return False
    value = word >> 1
    for bit in range(30, 9, -1):
        if value & (1 << bit):
            value ^= POLYNOMIAL << (bit - 10)
    return (value & 0x3FF) == 0


def correctable_bits(word: int) -> int | None:
    if valid_codeword(word):
        return 0
    for first in range(32):
        if valid_codeword(word ^ (1 << first)):
            return 1
    for first in range(31):
        for second in range(first + 1, 32):
            if valid_codeword(word ^ (1 << first) ^ (1 << second)):
                return 2
    return None


def replay(samples: np.ndarray, sample_rate: float, baud: int = 1200) -> dict[str, int | float]:
    phase_step = baud / sample_rate
    low_pass_alpha = min(0.45, 6.0 * baud / sample_rate)
    dc = 0.0
    filtered = 0.0
    envelope = 0.001
    phase = 0.0
    last_level = False
    have_level = False
    samples_since_edge = 0
    preamble_edge_count = 0
    preamble_window = 0
    preamble_bits = 0
    preamble_transitions = 0
    previous_bit = False
    have_previous_bit = False
    search_register = 0
    transitions = 0
    syncs = 0
    minimum_sync_distance = 32
    bits: list[int] = []
    preamble_flags: list[bool] = []

    for sample in samples:
        samples_since_edge = min(samples_since_edge + 1, int(sample_rate))
        value = float(sample)
        dc += 0.0005 * (value - dc)
        ac = value - dc
        filtered += low_pass_alpha * (ac - filtered)
        envelope += 0.002 * (abs(filtered) - envelope)

        threshold = max(0.00002, envelope * 0.08)
        level = last_level
        if filtered > threshold:
            level = True
        elif filtered < -threshold:
            level = False

        if not have_level:
            last_level = level
            have_level = True
        elif level != last_level:
            transitions += 1
            samples_per_bit = sample_rate / baud
            edge_length = float(samples_since_edge)
            if samples_per_bit * 0.70 <= edge_length <= samples_per_bit * 1.30:
                preamble_edge_count += 1
            else:
                preamble_edge_count = 0
            if samples_per_bit * 0.32 <= edge_length <= samples_per_bit * 1.90:
                phase = 0.0
            if preamble_edge_count >= 40:
                preamble_window = 512
            samples_since_edge = 0
            last_level = level

        old_phase = phase
        phase += phase_step
        if phase >= 1.0:
            phase -= 1.0
        if old_phase < 0.5 <= phase:
            bit = level
            bits.append(int(bit))
            preamble_flags.append(preamble_window > 0)
            search_register = ((search_register << 1) | int(bit)) & 0xFFFFFFFF
            transition = have_previous_bit and bit != previous_bit
            preamble_transitions = ((preamble_transitions << 1) | int(transition)) & ((1 << 64) - 1)
            preamble_bits = min(preamble_bits + 1, 64)
            previous_bit = bit
            have_previous_bit = True
            if preamble_bits >= 64 and preamble_transitions.bit_count() >= 54:
                preamble_window = 256
            elif preamble_window > 0:
                preamble_window -= 1
            if preamble_window > 0:
                distance = min(hamming32(search_register, SYNC), hamming32(search_register, (~SYNC) & 0xFFFFFFFF))
                minimum_sync_distance = min(minimum_sync_distance, distance)
                if distance <= 2:
                    syncs += 1
                    preamble_window = 0

    candidates: list[tuple[int, int, bool]] = []
    register = 0
    for index, bit in enumerate(bits):
        register = ((register << 1) | bit) & 0xFFFFFFFF
        if index >= 31 and preamble_flags[index]:
            normal = hamming32(register, SYNC)
            inverted = hamming32(register, (~SYNC) & 0xFFFFFFFF)
            candidates.append((min(normal, inverted), index, inverted < normal))
    candidates.sort()

    candidate_reports = []
    for distance, index, inverted in candidates[:6]:
        payload = bits[index + 1:index + 1 + 16 * 32]
        valid = 0
        corrected = 0
        invalid = 0
        for offset in range(0, len(payload) - 31, 32):
            word = 0
            for bit in payload[offset:offset + 32]:
                word = ((word << 1) | (bit ^ int(inverted))) & 0xFFFFFFFF
            errors = correctable_bits(word)
            if errors is None:
                invalid += 1
            else:
                valid += 1
                corrected += errors
        candidate_reports.append({
            "distance": distance,
            "bit_index": index,
            "inverted": inverted,
            "valid_words": valid,
            "invalid_words": invalid,
            "corrected_bits": corrected,
        })

    return {
        "samples": int(samples.size),
        "transitions": transitions,
        "syncs": syncs,
        "minimum_sync_distance": minimum_sync_distance,
        "best_candidates": candidate_reports,
    }


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

    if width != 2:
        raise SystemExit(f"Expected 16-bit PCM, got {width * 8}-bit")
    audio = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    frames = audio.reshape(-1, channels)

    print({"channels": channels, "sample_rate": sample_rate, "frames": len(frames)})
    print("mono-left", replay(frames[:, 0], sample_rate))
    if channels > 1:
        print("mono-average", replay(frames.mean(axis=1), sample_rate))
        print("interleaved-as-mono", replay(audio, sample_rate))


if __name__ == "__main__":
    main()
