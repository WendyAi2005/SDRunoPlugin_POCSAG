#!/usr/bin/env python3
"""Replay exported POCSAG JSONL into the mileage anchor data contract."""

from __future__ import annotations

import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path


OUTLIER_M = 500.0
MAX_GAP_KM = 2.0


def haversine(a_lat: float, a_lon: float, b_lat: float, b_lon: float) -> float:
    r = 6371000.0
    p1, p2 = math.radians(a_lat), math.radians(b_lat)
    dp, dl = math.radians(b_lat - a_lat), math.radians(b_lon - a_lon)
    x = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return r * 2 * math.atan2(math.sqrt(x), math.sqrt(max(0.0, 1 - x)))


def nmea(raw: str, longitude: bool) -> float | None:
    digits = 3 if longitude else 2
    expected = digits + 6
    if len(raw) != expected or not raw.isdigit():
        return None
    degrees, minutes = int(raw[:digits]), int(raw[digits:]) / 10000.0
    maximum = 180 if longitude else 90
    if degrees > maximum or minutes >= 60 or (degrees == maximum and minutes > 0):
        return None
    return degrees + minutes / 60.0


def ext_coordinate(message: dict) -> tuple[float, float] | None:
    normalized = message.get("railway_ext_normalized_hex") or ""
    if len(normalized) != 50:
        return None
    lon, lat = nmea(normalized[30:39], True), nmea(normalized[39:47], False)
    return (lat, lon) if lat is not None and lon is not None else None


def strict_basic(m: dict) -> bool:
    return (m.get("ric") == 1234000 and m.get("railway_valid") and
            m.get("message_codeword_count") == 3 and not m.get("has_uncorrectable_codeword") and
            m.get("decode_confidence") == "HIGH" and m.get("decoded_train") and
            m.get("decoded_km") is not None)


def strict_ext(m: dict) -> bool:
    return (m.get("ric") == 1234002 and m.get("railway_ext_valid") and
            m.get("message_codeword_count") == 10 and not m.get("has_uncorrectable_codeword") and
            m.get("railway_ext_confidence") == "HIGH" and m.get("line_name") and
            ext_coordinate(m) is not None)


def lookup(anchors: dict[tuple[str, float], dict], line: str, km: float) -> tuple[str, float, float] | None:
    key = (line, round(km, 1))
    if key in anchors:
        a = anchors[key]
        return "LOCAL_MILEAGE_EXACT", a["latitude"], a["longitude"]
    line_points = sorted((k[1], a) for k, a in anchors.items() if k[0] == line)
    lower = [p for p in line_points if p[0] < km]
    upper = [p for p in line_points if p[0] > km]
    if not lower or not upper:
        return None
    (lo_km, lo), (hi_km, hi) = lower[-1], upper[0]
    if hi_km - lo_km > MAX_GAP_KM:
        return None
    ratio = (km - lo_km) / (hi_km - lo_km)
    return ("LOCAL_MILEAGE_INTERPOLATED",
            lo["latitude"] + ratio * (hi["latitude"] - lo["latitude"]),
            lo["longitude"] + ratio * (hi["longitude"] - lo["longitude"]))


