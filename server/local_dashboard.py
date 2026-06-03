#!/usr/bin/env python3
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
from urllib.parse import quote, unquote, urlparse

#directories and defaults
BASE_DIR = Path(__file__).resolve().parent
STATE_DIR = BASE_DIR / "state"
SETTINGS_FILE = STATE_DIR / "alarm_settings.json"
STATUS_FILE = STATE_DIR / "device_status.json"
DASHBOARD_FILE = BASE_DIR / "dashboard.html"
TRACKS_DIR = BASE_DIR / "tracks"
CSS_DIR = BASE_DIR / "style.css"
SCRIPT_DIR = BASE_DIR / "script.js"
MAX_ALARMS = 8
ALARM_DAYS_EVERYDAY = 0x7F
#looks for audio that starts with a 1 to 3 digit number and has one of those extensions
TRACK_FILE_RE = re.compile(r"^(\d{1,3})[-_\s]?.*\.(mp3|mpeg|wav|ogg|m4a)$", re.IGNORECASE) 

#default settings of gui
DEFAULT_SETTINGS: dict[str, Any] = {
    "alarms": [
        {
            "id": "alarm-1",
            "label": "Morning",
            "enabled": False,
            "alarm_time": "07:00",
            "snooze_minutes": 5,
            "volume": 20,
            "track": 1,
            "days_mask": 127,
        }
    ],
    "comfort_min_temperature": 18.0,
    "comfort_max_temperature": 26.0,
    "sleep_profile": {
        "age_group": "adult_18_60",
        "time_to_fall_asleep": 15,
        "cycle_minutes": 90,
        "preferred_cycles": 5,
        "schedule_mode": "wake_at",
        "wake_time": "07:00",
        "bed_time": "23:00",
        "everyday": True,
    },
}

DEFAULT_STATUS: dict[str, Any] = {
    "device_id": "not-connected",
    "wifi_connected": False,
    "alarm_enabled": False,
    "alarm_time": "07:00",
    "snooze_minutes": 5,
    "volume": 20,
    "track": 1,
    "ringing": False,
    "alarm_count": 0,
    "active_alarm_count": 0,
    "comfort_min_temperature": 18.0,
    "comfort_max_temperature": 26.0,
    "alarms": [],
    "time_valid": False,
    "environment_valid": False,
    "air_quality_valid": False,
    "air_quality_index": 0,
    "eco2_ppm": 0,
    "tvoc_ppb": 0,
    "last_error": "No device status received yet",
}
#validattes if the string has a valid HH:HH format
TIME_RE = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$")
STATE_LOCK = threading.Lock()

#setting up how many hours a person should sleep based on the age group
AGE_SLEEP_HOURS: dict[str, tuple[int, int]] = {
    "newborn": (14, 17),
    "infant": (12, 16),
    "toddler": (11, 14),
    "preschool": (10, 13),
    "school_age": (9, 12),
    "teen": (8, 10),
    "adult_18_60": (7, 9),
    "adult_61_64": (7, 9),
    "adult_65": (7, 8),
}


FALLBACK_INDEX_HTML = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Alarm Clock</title>
</head>
<body>
  <main>
    <h1>Smart Alarm Clock</h1>
    <p>Dashboard asset missing. Start the server from the project checkout so server/dashboard.html can be served.</p>
  </main>
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

def ensure_tracks_dir() -> None:
    TRACKS_DIR.mkdir(parents=True, exist_ok=True)

#functions to load and save json data for settings and status, with error handling and defaults
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

#function to read the dashboard HTML file
def dashboard_html() -> str:
    try:
        return DASHBOARD_FILE.read_text(encoding="utf-8")
    except OSError:
        return FALLBACK_INDEX_HTML

