# Smart Alarm Clock

Local-only ESP32 smart alarm clock project. The ESP32 connects to your Wi-Fi LAN, polls a local dashboard for alarm settings, uploads device status, and rings a DFPlayer alarm track when the configured local time is reached.

No internet or cloud service is required.

## Quick tutorial

### 1. Run the local web app

From the project folder:

```sh
python3 server/local_dashboard.py --host 0.0.0.0 --port 8000
```

Open this on the same computer:

```text
http://localhost:8000
```

The page is the monitoring dashboard, alarm editor, and sleep schedule planner.

### 2. Configure alarms in the dashboard

Use the browser page to prepare the device settings:

1. In `Alarms`, add one or more alarms, set their times, snooze durations, ringtone tracks, enabled state, and active days.
2. In `Sleep Schedule`, tune the age group, fall-asleep delay, cycle length, and preferred number of sleep cycles.
3. Use a recommended sleep-cycle time to add an alarm, then press `Save`.
4. Keep the page open while the ESP32 is running so it can show live status from the board.

The ESP32 fetches the saved settings from `GET /api/alarm-settings` and uploads live monitoring data to `POST /api/status`.

### 3. Find your computer LAN IP

The ESP32 cannot use `localhost`, because `localhost` would mean the ESP32 itself. Use your computer's LAN IP.

On macOS, usually:

```sh
ipconfig getifaddr en0
```

If you are on Ethernet, try:

```sh
ipconfig getifaddr en1
```

Example result:

```text
192.168.1.23
```

Then the ESP32 server URL should be:

```text
http://192.168.1.23:8000
```

### 4. Configure and flash the ESP32

In an ESP-IDF terminal:

```sh
idf.py menuconfig
```

Open `Smart Alarm Clock` and set:

- `Wi-Fi SSID`
- `Wi-Fi password`
- `Local dashboard/API base URL`, for example `http://192.168.1.23:8000`

Then build and flash:

```sh
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

Keep `server/local_dashboard.py` running while the ESP32 is on. The ESP32 polls settings and uploads status every few seconds.

## Project layout

- `main/`: ESP-IDF firmware.
- `main/alarm_manager.*`: alarm settings, ringing/snooze state, clock sync, sensor status.
- `main/wifi_manager.*`: Wi-Fi station connection and reconnect handling.
- `main/network_client.*`: HTTP polling/upload to the local dashboard API.
- `server/local_dashboard.py`: offline dashboard and REST API using Python standard library only.

## Local dashboard/API reference

Start the dashboard on the computer that is on the same Wi-Fi/LAN as the ESP32:

```sh
python3 server/local_dashboard.py --host 0.0.0.0 --port 8000
```

Open `http://localhost:8000` in a browser on that computer.

For the ESP32, use the computer's LAN IP address, not `localhost`. Example:

```text
http://192.168.1.23:8000
```

The server stores local state in `server/state/`.

## Firmware configuration

Configure the ESP32 firmware with ESP-IDF:

```sh
idf.py menuconfig
```

Open `Smart Alarm Clock` and set:

- `Wi-Fi SSID`
- `Wi-Fi password`
- `Local dashboard/API base URL`, for example `http://192.168.1.23:8000`
- Optional defaults such as device id, alarm time, snooze duration, timezone, and sync periods

Do not put private Wi-Fi credentials directly in `main/Kconfig.projbuild`. That file defines menu defaults and is part of the source tree. The values used by the firmware come from the generated local `sdkconfig`, which is created/updated by `idf.py menuconfig`.

To confirm what the firmware will compile with:

```sh
grep SMART_ALARM_WIFI sdkconfig
```

If it still shows placeholder values, run `idf.py menuconfig`, save the settings, then rebuild and flash.

The default timezone is for Italy/Central Europe:

```text
CET-1CEST,M3.5.0/2,M10.5.0/3
```

## Build, flash, monitor

