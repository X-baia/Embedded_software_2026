# Smart Alarm Clock

Smart Alarm Clock is a local-only ESP32 alarm clock that combines alarm
scheduling, environmental sensing, Wi-Fi synchronization, physical button control,
and DFPlayer Mini audio playback.

The ESP32 connects to a local Wi-Fi network, periodically downloads alarm settings
from a dashboard running on a computer, uploads live device status, reads sensors
over I2C, and plays the selected alarm track when an alarm condition is reached.

No cloud service is required.

<!--=========================================================================-->

The main components of the system are the ESP32 firmware in [`main/`](./main) and
the local dashboard/API in [`server/`](./server).

```text
                         Wi-Fi / HTTP
+-------------------+  <------------->  +-------------------------+
| ESP32 Firmware    |                   | Local Web Dashboard     |
| FreeRTOS tasks    |                   | Alarm editor + REST API |
+---------+---------+                   +-------------------------+
          |
          +-- I2C  --> AHT20 temperature/humidity sensor
          |
          +-- I2C  --> ENS160 air-quality sensor, optional
          |
          +-- GPIO --> User button on GPIO 4
          |
          +-- UART --> DFPlayer Mini audio module
```

The system is designed around small firmware modules:

- [`wifi_manager`](./main/wifi_manager.c): Wi-Fi station setup and reconnect handling.
- [`network_client`](./main/network_client.c): HTTP communication with the local dashboard.
- [`alarm_manager`](./main/alarm_manager.c): alarm state, scheduling, snooze/stop logic, and shared status.
- [`i2c_sensors`](./main/i2c_sensors.c): AHT20 and ENS160 sensor acquisition.
- [`user_button`](./main/user_button.c): button polling, debounce, snooze, and stop behavior.
- [`audio_player`](./main/audio_player.c): DFPlayer Mini UART commands.
- [`local_dashboard.py`](./server/local_dashboard.py): local web server and REST API.

## Roadmap for Running the Project

1. Start the local dashboard on a computer connected to the same Wi-Fi/LAN as the ESP32.
2. Configure alarms, tracks, and sleep schedule settings in the dashboard.
3. Configure the ESP32 firmware with Wi-Fi credentials and the dashboard URL.
4. Build the firmware with ESP-IDF.
5. Burn/flash the firmware to the ESP32.
6. Monitor the serial output and verify that the dashboard receives live status.
7. Test the alarm behavior using the DFPlayer Mini and the physical button.

<!--=========================================================================-->

## Requirements

### Hardware Requirements

The project requires the following hardware:

- ESP32 development board.
- AHT20 temperature/humidity sensor.
- ENS160 air-quality sensor, optional but supported.
- DFPlayer Mini MP3 module.
- MicroSD card for the DFPlayer Mini.
- Speaker compatible with the DFPlayer Mini.
- Push button.
- Breadboard and jumper wires.
- USB cable for flashing and serial monitoring.
- Stable power supply for the ESP32 and connected peripherals.

### Hardware Connections

| Component | ESP32 Pin | Description |
|---|---:|---|
| AHT20 SDA | GPIO 21 | I2C data |
| AHT20 SCL | GPIO 22 | I2C clock |
| ENS160 SDA | GPIO 21 | Same I2C bus, optional |
| ENS160 SCL | GPIO 22 | Same I2C bus, optional |
| Button | GPIO 4 | Active-low input, internal pull-up enabled |
| DFPlayer RX | GPIO 17 | ESP32 UART1 TX |
| DFPlayer TX | GPIO 16 | ESP32 UART1 RX |
| DFPlayer speaker output | Speaker | Alarm audio output |

The AHT20 uses I2C address `0x38`. The ENS160 is detected at address `0x52` or
`0x53`.

For the physical DFPlayer Mini SD card, put alarm files in an `MP3` folder using
four-digit names:

```text
MP3/0001.mp3
MP3/0002.mp3
MP3/0003.mp3
MP3/0004.mp3
```

The track number selected in the dashboard corresponds to the DFPlayer track
number.

### Software Requirements

