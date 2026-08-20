#!/usr/bin/env python3
"""Offline validation for legacy SDRuno POCSAG CSV exports."""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path


NIBBLE_REVERSE = str.maketrans("0123456789ABCDEF", "084C2A6E195D3B7F")
POCSAG_NUMERIC_ALPHABET = "084 2.6]195-3U7["


def parse_nmea_coordinate(raw: str, degree_digits: int, maximum: int, hemisphere: str) -> dict[str, object]:
    result: dict[str, object] = {"raw": raw, "valid": False}
    if len(raw) != degree_digits + 6 or not raw.isdecimal():
        return result
    degrees = int(raw[:degree_digits])
    minutes = int(raw[degree_digits:]) / 10_000
    if degrees > maximum or minutes >= 60 or (degrees == maximum and minutes > 0):
        return result
    result.update(
        {
            "valid": True,
            "degrees": degrees,
            "minutes": minutes,
            "degree_minute": f"{degrees}°{minutes:.4f}′ {hemisphere}",
            "decimal_degree": degrees + minutes / 60,
        }
    )
    return result


def normalize_bits(bits: str) -> str:
    output: list[str] = []
    for offset in range(0, len(bits) - 3, 4):
        nibble = bits[offset : offset + 4]
        if set(nibble) - {"0", "1"}:
            return ""
        output.append(f"{int(nibble[::-1], 2):X}")
    return "".join(output)


def parse_extension(row: dict[str, str]) -> dict[str, object]:
    count = int(row.get("message_codeword_count") or 0)
    uncorrectable = (row.get("has_uncorrectable_codeword") or "").lower() == "true"
    normalized = normalize_bits(row.get("message_bits") or "")
    failures: list[str] = []
    if count < 10 or len(normalized) < 50:
        failures.append("truncated")
    elif count != 10 or len(normalized) != 50:
        failures.append("unexpected_length")
    if uncorrectable:
        failures.append("uncorrectable_codeword")

    result: dict[str, object] = {
        "normalized_hex": normalized,
        "valid": False,
        "failures": failures,
        "message_codeword_count": count,
        "uncorrectable": uncorrectable,
    }
    if len(normalized) < 50:
        return result

    try:
        prefix = bytes.fromhex(normalized[0:4]).decode("ascii").strip(" \x00")
        prefix_valid = True
    except (ValueError, UnicodeDecodeError):
        prefix = ""
        prefix_valid = False
        failures.append("invalid_prefix_ascii")

    locomotive_id = normalized[4:12]
    locomotive_valid = len(locomotive_id) == 8 and locomotive_id.isdecimal()
    if not locomotive_valid:
        failures.append("invalid_locomotive_id")

    try:
        locomotive_end = bytes.fromhex(normalized[12:14]).decode("ascii").strip(" \x00")
        end_valid = bool(locomotive_end)
    except (ValueError, UnicodeDecodeError):
        locomotive_end = ""
        end_valid = False
    if not end_valid:
        failures.append("invalid_locomotive_end_ascii")

    line_hex = normalized[14:30]
    try:
        line_name = bytes.fromhex(line_hex).strip(b"\x00 ").decode("gbk")
        line_valid = bool(line_name)
    except (ValueError, UnicodeDecodeError):
        line_name = ""
        line_valid = False
    if not line_valid:
        failures.append("invalid_gbk_line_name")

    longitude_raw = normalized[30:39]
    longitude_coordinate = parse_nmea_coordinate(longitude_raw, 3, 180, "E")
    longitude_valid = bool(longitude_coordinate["valid"])
    longitude = longitude_coordinate.get("decimal_degree")
    if not longitude_valid:
        failures.append("invalid_longitude")

    latitude_raw = normalized[39:47]
    latitude_coordinate = parse_nmea_coordinate(latitude_raw, 2, 90, "N")
    latitude_valid = bool(latitude_coordinate["valid"])
    latitude = latitude_coordinate.get("decimal_degree")
    if not latitude_valid:
        failures.append("invalid_latitude")

    result.update(
        {
            "train_prefix": prefix,
            "train_prefix_valid": prefix_valid,
            "locomotive_id": locomotive_id,
            "locomotive_id_valid": locomotive_valid,
            "locomotive_end": locomotive_end,
            "locomotive_end_valid": end_valid,
            "line_name": line_name,
            "line_name_valid": line_valid,
            "longitude_raw": longitude_raw,
            "longitude": longitude,
            "longitude_valid": longitude_valid,
            "longitude_degree_minute": longitude_coordinate.get("degree_minute", ""),
            "longitude_minutes": longitude_coordinate.get("minutes"),
            "latitude_raw": latitude_raw,
            "latitude": latitude,
            "latitude_valid": latitude_valid,
            "latitude_degree_minute": latitude_coordinate.get("degree_minute", ""),
            "latitude_minutes": latitude_coordinate.get("minutes"),
            "aux_raw": normalized[47:50],
        }
    )
    result["valid"] = (
        count == 10
        and len(normalized) == 50
        and not uncorrectable
        and prefix_valid
        and locomotive_valid
        and end_valid
        and line_valid
        and longitude_valid
        and latitude_valid
    )
    return result


