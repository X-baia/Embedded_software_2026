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

The page is both the alarm dashboard and a small local test app.

### 2. Test without the ESP32

Use the browser page first:

1. In `Alarm Settings`, choose an alarm time, enable or disable the alarm, set snooze minutes, and press `Save Settings`.
2. In `Local Test Device`, press `Fetch Settings as Device`. This shows the JSON the ESP32 firmware will fetch from `GET /api/alarm-settings`.
3. Set a fake temperature/humidity and press `Send Simulated Status`.
4. Check `Device Status` and `Latest Raw Status`. They should update immediately.
5. Enable `Ringing` in `Local Test Device` and send again to test the dashboard's ringing state.

This confirms the local server, dashboard, settings endpoint, and status endpoint all work before flashing hardware.

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
  "server_epoch": 1714470000,
  "server_iso": "2026-04-30 09:00:00 CEST"
}
```

`POST /api/alarm-settings`

Saves alarm settings from the dashboard or curl:

```sh
curl -X POST http://localhost:8000/api/alarm-settings \
  -H 'Content-Type: application/json' \
  -d '{"enabled":true,"alarm_time":"07:30","snooze_minutes":5}'
```

`GET /api/status`

Returns the latest uploaded ESP32 status.

`POST /api/status`

Used by the ESP32 to upload status. You can simulate it locally:

```sh
curl -X POST http://localhost:8000/api/status \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"test-device","wifi_connected":true,"alarm_enabled":true,"alarm_time":"07:30","snooze_minutes":5,"ringing":false,"time_valid":true,"environment_valid":true,"temperature_c":22.5,"humidity_percent":48.0}'
```

## Hardware behavior

- DFPlayer Mini is controlled on UART1 with TX GPIO 17 and RX GPIO 16.
- The alarm plays track `1` when ringing.
- The button on GPIO 4 snoozes the alarm when it is ringing.
- The AHT20 sensor is read on I2C SDA GPIO 21 and SCL GPIO 22.

## Offline testing checklist

1. Start `server/local_dashboard.py`.
2. Save alarm settings in the dashboard.
3. Confirm `curl http://localhost:8000/api/alarm-settings` returns the saved settings.
4. Configure ESP32 server URL with your computer's LAN IP.
5. Flash and monitor the ESP32.
6. Confirm logs show Wi-Fi connected, settings polling, clock synchronization, and status uploads.
7. Confirm the dashboard shows the ESP32 device id, status, temperature/humidity, and alarm state.