- ESP-IDF installed and exported in the terminal.
- Python 3.
- A modern browser.
- A local Wi-Fi network or mobile hotspot.
- ESP32 USB serial driver, if required by the operating system.

The dashboard uses Python standard-library modules only, so no extra Python package
installation is required.

<!--=========================================================================-->

## Project Layout

```text
.
|-- CMakeLists.txt
|-- README.md
|-- sdkconfig
|-- main
|   |-- CMakeLists.txt
|   |-- Kconfig.projbuild
|   |-- app_config.h
|   |-- main.c
|   |-- alarm_manager.c
|   |-- alarm_manager.h
|   |-- audio_player.c
|   |-- audio_player.h
|   |-- i2c_sensors.c
|   |-- i2c_sensors.h
|   |-- network_client.c
|   |-- network_client.h
|   |-- user_button.c
|   |-- user_button.h
|   |-- wifi_manager.c
|   `-- wifi_manager.h
`-- server
    |-- local_dashboard.py
    |-- dashboard.html
    |-- script.js
    |-- style.css
    `-- tracks
        |-- 01-Manu Chao  Tu Te Vas Feat. Laeti.mp3.mpeg
        |-- 02-Fabrizio De Andre - Via del campo.mp3.mpeg
        |-- 03-Lucio Dalla - Il parco della luna.mp3.mpeg
        `-- 04-Olivia Dean - So Easy (To Fall In Love).mp3.mpeg
```

### Source Code Organization

| Path | Role |
|---|---|
| [`main/main.c`](./main/main.c) | Firmware entry point, initialization, FreeRTOS task startup |
| [`main/alarm_manager.c`](./main/alarm_manager.c) | Alarm scheduling, shared state, snooze/stop handling, comfort alerts |
| [`main/audio_player.c`](./main/audio_player.c) | DFPlayer Mini UART setup, volume, play, and stop commands |
| [`main/i2c_sensors.c`](./main/i2c_sensors.c) | I2C initialization, AHT20 readings, ENS160 readings |
| [`main/user_button.c`](./main/user_button.c) | Button polling, debounce, single/double press actions |
| [`main/wifi_manager.c`](./main/wifi_manager.c) | Wi-Fi station mode, event callbacks, reconnect behavior |
| [`main/network_client.c`](./main/network_client.c) | Dashboard API polling and status upload |
| [`main/Kconfig.projbuild`](./main/Kconfig.projbuild) | ESP-IDF menuconfig entries |
| [`server/local_dashboard.py`](./server/local_dashboard.py) | Local web dashboard and API server |
| [`server/dashboard.html`](./server/dashboard.html) | Dashboard page |
| [`server/script.js`](./server/script.js) | Dashboard client logic |
| [`server/style.css`](./server/style.css) | Dashboard visual style |

<!--=========================================================================-->

## Getting Started

The following guide assumes that ESP-IDF is already installed and available in the
terminal. Commands are shown from the root of this repository.

### 1. Start the Local Dashboard

```sh
python3 server/local_dashboard.py --host 0.0.0.0 --port 8000
```

Open the dashboard on the same computer:

```text
http://localhost:8000
```

The page is used to configure alarms, preview tracks, plan sleep schedules, and
monitor live ESP32 status.

### 2. Find the Computer LAN IP Address

The ESP32 must connect to the computer using its LAN IP address. Do not configure
the ESP32 with `localhost`, because on the ESP32 that would refer to the ESP32
itself.

On Windows:

```sh
ipconfig
```

On macOS, usually:

```sh
ipconfig getifaddr en0
```

Example result:

```text
192.168.1.23
```

The firmware dashboard URL should then be:

```text
http://192.168.1.23:8000
```

### 3. Configure the ESP32 Firmware

Open the ESP-IDF configuration menu:

```sh
idf.py menuconfig
```

Go to `Smart Alarm Clock` and set:

- Wi-Fi SSID.
- Wi-Fi password.
- Local dashboard/API base URL.
- Device ID.
- Default alarm time.
- Default snooze duration.
- Timezone.
- Settings polling period.
- Status upload period.
- HTTP timeout.