def parse_basic(row: dict[str, str]) -> dict[str, object] | None:
    bits = row.get("message_bits") or ""
    text = "".join(
        POCSAG_NUMERIC_ALPHABET[int(bits[offset : offset + 4], 2)]
        for offset in range(0, len(bits) - 3, 4)
        if not (set(bits[offset : offset + 4]) - {"0", "1"})
    ).rstrip(" \x00\x03\x04\x17")
    if len(text) != 15 or text[5:6] != " " or text[9:10] != " ":
        return None
    layout = text[:15]
    if any(ch not in "0123456789 -" for ch in layout):
        return None
    train, speed, kilometer = layout[:5].strip(), layout[6:9].strip(), layout[10:15].strip()
    return {
        "train": train if train.isdecimal() else "",
        "speed": int(speed) if speed.isdecimal() else None,
        "kilometer": int(kilometer) / 10 if kilometer.isdecimal() else None,
    }


def strict_basic(row: dict[str, str]) -> dict[str, object] | None:
    if int(row.get("message_codeword_count") or 0) != 3:
        return None
    if (row.get("has_uncorrectable_codeword") or "").lower() == "true":
        return None
    return parse_basic(row)


def haversine_km(a: tuple[float, float], b: tuple[float, float]) -> float:
    lon1, lat1 = map(math.radians, a)
    lon2, lat2 = map(math.radians, b)
    dlon, dlat = lon2 - lon1, lat2 - lat1
    value = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    return 6371.0088 * 2 * math.asin(math.sqrt(value))