#function to list available tracks in the tracks directory, extracting track number and label from filenames
def list_tracks() -> list[dict[str, Any]]:
    ensure_tracks_dir()
    tracks: dict[int, dict[str, Any]] = {}
    for path in TRACKS_DIR.iterdir():
        if not path.is_file():
            continue
        match = TRACK_FILE_RE.match(path.name)
        if not match:
            continue
        number = int(match.group(1))
        if number < 1 or number > 255:
            continue
        label_source = path.stem[len(match.group(1)):].lstrip("-_ ") or f"Track {number}"
        for suffix in (".mp3", ".mpeg", ".wav", ".ogg", ".m4a"):
            if label_source.lower().endswith(suffix):
                label_source = label_source[: -len(suffix)]
                break
        tracks[number] = {
            "number": number,
            "label": label_source,
            "file": path.name,
            "url": f"/tracks/{quote(path.name)}",
        }
    if tracks:
        return [tracks[number] for number in sorted(tracks)]

    return [{"number": number, "label": f"Track {number}", "file": "", "url": ""} for number in range(1, 9)]

def parse_int(value: Any, default: int, minimum: int, maximum: int, name: str) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if parsed < minimum or parsed > maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return parsed

def parse_float(value: Any, default: float, minimum: float, maximum: float, name: str) -> float:
    # The dashboard stores comfort limits as plain numbers, so we accept integers or decimals here.
    try:
        parsed = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be a number") from exc
    if parsed < minimum or parsed > maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return parsed

def preferred_cycles_for_age(age_group: str) -> int:
    min_hours, max_hours = AGE_SLEEP_HOURS.get(age_group, AGE_SLEEP_HOURS["adult_18_60"])
    midpoint_minutes = ((min_hours + max_hours) / 2) * 60
    return max(3, min(12, round(midpoint_minutes / 90)))

#functions to noralize to defaults, ensuring correcting formats are used
def normalize_alarm(raw: Any, index: int, previous: dict[str, Any] | None = None) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise ValueError("Each alarm must be an object")
    if not isinstance(previous, dict):
        previous = {}

    alarm_time = str(raw.get("alarm_time", "07:00")).strip()
    if not TIME_RE.match(alarm_time):
        raise ValueError("alarm_time must use HH:MM in 24-hour format")

    label = str(raw.get("label", f"Alarm {index + 1}")).strip()[:23] or f"Alarm {index + 1}"
    alarm_id = str(raw.get("id", f"alarm-{index + 1}")).strip()[:40] or f"alarm-{index + 1}"

    return {
        "id": alarm_id,
        "label": label,
        "enabled": bool(raw.get("enabled", False)),
        "alarm_time": alarm_time,
        "snooze_minutes": parse_int(raw.get("snooze_minutes", 5), 5, 1, 120, "snooze_minutes"),
        "volume": parse_int(raw.get("volume", previous.get("volume", 20)), 20, 0, 30, "volume"),
        "track": parse_int(raw.get("track", previous.get("track", 1)), 1, 1, 255, "track"),
        "days_mask": parse_int(raw.get("days_mask", ALARM_DAYS_EVERYDAY), ALARM_DAYS_EVERYDAY, 1, ALARM_DAYS_EVERYDAY, "days_mask"),
    }

def normalize_sleep_profile(raw: Any) -> dict[str, Any]:
    default = DEFAULT_SETTINGS["sleep_profile"]
    if not isinstance(raw, dict):
        raw = {}

    wake_time = str(raw.get("wake_time", default["wake_time"])).strip()
    bed_time = str(raw.get("bed_time", default["bed_time"])).strip()
    if not TIME_RE.match(wake_time):
        raise ValueError("wake_time must use HH:MM in 24-hour format")
    if not TIME_RE.match(bed_time):
        raise ValueError("bed_time must use HH:MM in 24-hour format")

    schedule_mode = str(raw.get("schedule_mode", default["schedule_mode"]))
    if schedule_mode not in {"wake_at", "bed_at"}:
        schedule_mode = default["schedule_mode"]

    return {
        "age_group": str(raw.get("age_group", default["age_group"]))[:32],
        "time_to_fall_asleep": parse_int(raw.get("time_to_fall_asleep", default["time_to_fall_asleep"]), default["time_to_fall_asleep"], 0, 90, "time_to_fall_asleep"),
        "cycle_minutes": 90,
        "preferred_cycles": preferred_cycles_for_age(str(raw.get("age_group", default["age_group"]))[:32]),
        "schedule_mode": schedule_mode,
        "wake_time": wake_time,
        "bed_time": bed_time,
        "everyday": bool(raw.get("everyday", default["everyday"])),
    }

