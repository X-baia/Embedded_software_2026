#!/usr/bin/env python3
"""Local-only smart alarm dashboard and API.

This server intentionally uses only the Python standard library so the project
can be run and tested on a LAN without internet access or package installs.
"""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import re
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse


BASE_DIR = Path(__file__).resolve().parent
STATE_DIR = BASE_DIR / "state"
SETTINGS_FILE = STATE_DIR / "alarm_settings.json"
STATUS_FILE = STATE_DIR / "device_status.json"

DEFAULT_SETTINGS: dict[str, Any] = {
    "enabled": False,
    "alarm_time": "07:00",
    "snooze_minutes": 5,
}

DEFAULT_STATUS: dict[str, Any] = {
    "device_id": "not-connected",
    "wifi_connected": False,
    "alarm_enabled": False,
    "alarm_time": "07:00",
    "snooze_minutes": 5,
    "ringing": False,
    "time_valid": False,
    "environment_valid": False,
    "last_error": "No device status received yet",
}

TIME_RE = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$")
STATE_LOCK = threading.Lock()


INDEX_HTML = r"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Alarm Clock</title>
  <style>
    :root {
      color-scheme: light dark;
      --bg: #f6f7f9;
      --panel: #ffffff;
      --text: #17202a;
      --muted: #637083;
      --border: #d9dee7;
      --accent: #1d7f72;
      --warn: #a35f00;
      --danger: #a33131;
      --ok: #216c42;
    }
    @media (prefers-color-scheme: dark) {
      :root {
        --bg: #111418;
        --panel: #191f26;
        --text: #ecf1f7;
        --muted: #a4afbf;
        --border: #313b48;
        --accent: #37b69f;
        --warn: #dfa141;
        --danger: #e06767;
        --ok: #63c58b;
      }
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      background: var(--bg);
      color: var(--text);
      font: 15px/1.45 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }
    main {
      width: min(1100px, calc(100vw - 32px));
      margin: 32px auto;
      display: grid;
      gap: 18px;
    }
    header {
      display: flex;
      align-items: end;
      justify-content: space-between;
      gap: 16px;
      border-bottom: 1px solid var(--border);
      padding-bottom: 14px;
    }
    h1 {
      margin: 0;
      font-size: clamp(28px, 4vw, 44px);
      line-height: 1.05;
      letter-spacing: 0;
    }
    .server-time {
      color: var(--muted);
      white-space: nowrap;
      font-variant-numeric: tabular-nums;
    }
    .grid {
      display: grid;
      grid-template-columns: minmax(0, 1.1fr) minmax(320px, .9fr);
      gap: 18px;
    }
    .test-grid {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 12px;
      align-items: end;
    }
    section {
      background: var(--panel);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 18px;
    }
    h2 {
      margin: 0 0 14px;
      font-size: 18px;
      letter-spacing: 0;
    }
    .status-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
    }
    .metric {
      min-height: 70px;
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 12px;
      display: grid;
      align-content: center;
      gap: 4px;
    }
    .label {
      color: var(--muted);
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: .06em;
    }
    .value {
      font-size: 18px;
      font-weight: 650;
      min-width: 0;
      overflow-wrap: anywhere;
    }
    .ok { color: var(--ok); }
    .warn { color: var(--warn); }
    .danger { color: var(--danger); }
    form {
      display: grid;
      gap: 14px;
    }
    label {
      display: grid;
      gap: 6px;
      font-weight: 600;
    }
    input[type="time"],
    input[type="number"],
    input[type="text"] {
      width: 100%;
      border: 1px solid var(--border);
      border-radius: 6px;
      padding: 10px 11px;
      background: transparent;
      color: var(--text);
      font: inherit;
    }
    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
    }
    .toggle {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 12px;
    }
    .toggle input {
      width: 22px;
      height: 22px;
      accent-color: var(--accent);
    }
    button {
      appearance: none;
      border: 0;
      border-radius: 6px;
      background: var(--accent);
      color: white;
      padding: 11px 14px;
      font: inherit;
      font-weight: 700;
      cursor: pointer;
    }
    button.secondary {
      background: transparent;
      color: var(--text);
      border: 1px solid var(--border);
    }
    button:disabled {
      opacity: .65;
      cursor: wait;
    }
    .message {
      min-height: 22px;
      color: var(--muted);
    }
    pre {
      margin: 0;
      overflow: auto;
      border-radius: 8px;
      border: 1px solid var(--border);
      padding: 12px;
      max-height: 300px;
      background: color-mix(in srgb, var(--panel), var(--bg) 55%);
      font-size: 12px;
    }
    @media (max-width: 760px) {
      main { width: min(100vw - 20px, 980px); margin: 18px auto; }
      header { align-items: start; flex-direction: column; }
      .grid { grid-template-columns: 1fr; }
      .test-grid { grid-template-columns: 1fr; }
      .status-grid { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <main>
    <header>
      <h1>Smart Alarm Clock</h1>
      <div id="serverTime" class="server-time">--</div>
    </header>

    <div class="grid">
      <section>
        <h2>Device Status</h2>
        <div class="status-grid">
          <div class="metric"><div class="label">Device</div><div id="deviceId" class="value">--</div></div>
          <div class="metric"><div class="label">Connection</div><div id="connection" class="value">--</div></div>
          <div class="metric"><div class="label">Alarm</div><div id="alarmState" class="value">--</div></div>
          <div class="metric"><div class="label">Current Time</div><div id="deviceTime" class="value">--</div></div>
          <div class="metric"><div class="label">Temperature</div><div id="temperature" class="value">--</div></div>
          <div class="metric"><div class="label">Humidity</div><div id="humidity" class="value">--</div></div>
          <div class="metric"><div class="label">Snooze Until</div><div id="snoozeUntil" class="value">--</div></div>
          <div class="metric"><div class="label">Last Upload</div><div id="lastUpload" class="value">--</div></div>
        </div>
      </section>

      <section>
        <h2>Alarm Settings</h2>
        <form id="settingsForm">
          <div class="toggle">
            <span>
              <strong>Alarm enabled</strong><br>
              <span class="label">Device applies this on the next poll</span>
            </span>
            <input id="enabled" type="checkbox" aria-label="Alarm enabled">
          </div>
          <label>
            Alarm time
            <input id="alarmTime" type="time" required>
          </label>
          <label>
            Snooze duration
            <input id="snoozeMinutes" type="number" min="1" max="120" step="1" required>
          </label>
          <button id="saveButton" type="submit">Save Settings</button>
          <div id="message" class="message"></div>
        </form>
      </section>
    </div>

    <section>
      <h2>Local Test Device</h2>
      <form id="simForm">
        <div class="test-grid">
          <label>
            Device id
            <input id="simDeviceId" type="text" value="browser-test-device" required>
          </label>
          <label>
            Temperature
            <input id="simTemperature" type="number" value="22.5" step="0.1" required>
          </label>
          <label>
            Humidity
            <input id="simHumidity" type="number" value="48" step="0.1" min="0" max="100" required>
          </label>
          <div class="toggle">
            <span><strong>Ringing</strong></span>
            <input id="simRinging" type="checkbox" aria-label="Simulated ringing state">
          </div>
        </div>
        <div class="actions">
          <button id="sendSimButton" type="submit">Send Simulated Status</button>
          <button id="fetchSettingsButton" class="secondary" type="button">Fetch Settings as Device</button>
        </div>
        <div id="simMessage" class="message"></div>
      </form>
      <pre id="settingsPreview">{}</pre>
    </section>

    <section>
      <h2>Latest Raw Status</h2>
      <pre id="rawStatus">{}</pre>
    </section>
  </main>

  <script>
    const byId = (id) => document.getElementById(id);

    function formatEpoch(value) {
      if (!value) return "--";
      return new Date(value * 1000).toLocaleString();
    }

    function setClass(el, className) {
      el.className = "value" + (className ? " " + className : "");
    }

    async function fetchJson(url, options) {
      const response = await fetch(url, options);
      const data = await response.json();
      if (!response.ok) {
        throw new Error(data.error || response.statusText);
      }
      return data;
    }

    async function loadSettings() {
      const settings = await fetchJson("/api/alarm-settings");
      byId("enabled").checked = Boolean(settings.enabled);
      byId("alarmTime").value = settings.alarm_time || "07:00";
      byId("snoozeMinutes").value = settings.snooze_minutes || 5;
      byId("serverTime").textContent = settings.server_iso || "--";
      byId("settingsPreview").textContent = JSON.stringify(settings, null, 2);
      return settings;
    }

    async function loadStatus() {
      const payload = await fetchJson("/api/status");
      const status = payload.status || {};
      byId("serverTime").textContent = payload.server_iso || "--";
      byId("deviceId").textContent = status.device_id || "--";

      const connection = byId("connection");
      connection.textContent = status.wifi_connected ? "Wi-Fi connected" : "Waiting";
      setClass(connection, status.wifi_connected ? "ok" : "warn");

      const alarmState = byId("alarmState");
      if (status.ringing) {
        alarmState.textContent = "Ringing";
        setClass(alarmState, "danger");
      } else {
        alarmState.textContent = `${status.alarm_enabled ? "Enabled" : "Disabled"} ${status.alarm_time || ""}`.trim();
        setClass(alarmState, status.alarm_enabled ? "ok" : "");
      }

      const deviceEpoch = status.device_epoch || 0;
      byId("deviceTime").textContent = status.time_valid ? formatEpoch(deviceEpoch) : "Not synced";
      byId("temperature").textContent = status.environment_valid && status.temperature_c !== undefined
        ? `${Number(status.temperature_c).toFixed(1)} C`
        : "--";
      byId("humidity").textContent = status.environment_valid && status.humidity_percent !== undefined
        ? `${Number(status.humidity_percent).toFixed(1)} %`
        : "--";
      byId("snoozeUntil").textContent = formatEpoch(status.snoozed_until_epoch);
      byId("lastUpload").textContent = status.received_at || "--";
      byId("rawStatus").textContent = JSON.stringify(status, null, 2);
    }

    byId("settingsForm").addEventListener("submit", async (event) => {
      event.preventDefault();
      const button = byId("saveButton");
      button.disabled = true;
      byId("message").textContent = "Saving...";
      try {
        await fetchJson("/api/alarm-settings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            enabled: byId("enabled").checked,
            alarm_time: byId("alarmTime").value,
            snooze_minutes: Number(byId("snoozeMinutes").value),
          }),
        });
        byId("message").textContent = "Saved. The ESP32 will fetch it on the next poll.";
        await loadSettings();
      } catch (error) {
        byId("message").textContent = error.message;
      } finally {
        button.disabled = false;
      }
    });

    async function sendSimulatedStatus() {
      const settings = await loadSettings();
      const status = {
        device_id: byId("simDeviceId").value.trim() || "browser-test-device",
        wifi_connected: true,
        alarm_enabled: Boolean(settings.enabled),
        alarm_time: settings.alarm_time || "07:00",
        alarm_hour: Number((settings.alarm_time || "07:00").slice(0, 2)),
        alarm_minute: Number((settings.alarm_time || "07:00").slice(3, 5)),
        snooze_minutes: Number(settings.snooze_minutes || 5),
        ringing: byId("simRinging").checked,
        snoozed_until_epoch: 0,
        last_alarm_epoch: byId("simRinging").checked ? Math.floor(Date.now() / 1000) : 0,
        last_settings_sync_epoch: Math.floor(Date.now() / 1000),
        last_status_upload_epoch: Math.floor(Date.now() / 1000),
        uptime_ms: Math.floor(performance.now()),
        time_valid: true,
        environment_valid: true,
        temperature_c: Number(byId("simTemperature").value),
        humidity_percent: Number(byId("simHumidity").value),
        device_epoch: Math.floor(Date.now() / 1000),
        last_error: "",
      };

      await fetchJson("/api/status", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(status),
      });
      byId("simMessage").textContent = "Simulated status uploaded.";
      await loadStatus();
    }

    byId("simForm").addEventListener("submit", async (event) => {
      event.preventDefault();
      const button = byId("sendSimButton");
      button.disabled = true;
      byId("simMessage").textContent = "Uploading simulated status...";
      try {
        await sendSimulatedStatus();
      } catch (error) {
        byId("simMessage").textContent = error.message;
      } finally {
        button.disabled = false;
      }
    });

    byId("fetchSettingsButton").addEventListener("click", async () => {
      const button = byId("fetchSettingsButton");
      button.disabled = true;
      byId("simMessage").textContent = "Fetching settings...";
      try {
        await loadSettings();
        byId("simMessage").textContent = "Settings fetched from the same endpoint used by the ESP32.";
      } catch (error) {
        byId("simMessage").textContent = error.message;
      } finally {
        button.disabled = false;
      }
    });

    async function refreshAll() {
      try {
        await loadStatus();
      } catch (error) {
        byId("message").textContent = error.message;
      }
    }

    loadSettings().catch((error) => byId("message").textContent = error.message);
    refreshAll();
    setInterval(refreshAll, 2000);
  </script>