def analyze(csv_path: Path) -> dict[str, object]:
    with csv_path.open("r", encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))

    extension_rows = [row for row in rows if row.get("ric") == "1234002"]
    parsed_extensions = [(row, parse_extension(row)) for row in extension_rows]
    basic_rows = [row for row in rows if row.get("ric") == "1234000"]
    strict_basics = [(row, parsed) for row in basic_rows if (parsed := strict_basic(row)) is not None]
    basic_candidates = [(row, parsed) for row, parsed in strict_basics if parsed["train"]]

    count_distribution = Counter(int(row.get("message_codeword_count") or 0) for row in extension_rows)
    aux_distribution = Counter(
        str(parsed.get("aux_raw", "")) for _, parsed in parsed_extensions if parsed.get("aux_raw")
    )
    failure_distribution = Counter(
        failure for _, parsed in parsed_extensions for failure in parsed.get("failures", [])
    )
    line_distribution = Counter(
        str(parsed.get("line_name", ""))
        for _, parsed in parsed_extensions
        if parsed.get("line_name_valid")
    )

    pairs: list[dict[str, object]] = []
    for ext_row, extension in parsed_extensions:
        if not extension.get("valid"):
            continue
        ext_time = int(ext_row["timestamp_unix_ms"])
        nearby = [
            (abs(int(row["timestamp_unix_ms"]) - ext_time), row, basic)
            for row, basic in basic_candidates
            if abs(int(row["timestamp_unix_ms"]) - ext_time) <= 1000
        ]
        if not nearby:
            continue
        _, basic_row, basic = min(nearby, key=lambda item: item[0])
        pairs.append(
            {
                "timestamp_unix_ms": ext_time,
                "pairing_method": "timestamp_fallback",
                "full_train_number": str(extension["train_prefix"]) + str(basic["train"]),
                "train_number": basic["train"],
                "speed": basic["speed"],
                "kilometer": basic["kilometer"],
                "locomotive_id": extension["locomotive_id"],
                "line_name": extension["line_name"],
                "longitude": extension["longitude"],
                "latitude": extension["latitude"],
                "time_delta_ms": abs(int(basic_row["timestamp_unix_ms"]) - ext_time),
            }
        )

    by_train: dict[str, list[dict[str, object]]] = defaultdict(list)
    for pair in pairs:
        by_train[str(pair["full_train_number"])].append(pair)
    repeated = {train: sorted(values, key=lambda item: int(item["timestamp_unix_ms"]))
                for train, values in by_train.items() if len(values) > 1}
    locomotive_change_trains = [
        train for train, values in repeated.items()
        if len({str(item["locomotive_id"]) for item in values}) > 1
    ]
    line_change_trains = [
        train for train, values in repeated.items()
        if len({str(item["line_name"]) for item in values}) > 1
    ]

    movement_intervals = 0
    kilometer_changed = 0
    gps_changed = 0
    kilometer_and_gps_move = 0
    speed_errors: list[float] = []
    for values in repeated.values():
        for previous, current in zip(values, values[1:]):
            seconds = (int(current["timestamp_unix_ms"]) - int(previous["timestamp_unix_ms"])) / 1000
            if seconds <= 0:
                continue
            distance = haversine_km(
                (float(previous["longitude"]), float(previous["latitude"])),
                (float(current["longitude"]), float(current["latitude"])),
            )
            old_km, new_km = previous["kilometer"], current["kilometer"]
            if old_km is not None and new_km is not None:
                movement_intervals += 1
                km_moved = abs(float(new_km) - float(old_km)) > 0
                gps_moved = distance > 0.001
                kilometer_changed += int(km_moved)
                gps_changed += int(gps_moved)
                if km_moved and gps_moved:
                    kilometer_and_gps_move += 1
            if current["speed"] is not None and 3 <= seconds <= 30 and distance > 0.001:
                inferred = distance / (seconds / 3600)
                speed_errors.append(abs(inferred - float(current["speed"])))

    speed_errors.sort()
    median_speed_error = speed_errors[len(speed_errors) // 2] if speed_errors else None
    valid_extensions = [parsed for _, parsed in parsed_extensions if parsed.get("valid")]
    full_ten = [(row, parsed) for row, parsed in parsed_extensions
                if int(row.get("message_codeword_count") or 0) == 10]
    full_ten_clean = [(row, parsed) for row, parsed in full_ten
                      if (row.get("has_uncorrectable_codeword") or "").lower() != "true"]

    valid_ext_rows = [(row, parsed) for row, parsed in parsed_extensions if parsed.get("valid")]
    available_basics = set(range(len(strict_basics)))
    paired_basic_indexes: set[int] = set()
    paired_ext_indexes: set[int] = set()
    state_pairs: list[tuple[dict[str, object], dict[str, object]]] = []
    for ext_index, (ext_row, ext) in enumerate(valid_ext_rows):
        ext_time = int(ext_row.get("timestamp_unix_ms") or 0)
        candidates = [
            (abs(int(strict_basics[index][0].get("timestamp_unix_ms") or 0) - ext_time), index)
            for index in available_basics
            if abs(int(strict_basics[index][0].get("timestamp_unix_ms") or 0) - ext_time) <= 2000
        ]
        if not candidates:
            continue
        _, basic_index = min(candidates)
        available_basics.remove(basic_index)
        paired_basic_indexes.add(basic_index)
        paired_ext_indexes.add(ext_index)
        state_pairs.append((strict_basics[basic_index][1], ext))

    full_train_ids = {
        str(ext.get("train_prefix", "")) + str(basic.get("train", ""))
        for basic, ext in state_pairs if basic.get("train")
    }
    full_train_ids.update(
        str(parsed.get("train")) for index, (_, parsed) in enumerate(strict_basics)
        if index not in paired_basic_indexes and parsed.get("train")
    )
    loco_only_ids = {
        (str(parsed.get("locomotive_id", "")), str(parsed.get("locomotive_end", "")))
        for index, (_, parsed) in enumerate(valid_ext_rows) if index not in paired_ext_indexes
    }
    loco_only_ids.update(
        (str(ext.get("locomotive_id", "")), str(ext.get("locomotive_end", "")))
        for basic, ext in state_pairs if not basic.get("train")
    )

    regression = {
        "ric_1234000_total": len(basic_rows),
        "valid_3cw_basic": len(strict_basics),
        "placeholder_basic": sum(
            not parsed.get("train") and parsed.get("speed") is None and parsed.get("kilometer") is None
            for _, parsed in strict_basics
        ),
        "ric_1234002_total": len(extension_rows),
        "valid_10cw_ext": len(valid_ext_rows),
        "basic_only": len(strict_basics) - len(paired_basic_indexes),
        "ext_only": len(valid_ext_rows) - len(paired_ext_indexes),
        "basic_ext_pairs": len(state_pairs),
        "blocked_false_basic": len(basic_rows) - len(strict_basics),
        "distinct_train_targets": len(full_train_ids),
        "locomotive_only_targets": len(loco_only_ids),
        "examples": {
            "basic_only": [parsed for index, (_, parsed) in enumerate(strict_basics)
                           if index not in paired_basic_indexes][:3],
            "ext_only": [parsed for index, (_, parsed) in enumerate(valid_ext_rows)
                         if index not in paired_ext_indexes][:3],
            "paired": [{"basic": basic, "extension": ext} for basic, ext in state_pairs[:3]],
        },
    }

    coordinate_samples = [
        {
            "timestamp_unix_ms": int(row.get("timestamp_unix_ms") or 0),
            "longitude_raw": parsed.get("longitude_raw"),
            "latitude_raw": parsed.get("latitude_raw"),
            "longitude_degree_minute": parsed.get("longitude_degree_minute"),
            "latitude_degree_minute": parsed.get("latitude_degree_minute"),
            "longitude_decimal_degree": parsed.get("longitude"),
            "latitude_decimal_degree": parsed.get("latitude"),
            "longitude_minutes": parsed.get("longitude_minutes"),
            "latitude_minutes": parsed.get("latitude_minutes"),
            "minutes_below_60": (
                float(parsed.get("longitude_minutes") or 60) < 60
                and float(parsed.get("latitude_minutes") or 60) < 60
            ),
        }
        for row, parsed in valid_ext_rows[:10]
    ]

    return {
        "source": str(csv_path),
        "coordinate_interpretation": {
            "format": "NMEA degrees/minutes without decimal point",
            "longitude_layout": "dddmm.mmmm",
            "latitude_layout": "ddmm.mmmm",
            "samples": coordinate_samples,
            "all_sample_minutes_below_60": all(sample["minutes_below_60"] for sample in coordinate_samples),
        },
        "partial_state_regression": regression,
        "ric_1234002": {
            "total": len(extension_rows),
            "message_codeword_count_distribution": dict(sorted(count_distribution.items())),
            "ten_cw": len(full_ten),
            "ten_cw_without_uncorrectable": len(full_ten_clean),
            "valid_extensions": len(valid_extensions),
            "prefix_decoded": sum(bool(parsed.get("train_prefix_valid")) for _, parsed in parsed_extensions),
            "locomotive_id_valid": sum(bool(parsed.get("locomotive_id_valid")) for _, parsed in parsed_extensions),
            "gbk_line_valid": sum(bool(parsed.get("line_name_valid")) for _, parsed in parsed_extensions),
            "longitude_valid": sum(bool(parsed.get("longitude_valid")) for _, parsed in parsed_extensions),
            "latitude_valid": sum(bool(parsed.get("latitude_valid")) for _, parsed in parsed_extensions),
            "aux_raw_distribution": dict(aux_distribution.most_common()),
            "failure_distribution": dict(failure_distribution.most_common()),
            "line_name_frequency": dict(line_distribution.most_common()),
        },
        "pairing": {
            "timestamp_fallback_pairs": len(pairs),
            "exact_timestamp_pairs": sum(int(pair["time_delta_ms"]) == 0 for pair in pairs),
            "repeated_train_groups": len(repeated),
            "groups_with_locomotive_changes": len(locomotive_change_trains),
            "locomotive_change_trains": locomotive_change_trains,
            "groups_with_line_changes": len(line_change_trains),
            "line_change_trains": line_change_trains,
            "movement_intervals": movement_intervals,
            "kilometer_changed_intervals": kilometer_changed,
            "gps_changed_intervals": gps_changed,
            "kilometer_and_gps_both_moved": kilometer_and_gps_move,
            "gps_vs_reported_speed_median_absolute_error_kmh": median_speed_error,
            "interpretation_note": (
                "Legacy pairing uses nearest timestamps within one second; inconsistencies are reported "
                "for review and are not used to alter the protocol decoder."
            ),
        },
    }


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = analyze(args.csv)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