Private Wi-Fi credentials should be configured through `idf.py menuconfig`, not
hard-coded into source files.

### 4. Build the Firmware

```sh
idf.py set-target esp32
idf.py build
```

### 5. Burn / Flash the Firmware

Connect the ESP32 board through USB and run:

```sh
idf.py flash
```

If the serial port is not detected automatically, specify it manually.

On Windows:

```sh
idf.py -p COM3 flash
```

On macOS/Linux:

```sh
idf.py -p /dev/cu.usbserial-210 flash
```

### 6. Run the Firmware Monitor

```sh
idf.py monitor
```

Or flash and monitor in one command:

```sh
idf.py flash monitor
```

### What Will You See?

When the system is working correctly:

1. The ESP32 initializes NVS, sensors, audio, button input, Wi-Fi, and tasks.
2. The serial monitor reports Wi-Fi connection progress.
3. The ESP32 polls `GET /api/alarm-settings`.
4. The ESP32 synchronizes its clock using `server_epoch` from the dashboard.
5. The ESP32 uploads live status using `POST /api/status`.
6. The dashboard displays temperature, humidity, air-quality data, alarm status,
   Wi-Fi status, and last error information.
7. When a configured alarm time is reached, the DFPlayer Mini plays the selected
   track.

<!--=========================================================================-->

## User Guide

### Creating an Alarm

1. Start the local dashboard.
2. Open `http://localhost:8000`.
3. In the `Alarms` section, create an alarm.
4. Configure the alarm time, label, snooze duration, track number, enabled state,
   and active days.
5. Press `Save`.
6. Keep the dashboard server running while the ESP32 is active.

The ESP32 periodically downloads alarm settings from:

```text
GET /api/alarm-settings
```

### Using the Sleep Schedule Planner

The dashboard includes a sleep schedule planner. It can calculate suggested sleep
or wake times using:

- Age group.
- Time to fall asleep.
- Sleep cycle duration.
- Preferred number of cycles.
- Wake-up or bedtime target.

The suggested times can be used to create alarms.

### Button Behavior

The physical button is connected to GPIO 4 and uses active-low logic.

| User Action | Firmware Behavior |
|---|---|
| First press while an alarm is ringing | Snoozes the current alarm |
| Second press within 2 seconds | Stops the alarm |

The button does not use a GPIO interrupt. The firmware polls the pin every 20 ms
and accepts a new state only after it remains stable for 80 ms.

```c
#define DOUBLE_PRESS_MS 2000
#define DEBOUNCE_MS 80
#define POLL_MS 20
```

The two-press stop interaction replaced an earlier long-press behavior because it
was more reliable and easier for the user to perform while the alarm was ringing.

### Audio Track Selection

The dashboard previews files from:

```text
server/tracks/
```

The physical DFPlayer Mini uses the numbered files on its SD card:

```text
MP3/0001.mp3
MP3/0002.mp3
MP3/0003.mp3
MP3/0004.mp3
```

If the dashboard alarm track is set to `3`, the DFPlayer should play:

```text
MP3/0003.mp3
```

### Sensor Readings

The firmware reads the AHT20 every 2 seconds. The measurement is triggered, the
firmware waits for conversion, then it reads 6 bytes and converts them to humidity
and temperature.

```c
uint8_t trigger_cmd[] = {0xAC, 0x33, 0x00};
i2c_master_write_to_device(I2C_MASTER_NUM, AHT20_ADDR,
                           trigger_cmd, 3, pdMS_TO_TICKS(100));

vTaskDelay(pdMS_TO_TICKS(100));

i2c_master_read_from_device(I2C_MASTER_NUM, AHT20_ADDR,
                            data, 6, pdMS_TO_TICKS(100));
```

The optional ENS160 sensor provides air-quality index, eCO2, and TVOC readings.
If the ENS160 is missing or not ready, the firmware continues running and tries to
detect it again periodically.

<!--=========================================================================-->

## Firmware Design Notes

### FreeRTOS Tasks

The firmware starts four main tasks from `app_main()`:

```c
xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
xTaskCreate(alarm_manager_task, "alarm_task", 4096, NULL, 5, NULL);
xTaskCreate(network_client_task, "network_client", 8192, NULL, 5, NULL);
```

| Task | Main Responsibility |
|---|---|
| `sensor_task` | Reads AHT20 and ENS160 sensor data |
| `button_task` | Polls and debounces the physical button |
| `alarm_manager_task` | Evaluates scheduled alarms, snooze timeouts, and comfort alerts |
| `network_client_task` | Polls settings and uploads status through HTTP |

### Shared Alarm State

The alarm manager stores global device state in a shared `alarm_status_t`
structure. Access is protected by a mutex because several tasks read and update
the same state.

```c
static SemaphoreHandle_t s_lock;
static alarm_status_t s_state;
```

Each configured alarm is represented by an `alarm_entry_t`, which contains both
configuration fields and runtime fields:

```c
typedef struct {
    bool enabled;
    int hour;
    int minute;
    int snooze_minutes;
    int volume;
    int track;
    uint8_t days_mask;
    char label[ALARM_LABEL_SIZE];
    bool active;
    int64_t active_since_uptime_ms;
    int64_t snoozed_until_epoch;
    int64_t snoozed_until_uptime_ms;
    int64_t last_alarm_epoch;
    int last_fired_day_key;
} alarm_entry_t;
```

### Alarm Evaluation

The alarm task checks the current local time and activates an alarm when:

- The alarm is enabled.
- The alarm is configured to run on the current day.
- The current hour and minute match the alarm time.
- The alarm has not already fired on the same day.

```c
if (alarm_runs_today(alarm, &local_now) &&
    local_now.tm_hour == alarm->hour &&
    local_now.tm_min == alarm->minute &&
    alarm->last_fired_day_key != today_key) {
    alarm->active = true;
    alarm->active_since_uptime_ms = now_ms;
    alarm->last_alarm_epoch = now;
    alarm->last_fired_day_key = today_key;
}
```

Audio commands are selected while the alarm state is locked, but the actual UART
commands are sent after unlocking so hardware communication does not block shared
state access.

### Wi-Fi Events

Wi-Fi state is updated through ESP-IDF event callbacks. On disconnect, the firmware
clears the connected bit and starts a reconnect attempt. On IP acquisition, it sets
the connected bit so the network client can proceed.

```c
if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_wifi_connect();
}

if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}
```

<!--=========================================================================-->

## Local Dashboard API

The ESP32 and dashboard communicate through local HTTP endpoints.

| Endpoint | Method | Used By | Purpose |
|---|---:|---|---|
| `/api/alarm-settings` | `GET` | ESP32 | Download alarm settings and server time |
| `/api/alarm-settings` | `POST` | Dashboard | Save alarm settings |
| `/api/status` | `POST` | ESP32 | Upload live device status |
| `/api/status` | `GET` | Dashboard | Read latest uploaded status |
| `/api/tracks` | `GET` | Dashboard | List previewable track files |