def main() -> None:
    source, output_json, output_report = map(Path, sys.argv[1:4])
    pairs: list[dict] = []
    for line in source.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        tx = json.loads(line)
        basics = [m for m in tx.get("messages", []) if strict_basic(m)]
        exts = [m for m in tx.get("messages", []) if strict_ext(m)]
        for basic, ext in zip(basics, exts):
            lat, lon = ext_coordinate(ext)  # type: ignore[misc]
            pairs.append({
                "timestamp": int(tx.get("timestamp_unix_ms", 0)),
                "train": str(basic["decoded_train"]),
                "speed": int(basic.get("decoded_speed_kmh") or 0),
                "line": ext["line_name"],
                "km": round(float(basic["decoded_km"]), 1),
                "latitude": lat,
                "longitude": lon,
            })

    anchors: dict[tuple[str, float], dict] = {}
    outliers = 0
    motion_rejected = 0
    last_train: dict[str, tuple[float, int]] = {}
    accepted_pairs = 0
    for p in pairs:
        previous = last_train.get(p["train"])
        if previous and p["timestamp"] > previous[1]:
            hours = (p["timestamp"] - previous[1]) / 3_600_000
            allowed = max(1.0, abs(p["speed"]) * hours * 3 + 0.5)
            if abs(p["km"] - previous[0]) > allowed:
                motion_rejected += 1
                continue
        last_train[p["train"]] = (p["km"], p["timestamp"])
        key = (p["line"], p["km"])
        a = anchors.get(key)
        if a is None:
            anchors[key] = {"line": p["line"], "mileage_km": p["km"],
                            "latitude": p["latitude"], "longitude": p["longitude"],
                            "samples": 1, "variance_accumulator": 0.0, "std_m": 0.0,
                            "outliers": 0, "first_seen": p["timestamp"], "last_seen": p["timestamp"]}
            accepted_pairs += 1
            continue
        distance = haversine(a["latitude"], a["longitude"], p["latitude"], p["longitude"])
        if distance > OUTLIER_M:
            a["outliers"] += 1
            outliers += 1
            continue
        old_count, new_count = a["samples"], a["samples"] + 1
        a["latitude"] += (p["latitude"] - a["latitude"]) / new_count
        a["longitude"] += (p["longitude"] - a["longitude"]) / new_count
        a["variance_accumulator"] += distance * distance * old_count / new_count
        a["samples"] = new_count
        a["std_m"] = math.sqrt(a["variance_accumulator"] / max(1, new_count - 1))
        a["last_seen"] = max(a["last_seen"], p["timestamp"])
        accepted_pairs += 1

    anchor_rows = []
    for _, a in sorted(anchors.items()):
        row = dict(a)
        row["quality"] = "HIGH" if row["samples"] >= 3 and row["std_m"] < 30 else "MEDIUM"
        row["source"] = "LOCAL_MILEAGE_EXACT"
        anchor_rows.append(row)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(json.dumps({"version": 1, "learning_enabled": True,
        "rejected_outliers": outliers, "anchors": anchor_rows}, ensure_ascii=False,
        separators=(",", ":")), encoding="utf-8")

    exact = interpolated = unavailable = 0
    deviations: list[float] = []
    for p in pairs:
        result = lookup(anchors, p["line"], p["km"])
        if result is None:
            unavailable += 1
            continue
        source_name, lat, lon = result
        exact += source_name.endswith("EXACT")
        interpolated += source_name.endswith("INTERPOLATED")
        deviations.append(haversine(p["latitude"], p["longitude"], lat, lon))

    lines: dict[str, list[dict]] = defaultdict(list)
    for a in anchor_rows:
        lines[a["line"]].append(a)
    summary_lines = []
    for name, values in sorted(lines.items()):
        kms = [v["mileage_km"] for v in values]
        summary_lines.append({"line": name, "anchors": len(values), "min_km": min(kms),
            "max_km": max(kms), "average_samples": sum(v["samples"] for v in values) / len(values),
            "gps_samples": sum(v["samples"] for v in values)})
    report = {
        "raw_transmissions": sum(1 for line in source.read_text(encoding="utf-8").splitlines() if line.strip()),
        "strict_basic_ext_pairs": len(pairs), "accepted_learning_samples": accepted_pairs,
        "motion_rejected": motion_rejected, "gps_outliers_rejected": outliers,
        "line_count": len(lines), "anchor_count": len(anchor_rows), "lines": summary_lines,
        "mileage_exact_messages": int(exact), "mileage_interpolated_messages": int(interpolated),
        "mileage_unavailable_messages": unavailable,
        "gps_vs_mileage_mean_m": statistics.fmean(deviations) if deviations else None,
        "gps_vs_mileage_median_m": statistics.median(deviations) if deviations else None,
        "gps_vs_mileage_max_m": max(deviations) if deviations else None,
    }
    output_report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