def normalize_settings(payload: dict[str, Any], previous_settings: dict[str, Any] | None = None) -> dict[str, Any]:
    previous_alarms = []
    if isinstance(previous_settings, dict) and isinstance(previous_settings.get("alarms"), list):
        previous_alarms = previous_settings["alarms"]

    previous_comfort_min = DEFAULT_SETTINGS["comfort_min_temperature"]
    previous_comfort_max = DEFAULT_SETTINGS["comfort_max_temperature"]
    if isinstance(previous_settings, dict):
        previous_comfort_min = previous_settings.get("comfort_min_temperature", previous_comfort_min)
        previous_comfort_max = previous_settings.get("comfort_max_temperature", previous_comfort_max)

    if "alarms" in payload:
        raw_alarms = payload.get("alarms")
        if not isinstance(raw_alarms, list):
            raise ValueError("alarms must be an array")
        alarms = [
            normalize_alarm(
                raw_alarm,
                index,
                previous_alarms[index] if index < len(previous_alarms) else None,
            )
            for index, raw_alarm in enumerate(raw_alarms[:MAX_ALARMS])
        ]
    else:
        alarms = [
            normalize_alarm(
                {
                    "id": "alarm-1",
                    "label": "Morning",
                    "enabled": payload.get("enabled", False),
                    "alarm_time": payload.get("alarm_time", "07:00"),
                    "snooze_minutes": payload.get("snooze_minutes", 5),
                    "volume": payload.get("volume", 20),
                    "track": payload.get("track", 1),
                    "days_mask": ALARM_DAYS_EVERYDAY,
                },
                0,
            )
        ]

    comfort_min_temperature = parse_float(
        payload.get("comfort_min_temperature", previous_comfort_min),
        previous_comfort_min,
        -20.0,
        60.0,
        "comfort_min_temperature",
    )
    comfort_max_temperature = parse_float(
        payload.get("comfort_max_temperature", previous_comfort_max),
        previous_comfort_max,
        -20.0,
        60.0,
        "comfort_max_temperature",
    )
    if comfort_min_temperature > comfort_max_temperature:
        # Swapping the bounds is easier for the user than rejecting the whole save.
        comfort_min_temperature, comfort_max_temperature = comfort_max_temperature, comfort_min_temperature

    normalized = {
        "alarms": alarms,
        "comfort_min_temperature": comfort_min_temperature,
        "comfort_max_temperature": comfort_max_temperature,
        "sleep_profile": normalize_sleep_profile(payload.get("sleep_profile")),
    }
    if "updated_at" in payload:
        normalized["updated_at"] = payload["updated_at"]
    return normalized

def settings_response(settings: dict[str, Any]) -> dict[str, Any]:
    response = normalize_settings(settings)
    first_alarm = response["alarms"][0] if response["alarms"] else normalize_alarm(DEFAULT_SETTINGS["alarms"][0], 0)
    response.update(
        {
            "enabled": first_alarm["enabled"],
            "alarm_time": first_alarm["alarm_time"],
            "snooze_minutes": first_alarm["snooze_minutes"],
            "volume": first_alarm["volume"],
            "track": first_alarm["track"],
        }
    )
    return response