Example `GET /api/alarm-settings` response:

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
  "server_epoch": 1714470000,
  "server_iso": "2026-04-30 09:00:00 CEST"
}
```

The ESP32 uses `server_epoch` to synchronize its local clock.

<!--=========================================================================-->

## Testing and Expected Results

The project was tested through module-level checks and full integration tests with
the dashboard, sensors, button, Wi-Fi, and DFPlayer Mini connected.

### Tested Features

- Wi-Fi connection and reconnection.
- Dashboard alarm synchronization.
- Status upload to the local server.
- AHT20 temperature/humidity readings.
- ENS160 air-quality readings and reprobe behavior.
- Alarm scheduling.
- Multiple alarm handling.
- Snooze and stop behavior.
- Button responsiveness and debounce.
- DFPlayer Mini track playback and stop commands.

### Problems Encountered

| Problem | Cause | Solution |
|---|---|---|
| Multiple alarms occasionally failed to sound | Overlapping alarm states could create conflicting audio decisions | Cached current audio state and selected the oldest active alarm as priority |
| Alarm stop originally required a 2-second press | Long press timing was unreliable with debounce and real user behavior | Changed stop behavior to two quick presses within 2 seconds |
| Button bouncing | Mechanical button transitions were noisy | Added 80 ms stable-state debounce with 20 ms polling |
| Sensor timing errors | AHT20 needs conversion time before data is read | Added a measurement delay before reading sensor bytes |
| ENS160 not always detected at boot | Optional sensor may be missing or not ready | Added periodic reprobe behavior |
| Wi-Fi loss during runtime | Network may disconnect or hotspot may disappear | Used ESP-IDF event callbacks and reconnect attempts |
| Shared-state race conditions | Multiple FreeRTOS tasks access alarm state | Protected shared state with a FreeRTOS mutex |

### What Will You See During a Demo?

- The dashboard shows the ESP32 device ID and connectivity state.
- Temperature and humidity update periodically.
- Air-quality values appear if the ENS160 is connected and ready.
- Alarm configuration saved in the dashboard is reflected on the device.
- At the configured alarm time, the selected DFPlayer track plays.
- One button press snoozes the alarm.
- A second press within 2 seconds stops the alarm.

<!--=========================================================================-->

## Troubleshooting

### Wi-Fi Does Not Connect

Check:

- SSID and password are correct.
- Network is 2.4 GHz compatible.
- Mobile hotspot is visible and active.
- The ESP32 and dashboard computer are on the same network.
- The dashboard URL uses the computer LAN IP, not `localhost`.

If logs show `NO_AP_FOUND`, the ESP32 cannot see the configured access point.

### Dashboard Does Not Update

Check:

- `server/local_dashboard.py` is running.
- The computer firewall allows port `8000`.
- The ESP32 firmware was configured with the correct dashboard URL.
- Serial monitor logs show successful status uploads.

### DFPlayer Does Not Play

Check:

- SD card is inserted.
- Track files are named as `MP3/0001.mp3`, `MP3/0002.mp3`, etc.
- Speaker wiring is correct.
- UART TX/RX wiring is correct.
- The selected track number exists on the SD card.

### Sensor Values Are Missing

Check:

- SDA is connected to GPIO 21.
- SCL is connected to GPIO 22.
- Sensor power and ground are correct.
- The ENS160 is optional and may take time before reporting valid values.

<!--=========================================================================-->

## Links

- PowerPoint presentation: `[add presentation link here]`
- YouTube demo video: `[add YouTube video link here]`

Replace these placeholders with the final submission links.

<!--=========================================================================-->

## Team Members and Contributions

| Team Member | Main Contribution | Details |
|---|---|---|
| Daniel | Audio Player implementation | DFPlayer Mini UART setup, command packets, volume, track playback, and stop command handling |
| Filippo | WiFi Manager implementation | Wi-Fi station setup, ESP-IDF event callbacks, reconnect logic, and connection-state tracking |
| Gaetano | User Button implementation | GPIO button setup, active-low polling, debounce, snooze action, and double-press stop behavior |
| Chiara | I2C Sensor implementation | I2C bus setup, AHT20 temperature/humidity reading, ENS160 air-quality reading, and sensor error handling |
| Whole team | Remaining modules | Alarm manager, network integration, dashboard behavior, testing, debugging, and final integration |

<!--=========================================================================-->

## Future Work

### Hardware

- Custom protective enclosure.
- Better speaker system.
- Improved power management.
- More robust wiring or custom PCB.

### Software

- More distinctive alarm sounds.
- Improved dashboard user interface.
- More robust network recovery.
- Event logging for alarms, sensor errors, and connectivity.
- Historical visualization of sensor data.

### Additional Features

- Upload custom alarm tracks from the website.
- Remote notifications.
- Cloud synchronization.
- OTA firmware updates.
- Multiple user profiles.
- Alarm history.

### Additional Sensors

- Temperature sensor.
- Humidity sensor.
- Ambient light sensor.
- Motion detector.
- Air quality sensor.

## Acknowledgments

This project was developed as part of the Embedded Software course project.

## Final Statement

The project successfully demonstrates an integrated embedded system combining
sensing, networking, user interaction, and audio feedback while providing a solid
foundation for future extensions.