</body>
</html>
"""


def now_payload() -> dict[str, Any]:
    return {
        "server_epoch": int(time.time()),
        "server_iso": time.strftime("%Y-%m-%d %H:%M:%S %Z"),
    }


def ensure_state_dir() -> None:
    STATE_DIR.mkdir(parents=True, exist_ok=True)


def load_json(path: Path, default: dict[str, Any]) -> dict[str, Any]:
    ensure_state_dir()
    if not path.exists():
        save_json(path, default)
        return dict(default)
    try:
        with path.open("r", encoding="utf-8") as file:
            data = json.load(file)
        if isinstance(data, dict):
            return data
    except (OSError, json.JSONDecodeError):
        pass
    return dict(default)


def save_json(path: Path, data: dict[str, Any]) -> None:
    ensure_state_dir()
    temp_path = path.with_suffix(path.suffix + ".tmp")
    with temp_path.open("w", encoding="utf-8") as file:
        json.dump(data, file, indent=2, sort_keys=True)
        file.write("\n")
    os.replace(temp_path, path)


def validate_settings(payload: dict[str, Any]) -> dict[str, Any]:
    enabled = bool(payload.get("enabled", False))
    alarm_time = str(payload.get("alarm_time", "")).strip()
    if not TIME_RE.match(alarm_time):
        raise ValueError("alarm_time must use HH:MM in 24-hour format")

    try:
        snooze_minutes = int(payload.get("snooze_minutes", 5))
    except (TypeError, ValueError) as exc:
        raise ValueError("snooze_minutes must be an integer") from exc

    if snooze_minutes < 1 or snooze_minutes > 120:
        raise ValueError("snooze_minutes must be between 1 and 120")

    return {
        "enabled": enabled,
        "alarm_time": alarm_time,
        "snooze_minutes": snooze_minutes,
        "updated_at": time.strftime("%Y-%m-%d %H:%M:%S %Z"),
    }


class AlarmRequestHandler(BaseHTTPRequestHandler):
    server_version = "SmartAlarmLocal/1.0"

    def log_message(self, format: str, *args: Any) -> None:
        print(f"{self.address_string()} - {format % args}")

    def do_OPTIONS(self) -> None:
        self.send_response(HTTPStatus.NO_CONTENT)
        self.send_common_headers()
        self.end_headers()

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/":
            self.send_html(INDEX_HTML)
            return
        if path == "/api/alarm-settings":
            with STATE_LOCK:
                settings = load_json(SETTINGS_FILE, DEFAULT_SETTINGS)
            settings.update(now_payload())
            self.send_json(settings)
            return
        if path == "/api/status":
            with STATE_LOCK:
                status = load_json(STATUS_FILE, DEFAULT_STATUS)
            payload = {"status": status}
            payload.update(now_payload())
            self.send_json(payload)
            return

        self.send_error_json(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        if path == "/api/alarm-settings":
            self.handle_settings_post()
            return
        if path == "/api/status":
            self.handle_status_post()
            return

        self.send_error_json(HTTPStatus.NOT_FOUND, "Not found")

    def handle_settings_post(self) -> None:
        try:
            payload = self.read_json_body()
            settings = validate_settings(payload)
        except ValueError as exc:
            self.send_error_json(HTTPStatus.BAD_REQUEST, str(exc))
            return

        with STATE_LOCK:
            save_json(SETTINGS_FILE, settings)

        response = dict(settings)
        response.update(now_payload())
        self.send_json(response)

    def handle_status_post(self) -> None:
        try:
            payload = self.read_json_body()
        except ValueError as exc:
            self.send_error_json(HTTPStatus.BAD_REQUEST, str(exc))
            return

        if not isinstance(payload, dict):
            self.send_error_json(HTTPStatus.BAD_REQUEST, "JSON body must be an object")
            return

        payload["received_at"] = time.strftime("%Y-%m-%d %H:%M:%S %Z")
        payload["received_epoch"] = int(time.time())

        with STATE_LOCK:
            save_json(STATUS_FILE, payload)

        self.send_json({"ok": True, **now_payload()})

    def read_json_body(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            raise ValueError("Request body is empty")
        if length > 32768:
            raise ValueError("Request body is too large")

        raw = self.rfile.read(length)
        try:
            payload = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("Request body must be valid JSON") from exc
        if not isinstance(payload, dict):
            raise ValueError("JSON body must be an object")
        return payload

    def send_common_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Cache-Control", "no-store")

    def send_html(self, body: str) -> None:
        encoded = body.encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_common_headers()
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def send_json(self, payload: dict[str, Any], status: HTTPStatus = HTTPStatus.OK) -> None:
        encoded = json.dumps(payload, indent=2, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_common_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def send_error_json(self, status: HTTPStatus, message: str) -> None:
        self.send_json({"error": message, **now_payload()}, status)


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the local smart alarm dashboard/API.")
    parser.add_argument("--host", default="0.0.0.0", help="Host/interface to bind")
    parser.add_argument("--port", type=int, default=8000, help="TCP port to listen on")
    args = parser.parse_args()

    mimetypes.add_type("application/json", ".json")
    ensure_state_dir()
    load_json(SETTINGS_FILE, DEFAULT_SETTINGS)
    load_json(STATUS_FILE, DEFAULT_STATUS)

    server = ThreadingHTTPServer((args.host, args.port), AlarmRequestHandler)
    print(f"Smart alarm dashboard running at http://{args.host}:{args.port}")
    print("Use your computer's LAN IP in the ESP32 server URL, for example http://192.168.1.23:8000")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping dashboard")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