#ensures incoming settings are valid
def validate_settings(payload: dict[str, Any], previous_settings: dict[str, Any] | None = None) -> dict[str, Any]:
    settings = normalize_settings(payload, previous_settings)
    settings.update(
        {
            "updated_at": time.strftime("%Y-%m-%d %H:%M:%S %Z"),
        }
    )
    return settings


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
            self.send_html(dashboard_html())
            return
        if path == "/style.css":
            self.send_css(CSS_DIR)
            return
        if path == "/script.js":
            self.send_script(SCRIPT_DIR)
            return
        if path == "/api/alarm-settings":
            with STATE_LOCK:
                settings = settings_response(load_json(SETTINGS_FILE, DEFAULT_SETTINGS))
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
        if path == "/api/tracks":
            self.send_json({"tracks": list_tracks(), **now_payload()})
            return
        if path.startswith("/tracks/"):
            self.send_track(path)
            return

        self.send_error_json(HTTPStatus.NOT_FOUND, "Not found")

    #handles incoming status updates from the device, validating and saving them
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
            with STATE_LOCK:
                previous_settings = load_json(SETTINGS_FILE, DEFAULT_SETTINGS)
            settings = validate_settings(payload, previous_settings)
        except ValueError as exc:
            self.send_error_json(HTTPStatus.BAD_REQUEST, str(exc))
            return

        with STATE_LOCK:
            save_json(SETTINGS_FILE, settings)

        response = settings_response(settings)
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

    #reads and parses JSON body from incoming POST requests
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

    # Sends common HTTP headers for all responses
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

    def send_css(self, path: Path) -> None:
        try:
            css_content = (CSS_DIR).read_text(encoding="utf-8")
            self.send_response(HTTPStatus.OK)
            self.send_common_headers()
            self.send_header("Content-Type", "text/css; charset=utf-8")
            self.end_headers()
            self.wfile.write(css_content.encode("utf-8"))
        except OSError:
            self.send_error_json(HTTPStatus.NOT_FOUND, "CSS file not found")
            return

    def send_script(self, path: Path) -> None:
        try:
            script_content = (SCRIPT_DIR).read_text(encoding="utf-8")
            self.send_response(HTTPStatus.OK)
            self.send_common_headers()
            self.send_header("Content-Type", "application/javascript; charset=utf-8")
            self.end_headers()
            self.wfile.write(script_content.encode("utf-8"))
        except OSError:
            self.send_error_json(HTTPStatus.NOT_FOUND, "Script file not found")
            return

    def send_track(self, path: str) -> None:
        ensure_tracks_dir()
        requested = Path(unquote(urlparse(path).path)).name
        file_path = TRACKS_DIR / requested
        try:
            resolved = file_path.resolve(strict=True)
            tracks_root = TRACKS_DIR.resolve(strict=True)
        except OSError:
            self.send_error_json(HTTPStatus.NOT_FOUND, "Track not found")
            return

        if tracks_root not in resolved.parents or not resolved.is_file():
            self.send_error_json(HTTPStatus.NOT_FOUND, "Track not found")
            return

        content_type = "audio/mpeg" if resolved.suffix.lower() == ".mpeg" else mimetypes.guess_type(resolved.name)[0] or "application/octet-stream"
        try:
            data = resolved.read_bytes()
        except OSError:
            self.send_error_json(HTTPStatus.NOT_FOUND, "Track not found")
            return

        self.send_response(HTTPStatus.OK)
        self.send_common_headers()
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

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

#set ups the server and web app
def main() -> None:
    parser = argparse.ArgumentParser(description="Run the local smart alarm dashboard/API.")
    parser.add_argument("--host", default="0.0.0.0", help="Host/interface to bind")
    parser.add_argument("--port", type=int, default=8000, help="TCP port to listen on")
    args = parser.parse_args()

    mimetypes.add_type("application/json", ".json")
    mimetypes.add_type("audio/mpeg", ".mpeg")
    ensure_state_dir()
    ensure_tracks_dir()
    with STATE_LOCK:
        save_json(SETTINGS_FILE, normalize_settings(load_json(SETTINGS_FILE, DEFAULT_SETTINGS)))
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