```sh
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

If you see `ninja: error: loading 'build.ninja': No such file or directory`, the `build/` directory was not generated successfully. Clean the partial build output and let ESP-IDF configure again:

```sh
rm -rf build
. /Users/daniellancorai/esp/v6.0.1/esp-idf/export.sh
idf.py set-target esp32
idf.py reconfigure
idf.py build
```

In VS Code, the equivalent is `ESP-IDF: Full Clean Project`, then `ESP-IDF: Build Project`.

If flashing fails with `Could not open /dev/ttyUSB1`, the selected serial port is wrong for macOS. List available ports:

```sh
ls /dev/cu.*
```

Use the USB serial device, for example:

```sh
idf.py -p /dev/cu.usbserial-210 flash monitor
```

If the port is busy, close any open serial monitor first, then retry.

If Wi-Fi logs show `reason=201 (NO_AP_FOUND)`, the ESP32 cannot see any nearby access point with the configured SSID. Check:

- SSID spelling and capitalization exactly match the phone/router network name.
- iPhone hotspot is enabled with `Allow Others to Join`.
- iPhone `Maximize Compatibility` is enabled so the hotspot uses 2.4 GHz.
- Keep the iPhone hotspot settings screen open while the ESP32 connects.
- ESP32 cannot connect to a 5 GHz-only network.

Expected behavior:

1. ESP32 starts hardware tasks for I2C sensor, button, audio, alarm, Wi-Fi, and network sync.
2. ESP32 connects to the configured Wi-Fi.
3. ESP32 polls `GET /api/alarm-settings` and syncs its clock from `server_epoch`.
4. ESP32 uploads status to `POST /api/status`.
5. The dashboard updates every 2 seconds.

## API endpoints

`GET /api/alarm-settings`

Returns alarm settings for the device:

```json
{
  "enabled": true,
  "alarm_time": "07:30",
  "snooze_minutes": 5,
  "alarms": [
    {
      "id": "alarm-1",
      "label": "Morning",
      "enabled": true,
      "alarm_time": "07:30",
      "snooze_minutes": 5,
      "track": 1,
      "days_mask": 127
    }
  ],
  "sleep_profile": {
    "age_group": "adult_18_60",
    "time_to_fall_asleep": 15,
    "cycle_minutes": 90,
    "preferred_cycles": 5,
    "schedule_mode": "wake_at",
    "wake_time": "07:00",
    "bed_time": "23:00",
    "everyday": true
  },
  "server_epoch": 1714470000,
  "server_iso": "2026-04-30 09:00:00 CEST"
}
```

`POST /api/alarm-settings`

Saves alarm settings from the dashboard or curl:

```sh
curl -X POST http://localhost:8000/api/alarm-settings \
  -H 'Content-Type: application/json' \
  -d '{"alarms":[{"id":"alarm-1","label":"Morning","enabled":true,"alarm_time":"07:30","snooze_minutes":5,"track":1,"days_mask":127}]}'
```

`GET /api/tracks`

Returns the browser-previewable ringtone files found in `server/tracks`. Name files with the DFPlayer track number first, for example `001-morning.mp3`, `002-bells.mp3`, or `003-radio.wav`.

For the physical DFPlayer Mini SD card, put the alarm files in an `MP3` folder using four digit names:

```text
MP3/0001.mp3
MP3/0002.mp3
MP3/0003.mp3
MP3/0004.mp3
```

The website preview files can have descriptive names, but the DFPlayer SD card should use the numbered `MP3/000N.mp3` names so track selection is deterministic.

`GET /api/status`

Returns the latest uploaded ESP32 status.

`POST /api/status`

Used by the ESP32 to upload live status, including temperature, humidity, ENS160 air quality data, alarm activity, and connectivity.

## Hardware behavior

- DFPlayer Mini is controlled on UART1 with TX GPIO 17 and RX GPIO 16.
- Each alarm plays its configured DFPlayer `MP3/000N.mp3` track number when ringing.
- The button on GPIO 4 snoozes ringing alarms on one press and stops active/snoozed alarms on a second press within 2 seconds.
- The AHT20 sensor is read on I2C SDA GPIO 21 and SCL GPIO 22.
- The optional ENS160 air quality sensor is read on the same I2C bus at address `0x52` or `0x53`.

## Local integration checklist

1. Start `server/local_dashboard.py`.
2. Save alarm settings in the dashboard.
3. Confirm `curl http://localhost:8000/api/alarm-settings` returns the saved settings.
4. Configure ESP32 server URL with your computer's LAN IP.
5. Flash and monitor the ESP32.
6. Confirm logs show Wi-Fi connected, settings polling, clock synchronization, and status uploads.
7. Confirm the dashboard shows the ESP32 device id, status, temperature/humidity, air quality, and alarm state.
