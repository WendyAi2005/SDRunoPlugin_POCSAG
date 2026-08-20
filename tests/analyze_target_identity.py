#!/usr/bin/env python3
"""Replay exported railway rows through the legacy and target_uid identity models."""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path

REMOVE_MS = 300_000


def truth(value: str) -> bool:
    return value.lower() == "true"


def load_updates(path: Path) -> list[dict]:
    groups: dict[tuple[int, int], list[dict]] = defaultdict(list)
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            if truth(row.get("railway_valid", "")) or truth(row.get("railway_ext_valid", "")):
                groups[(int(row["timestamp_unix_ms"]), int(row["transmission_id"]))].append(row)
    updates: list[dict] = []
    for (timestamp, transmission), rows in sorted(groups.items()):
        basics = [row for row in rows if row["ric"] == "1234000" and truth(row["railway_valid"])]
        extensions = [row for row in rows if row["ric"] == "1234002" and truth(row["railway_ext_valid"])]
        count = max(len(basics), len(extensions))
        for index in range(count):
            basic = basics[index] if index < len(basics) else None
            ext = extensions[index] if index < len(extensions) else None
            updates.append({
                "timestamp": timestamp,
                "transmission": transmission,
                "has_basic": basic is not None,
                "has_ext": ext is not None,
                "train": basic["decoded_train"] if basic else "",
                "loco": ext["locomotive_id"] if ext else "",
                "end": ext["locomotive_end"] if ext else "",
                "line": ext["line_name"] if ext else "",
                "lon": float(ext["longitude_deg"]) if ext and ext["longitude_deg"] else None,
                "lat": float(ext["latitude_deg"]) if ext and ext["latitude_deg"] else None,
            })
    return updates


def duplicate_count(targets: list[dict], key: str) -> int:
    counts = Counter(target[key] for target in targets if target.get(key))
    return sum(count - 1 for count in counts.values() if count > 1)


def prune(targets: dict, now: int) -> None:
    for key in list(targets):
        if now - targets[key]["last"] > REMOVE_MS:
            del targets[key]


def replay_legacy(updates: list[dict]) -> dict:
    targets: dict[str, dict] = {}
    created = peak = 0
    anonymous = 0
    for update in updates:
        prune(targets, update["timestamp"])
        found = None
        if update["train"]:
            found = next((key for key, target in targets.items() if target["train"] == update["train"]), None)
        if found is None and update["loco"]:
            found = next((key for key, target in targets.items()
                          if target["loco"] == update["loco"] and target["end"] == update["end"]), None)
        if found is None:
            complements = [(key, target) for key, target in targets.items()
                           if update["timestamp"] - target["last"] <= 2000 and
                           ((update["has_basic"] and not update["has_ext"] and not target["has_basic"] and target["has_ext"]) or
                            (update["has_ext"] and not update["has_basic"] and target["has_basic"] and not target["has_ext"]))]
            if complements:
                found = max(complements, key=lambda item: item[1]["last"])[0]
        if found is None:
            anonymous += 1
            found = update["train"] or (f"LOCO:{update['loco']}:{update['end']}" if update["loco"] else f"UNKNOWN:{anonymous}")
            while found in targets:
                found += f":{anonymous}"
            targets[found] = {"train": "", "loco": "", "end": "", "has_basic": False,
                              "has_ext": False, "last": 0, "position": False}
            created += 1
        target = targets[found]
        if update["has_basic"]:
            target.update(train=update["train"], has_basic=True)
        if update["has_ext"]:
            target.update(loco=update["loco"], end=update["end"], has_ext=True,
                          position=update["lon"] is not None and update["lat"] is not None)
        target["last"] = update["timestamp"]
        peak = max(peak, len(targets))
    active = list(targets.values())
    return {"created_targets": created, "peak_active_targets": peak, "final_active_targets": len(active),
            "merge_count": 0, "duplicate_train_number": duplicate_count(active, "train"),
            "duplicate_locomotive_id": duplicate_count(active, "loco"),
            "orphan_map_marker": duplicate_count([t for t in active if t["position"]], "loco")}


