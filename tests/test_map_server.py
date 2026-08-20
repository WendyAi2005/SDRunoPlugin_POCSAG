#!/usr/bin/env python3
"""Local integration tests for RailwayMapServer (P8-P10)."""

from __future__ import annotations

import base64
import json
import os
import socket
import sys
import time
import urllib.request
import urllib.parse
from pathlib import Path


HOST, PORT = "127.0.0.1", 8765


def receive_frame(sock: socket.socket, pending: bytes = b"") -> tuple[str, bytes]:
    def need(count: int) -> None:
        nonlocal pending
        while len(pending) < count:
            chunk = sock.recv(65536)
            if not chunk:
                raise RuntimeError("WebSocket closed")
            pending += chunk

    need(2)
    length = pending[1] & 0x7F
    offset = 2
    if length == 126:
        need(4)
        length = int.from_bytes(pending[2:4], "big")
        offset = 4
    elif length == 127:
        need(10)
        length = int.from_bytes(pending[2:10], "big")
        offset = 10
    need(offset + length)
    payload = pending[offset : offset + length]
    return payload.decode("utf-8"), pending[offset + length :]


def main() -> None:
    state_path = Path(sys.argv[1])
    original = state_path.read_text(encoding="utf-8")
    index = urllib.request.urlopen(f"http://{HOST}:{PORT}/", timeout=3).read().decode("utf-8")
    api = json.loads(urllib.request.urlopen(f"http://{HOST}:{PORT}/api/trains", timeout=3).read())
    assert api[0]["id"] == "1" and api[0]["target_uid"] == 1 and "当前列车" in index
    assert api[0]["position_source"] == "LOCAL_MILEAGE_INTERPOLATED"
    assert api[0]["radio_longitude"] is None
    print("Test P8 passed: HTTP page and /api/trains loaded")

    sock = socket.create_connection((HOST, PORT), timeout=3)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET /ws HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode("ascii"))
    response = b""
    while b"\r\n\r\n" not in response:
        response += sock.recv(4096)
    headers, pending = response.split(b"\r\n\r\n", 1)
    assert b"101 Switching Protocols" in headers
    first, pending = receive_frame(sock, pending)
    assert json.loads(first)["trains"][0]["target_uid"] == 1

    updated = json.loads(original)
    updated[0]["speed_kmh"] = 82
    state_path.write_text(json.dumps(updated, ensure_ascii=False), encoding="utf-8")
    sock.settimeout(3)
    deadline = time.time() + 3
    received_update = False
    while time.time() < deadline:
        message, pending = receive_frame(sock, pending)
        payload = json.loads(message)
        if payload.get("trains", [{}])[0].get("speed_kmh") == 82:
            received_update = True
            break
    state_path.write_text(original, encoding="utf-8")
    assert received_update
    app = urllib.request.urlopen(f"http://{HOST}:{PORT}/app.js", timeout=3).read().decode("utf-8")
    assert "marker.setLatLng" in app and "location.reload" not in app
    assert "parseNmea" in app and "wgsToGcj" in app and "bdToGcj" in app
    assert "targetsByUid" in app and "target_remove" in app and "target_update" in app
    assert "restore-defaults" in index and "NMEA 度分" in index and "BD-09 → WGS84" in index
    assert "位置优先级" in index and "显示 GPS 与公里标对比" in index
    print("Test P9 passed: WebSocket update moves existing marker without page reload")

    assert "!window.L" in app and "tileerror" in app and "当前目标列表仍可用" in index
    print("Test P10 passed: offline-basemap fallback keeps the train list available")

    # M6: once UID 2 is merged into UID 1 the server must explicitly remove
    # UID 2, allowing the browser to delete its marker and trajectory.
    original_targets = json.loads(original)
    duplicate = dict(original_targets[0])
    duplicate.update({"id": "2", "target_uid": 2, "display_id": "LOCO:23900443",
                      "train": None, "speed_kmh": None, "kilometer_km": None,
                      "created_by": "EXT_ONLY"})
    state_path.write_text(json.dumps(original_targets + [duplicate], ensure_ascii=False), encoding="utf-8")
    deadline = time.time() + 3
    while time.time() < deadline:
        message, pending = receive_frame(sock, pending)
        payload = json.loads(message)
        if payload.get("type") == "snapshot" and len(payload.get("trains", [])) == 2:
            break
    else:
        raise AssertionError("two-target snapshot not received")

    merged = dict(original_targets[0])
    merged["merge_count"] = 1
    merged["last_merge_reason"] = "IDENTITY_CONFLICT"
    state_path.write_text(json.dumps([merged], ensure_ascii=False), encoding="utf-8")
    removed = False
    deadline = time.time() + 3
    while time.time() < deadline:
        message, pending = receive_frame(sock, pending)
        payload = json.loads(message)
        if payload.get("type") == "target_remove" and payload.get("target_uid") == "2":
            removed = True
            break
    state_path.write_text(original, encoding="utf-8")
    sock.close()
    assert removed
    print("Test M6 passed: merged secondary UID emits target_remove for orphan marker cleanup")

    # KM8: local DB miss falls back to a line-qualified cached OSM milestone.
    q = urllib.parse.urlencode({"line": "测试线", "km": "88.8"})
    km8 = json.loads(urllib.request.urlopen(f"http://{HOST}:{PORT}/api/mileage/lookup?{q}", timeout=3).read())
    assert km8["valid"] and km8["source"] == "OSM_MILEAGE_EXACT"
    print("Test KM8 passed: cached OSM exact milestone is used after local miss")

    # KM9: an uncached OSM request returns pending immediately; local lookup
    # remains responsive while the background request may fail offline.
    q = urllib.parse.urlencode({"line": "无网络测试线", "km": "9.9"})
    started = time.time()
    km9_pending = json.loads(urllib.request.urlopen(f"http://{HOST}:{PORT}/api/mileage/lookup?{q}", timeout=3).read())
    assert km9_pending.get("pending") and time.time() - started < 2
    q_local = urllib.parse.urlencode({"line": "京广线", "km": "1130.1"})
    km9_local = json.loads(urllib.request.urlopen(f"http://{HOST}:{PORT}/api/mileage/lookup?{q_local}", timeout=3).read())
    assert km9_local["valid"] and km9_local["source"] == "LOCAL_MILEAGE_INTERPOLATED"
    print("Test KM9 passed: offline OSM path is asynchronous and local lookup continues")

    # KM10: browser supports a mileage-only marker and explicitly labels its source.
    assert "selectedCoordinate" in app and "LOCAL_MILEAGE_INTERPOLATED" in app
    assert "定位来源" in app and "本地公里标插值" in app and "trackCoordinate" in app
    print("Test KM10 passed: GPS-missing target remains drawable with explicit mileage source")

    # KM11: null coordinates must not become Number(null) == 0 and drag the
    # map to Null Island. Missing line names use a user-editable Hu-Kun default.
    assert "lon===null" in app and "lat===null" in app and "(lon===0&&lat===0)" in app
    assert "effectiveLine" in app and "railwayLineOverrides" in app
    assert 'id="default-line" value="沪昆线"' in index
    assert "应用默认线路" in index and "修改" in app and "自动" in app
    print("Test KM11 passed: invalid zero marker rejected and line fallback is editable")


if __name__ == "__main__":
    main()