def replay_uid(updates: list[dict]) -> dict:
    targets: dict[int, dict] = {}
    train_index: dict[str, int] = {}
    loco_index: dict[str, int] = {}
    transmission_index: dict[int, int] = {}
    created = merges = peak = next_uid = 0

    def rebuild() -> None:
        train_index.clear(); loco_index.clear(); transmission_index.clear()
        for uid, target in targets.items():
            for value in target["trains"]: train_index[value] = uid
            for value in target["locos"]: loco_index[value] = uid
            for value in target["transmissions"]: transmission_index[value] = uid

    def merge(primary: int, secondary: int) -> None:
        nonlocal merges
        a, b = targets[primary], targets[secondary]
        for field in ("trains", "locos", "ends", "transmissions"):
            a[field].update(b[field])
        if b["last"] >= a["last"]:
            for field in ("train", "loco", "end", "has_basic", "has_ext", "position", "line", "lon", "lat"):
                if b.get(field) not in (None, "", False) or field.startswith("has_"):
                    a[field] = b.get(field)
        a["last"] = max(a["last"], b["last"])
        del targets[secondary]
        merges += 1

    for update in updates:
        prune(targets, update["timestamp"])
        rebuild()
        direct = {uid for uid in (transmission_index.get(update["transmission"]),
                                   train_index.get(update["train"]), loco_index.get(update["loco"])) if uid}
        if direct:
            uid = min(direct)
            for secondary in sorted(direct - {uid}): merge(uid, secondary)
        else:
            candidates = []
            for candidate_uid, target in targets.items():
                complementary = ((update["has_basic"] and not update["has_ext"] and not target["has_basic"] and target["has_ext"]) or
                                 (update["has_ext"] and not update["has_basic"] and target["has_basic"] and not target["has_ext"]))
                if not complementary: continue
                difference = abs(update["timestamp"] - target["last"])
                score = 20 + (40 if difference < 1000 else 20 if difference < 5000 else 0)
                if update["line"] and update["line"] == target["line"]: score += 20
                if score >= 60: candidates.append((score, target["last"], candidate_uid))
            uid = max(candidates)[2] if candidates else 0
        if not uid:
            next_uid += 1; uid = next_uid; created += 1
            targets[uid] = {"train": "", "loco": "", "end": "", "line": "", "lon": None, "lat": None,
                            "has_basic": False, "has_ext": False, "position": False, "last": 0,
                            "trains": set(), "locos": set(), "ends": set(), "transmissions": set()}
        target = targets[uid]
        target["transmissions"].add(update["transmission"])
        if update["has_basic"]:
            target.update(train=update["train"], has_basic=True)
            if update["train"]: target["trains"].add(update["train"])
        if update["has_ext"]:
            target.update(loco=update["loco"], end=update["end"], line=update["line"], lon=update["lon"], lat=update["lat"],
                          has_ext=True, position=update["lon"] is not None and update["lat"] is not None)
            if update["loco"]: target["locos"].add(update["loco"])
            if update["end"]: target["ends"].add(update["end"])
        target["last"] = update["timestamp"]
        rebuild()
        conflicts = [other for other, value in targets.items() if other != uid and
                     ((target["train"] and value["train"] == target["train"]) or
                      (target["loco"] and value["loco"] == target["loco"]))]
        for secondary in conflicts: merge(uid, secondary)
        peak = max(peak, len(targets))
    active = list(targets.values())
    return {"created_targets": created, "peak_active_targets": peak, "final_active_targets": len(active),
            "merge_count": merges, "duplicate_train_number": duplicate_count(active, "train"),
            "duplicate_locomotive_id": duplicate_count(active, "loco"),
            "orphan_map_marker": duplicate_count([t for t in active if t["position"]], "loco")}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    updates = load_updates(args.csv)
    report = {"source": str(args.csv), "valid_state_updates": len(updates),
              "before_legacy_identity": replay_legacy(updates),
              "after_target_uid_identity": replay_uid(updates)}
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
