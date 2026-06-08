# Smart Alarm Clock

ESP32-based smart alarm clock with local Wi-Fi configuration, environmental sensing,
physical button control, and DFPlayer Mini audio playback.

The project runs entirely on a local network. No cloud service is required: the ESP32
communicates with a local web dashboard to download alarm settings and upload live
device status.

## Project Overview

The Smart Alarm Clock combines multiple embedded-system features in one integrated
application:

- Multi-alarm scheduling with configurable time, days, snooze, volume, and track.
- Local web dashboard for alarm setup, sleep schedule planning, and live monitoring.
- AHT20 temperature/humidity sensing over I2C.
- Optional ENS160 air-quality sensing over I2C.
- Physical button interaction for snooze and stop.
- DFPlayer Mini audio output over UART.
- Wi-Fi reconnect handling and local REST API synchronization.
- Shared alarm state protected across FreeRTOS tasks.

## Requirements

### Hardware

- ESP32 development board.
- AHT20 temperature/humidity sensor.
- Optional ENS160 air-quality sensor.
- DFPlayer Mini MP3 module.
- MicroSD card for the DFPlayer Mini.
- Speaker compatible with the DFPlayer Mini.
- Push button.
- Breadboard and jumper wires.
- Stable 5 V / USB power supply for the ESP32 and peripherals.

### Wiring Summary

| Component | ESP32 Connection | Notes |
|---|---:|---|
| AHT20 SDA | GPIO 21 | I2C data |
| AHT20 SCL | GPIO 22 | I2C clock |
| ENS160 SDA | GPIO 21 | Same I2C bus, optional |
| ENS160 SCL | GPIO 22 | Same I2C bus, optional |
| Button | GPIO 4 | Active-low, internal pull-up enabled |
| DFPlayer RX | GPIO 17 | ESP32 UART1 TX |
| DFPlayer TX | GPIO 16 | ESP32 UART1 RX |
| DFPlayer speaker | Speaker output | Use suitable speaker/load |

The ENS160 can be detected at address `0x52` or `0x53`. The AHT20 uses address
`0x38`.

### Software

- ESP-IDF installed and configured.
- Python 3.
- A modern web browser.
- Local Wi-Fi network or mobile hotspot.
- Serial USB driver for the ESP32 board, if required by your operating system.

The local dashboard uses only Python standard-library modules.

## Project Layout

```text
.
|-- CMakeLists.txt
|-- README.md
|-- sdkconfig
|-- main/
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
`-- server/
    |-- local_dashboard.py
    |-- dashboard.html
    |-- script.js
    |-- style.css
    `-- tracks/
```

### Source Code Organization

| File / Module | Responsibility |
|---|---|
| `main/main.c` | Firmware entry point, hardware initialization, FreeRTOS task creation |
| `main/alarm_manager.*` | Alarm configuration, runtime state, snooze/stop logic, comfort alerts, shared status |
| `main/audio_player.*` | DFPlayer Mini UART setup, volume control, play/stop commands |
| `main/i2c_sensors.*` | I2C setup, AHT20 readings, ENS160 readings, sensor error handling |
| `main/user_button.*` | GPIO button setup, polling, debounce, single/double press behavior |
| `main/wifi_manager.*` | Wi-Fi station setup, reconnect handling, ESP-IDF event callbacks |
| `main/network_client.*` | HTTP polling of alarm settings and status upload to the dashboard |
| `main/app_config.h` | Firmware configuration aliases from `sdkconfig` |
| `main/Kconfig.projbuild` | ESP-IDF menuconfig options for Wi-Fi, dashboard URL, alarm defaults, and timing |
| `server/local_dashboard.py` | Local web server and REST API |
| `server/dashboard.html` | Dashboard page structure |
| `server/script.js` | Dashboard client-side behavior |
| `server/style.css` | Dashboard styling |
| `server/tracks/` | Browser-previewable alarm track files |

## Software Architecture

The firmware is organized as several cooperating FreeRTOS tasks:

```text
                       +----------------------+
                       |       main.c         |
                       | Init + task startup  |
                       +----------+-----------+
                                  |
          +-----------------------+-----------------------+
          |                       |                       |
+------------------+    +------------------+    +------------------+
| Sensor Task      |    | Button Task      |    | Alarm Task       |
| i2c_sensors.c    |    | user_button.c    |    | alarm_manager.c  |
+--------+---------+    +--------+---------+    +--------+---------+
         |                       |                       |
         +-----------------------+-----------------------+
                                  |
                       +----------------------+
                       | Shared Alarm State   |
                       | Mutex protected      |
                       +----------+-----------+
                                  |
          +-----------------------+-----------------------+
          |                                               |
+------------------+                            +------------------+
| Audio Player     |                            | Network Client   |
| DFPlayer / UART  |                            | HTTP API         |
+------------------+                            +--------+---------+
                                                        |
                                               +------------------+
                                               | WiFi Manager     |
                                               | ESP-IDF events   |
                                               +------------------+
```

The `alarm_manager` module is the central coordination point. It stores alarm
settings, runtime alarm state, sensor values, timestamps, and error information.
This shared data is protected by a FreeRTOS mutex because it is accessed by sensor,
button, alarm, and network tasks.

## How the Firmware Works

### Startup Sequence

At boot, `app_main()` initializes persistent storage, alarm defaults, hardware
drivers, Wi-Fi, and then starts the main FreeRTOS tasks:

```c
void app_main(void) {
    ESP_LOGI(TAG, "=== Smart Alarm Booting Up ===");

    init_nvs();
    alarm_manager_init();

    init_i2c_master();
    init_audio_player();
    init_user_button();
    ESP_ERROR_CHECK(wifi_manager_start());

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
    xTaskCreate(alarm_manager_task, "alarm_task", 4096, NULL, 5, NULL);
    xTaskCreate(network_client_task, "network_client", 8192, NULL, 5, NULL);
}
```

### Runtime Behavior

- `sensor_task` reads environmental data approximately every 2 seconds.
- `button_task` polls the button every 20 ms and applies software debouncing.
- `alarm_manager_task` checks alarm conditions every second.
- `network_client_task` waits for Wi-Fi, downloads alarm settings, and uploads status.
- `wifi_manager` uses ESP-IDF Wi-Fi/IP event callbacks to track connectivity.

### Interrupts and Events

The button does not use a GPIO interrupt. It is configured with interrupts disabled:

```c
.intr_type = GPIO_INTR_DISABLE
```

Button input is handled by polling because this makes debounce and double-press
detection simple and predictable.

Wi-Fi/IP events are handled through ESP-IDF callbacks:

```c
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_retry_count++;
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
```

## Build, Burn, and Run

### 1. Start the Local Dashboard

From the project root:

```sh
python3 server/local_dashboard.py --host 0.0.0.0 --port 8000
```

Open the dashboard on the computer running the server:

```text
http://localhost:8000
```

The dashboard is used to create alarms, preview tracks, configure sleep schedule
preferences, and monitor live device status.

### 2. Find the Computer LAN IP Address

The ESP32 cannot use `localhost`, because `localhost` would refer to the ESP32
itself. Use the IP address of the computer running the dashboard.

Examples:

```sh
ipconfig
```

or, on macOS:

```sh
ipconfig getifaddr en0
```

Example LAN address:

```text
192.168.1.23
```

The ESP32 dashboard URL should then be:

```text
http://192.168.1.23:8000
```

### 3. Configure the Firmware

Open ESP-IDF configuration:

```sh
idf.py menuconfig
```

Go to `Smart Alarm Clock` and configure:

- Wi-Fi SSID.
- Wi-Fi password.
- Local dashboard/API base URL, for example `http://192.168.1.23:8000`.
- Device ID.
- Default alarm time.
- Default snooze duration.
- Timezone.
- Settings polling period.
- Status upload period.
- HTTP timeout.

Do not hard-code private Wi-Fi credentials into source files. The firmware reads
these values from `sdkconfig`, generated by `idf.py menuconfig`.

### 4. Build the Project

```sh
idf.py set-target esp32
idf.py build
```

### 5. Burn / Flash the ESP32

Connect the ESP32 by USB, then run:

```sh
idf.py flash
```

If needed, specify the serial port:

```sh
idf.py -p COM3 flash
```

or on macOS/Linux:

```sh
idf.py -p /dev/cu.usbserial-210 flash
```

### 6. Run and Monitor

```sh
idf.py monitor
```

Or flash and monitor in one command:

```sh
idf.py flash monitor
```

Expected behavior:

1. ESP32 boots and initializes hardware modules.
2. ESP32 connects to Wi-Fi.
3. ESP32 polls `GET /api/alarm-settings`.
4. ESP32 synchronizes its local clock using `server_epoch`.
5. ESP32 uploads live status with `POST /api/status`.
6. The dashboard displays sensor data, alarm state, and connectivity information.

## User Guide

### Starting the System

1. Wire the ESP32, sensors, button, and DFPlayer Mini.
2. Put alarm tracks on the DFPlayer Mini SD card.
3. Start the local dashboard:

   ```sh
   python3 server/local_dashboard.py --host 0.0.0.0 --port 8000
   ```

4. Open `http://localhost:8000` in a browser.
5. Configure the ESP32 with the correct Wi-Fi credentials and dashboard URL.
6. Build, flash, and monitor the ESP32.

### Creating and Editing Alarms

In the dashboard:

1. Open the `Alarms` section.
2. Add one or more alarms.
3. Configure:
   - Alarm label.
   - Alarm time.
   - Enabled/disabled state.
   - Snooze duration.
   - Audio track number.
   - Active days.
4. Press `Save`.

The ESP32 periodically downloads these settings from the dashboard.

### Sleep Schedule Planner

The dashboard includes a sleep schedule planner. It can be used to estimate useful
bedtime or wake-up suggestions based on:

- Age group.
- Time needed to fall asleep.
- Sleep cycle duration.
- Preferred number of sleep cycles.
- Wake-up or bedtime target.

Suggested times can then be used when creating alarms.

### Button Behavior

The physical button is connected to GPIO 4 and uses active-low logic.

| Action | Result |
|---|---|
| First press while alarm is ringing | Snooze the current alarm |
| Second press within 2 seconds | Stop the alarm |

This double-press design replaced an earlier long-press stop behavior because two
quick presses were more reliable and easier to detect with software debouncing.

### Audio Tracks

The dashboard can preview files stored in:

```text
server/tracks/
```

For the physical DFPlayer Mini SD card, place files in the `MP3` folder using
four-digit names:

```text
MP3/0001.mp3
MP3/0002.mp3
MP3/0003.mp3
MP3/0004.mp3
```

The alarm track number selected in the dashboard corresponds to the DFPlayer track
number.

### Sensor Data

The dashboard can display:

- Temperature.
- Humidity.
- Air quality index.
- eCO2 concentration.
- TVOC concentration.
- Last error message, if any.

The AHT20 is required for temperature and humidity. The ENS160 air-quality sensor is
optional; if it is missing or not ready, the firmware continues running and retries
periodically.

## REST API Reference

The ESP32 communicates with the dashboard through local HTTP endpoints.

| Endpoint | Method | Purpose |
|---|---:|---|
| `/api/alarm-settings` | `GET` | ESP32 downloads alarm settings and server time |
| `/api/alarm-settings` | `POST` | Dashboard saves alarm settings |
| `/api/status` | `POST` | ESP32 uploads live status |
| `/api/status` | `GET` | Dashboard reads latest device status |
| `/api/tracks` | `GET` | Dashboard lists available preview tracks |

Example alarm settings response:

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

## Testing

The project was tested through a combination of module-level checks and integration
tests.

### Tested Areas

- Wi-Fi connection and reconnection.
- Dashboard alarm synchronization.
- Status upload to the local server.
- AHT20 temperature/humidity readings.
- ENS160 air-quality readings and reprobe behavior.
- Alarm scheduling and snooze behavior.
- Multiple alarm handling.
- Button responsiveness and debounce behavior.
- DFPlayer Mini playback and stop commands.

### Problems Encountered and Solutions

| Problem | Cause | Solution |
|---|---|---|
| Multiple alarms occasionally failed to sound | Overlapping alarm states could produce conflicting audio decisions | Cached audio state and selected the oldest active alarm as priority |
| Alarm stop originally required a 2-second press | Long press timing was unreliable with debounce and real user behavior | Changed interaction to two quick presses within 2 seconds |
| Button bouncing | Mechanical button transitions were noisy | Added 80 ms stable-state debounce and 20 ms polling |
| Sensor timing errors | AHT20 requires conversion time before reading | Added measurement delay before reading sensor bytes |
| ENS160 not always detected at boot | Optional sensor may be missing or not ready | Added periodic reprobe behavior |
| Wi-Fi loss during runtime | Network may disconnect or hotspot may disappear | ESP-IDF event callbacks clear/set connection bits and reconnect |
| Shared-state race conditions | Multiple tasks access alarm state | Protected shared alarm state with a FreeRTOS mutex |

## Troubleshooting

### ESP32 Cannot Connect to Wi-Fi

Check:

- SSID spelling and capitalization.
- Password correctness.
- Network is 2.4 GHz compatible.
- Mobile hotspot is visible and active.
- ESP32 dashboard URL uses the computer LAN IP, not `localhost`.

If logs show `NO_AP_FOUND`, the ESP32 cannot see the configured access point.

### Dashboard Does Not Update

Check:

- `server/local_dashboard.py` is still running.
- Computer and ESP32 are on the same network.
- Firewall allows connections to port `8000`.
- ESP32 `APP_SERVER_URL` points to the computer LAN IP.
- Serial monitor shows successful `POST /api/status`.

### DFPlayer Does Not Play Audio

Check:

- SD card is inserted and formatted correctly.
- Tracks are named as `MP3/0001.mp3`, `MP3/0002.mp3`, etc.
- Speaker is connected correctly.
- UART wiring is not swapped incorrectly.
- Track number selected in the dashboard exists on the SD card.

### Sensor Readings Are Missing

Check:

- SDA is connected to GPIO 21.
- SCL is connected to GPIO 22.
- Sensor power and ground are correct.
- I2C address matches the sensor.
- ENS160 is optional and may require time before valid readings are available.

## Links

- PowerPoint presentation: `[add presentation link here]`
- YouTube demo video: `[add YouTube video link here]`

Replace these placeholders with the final public or shared links before submission.

## Team Members and Contributions

| Team Member | Main Contribution | Details |
|---|---|---|
| Daniel | Audio Player implementation | Implemented DFPlayer Mini UART configuration, command packet creation, volume control, track playback, and stop command handling |
| Filippo | WiFi Manager implementation | Implemented Wi-Fi station setup, ESP-IDF event callback handling, reconnect behavior, and connection-state synchronization |
| Gaetano | User Button implementation | Implemented GPIO button setup, active-low polling, debounce logic, snooze action, and double-press stop behavior |
| Chiara | I2C Sensor implementation | Implemented I2C initialization, AHT20 temperature/humidity acquisition, ENS160 air-quality acquisition, and sensor error handling |
| Whole team | Remaining modules | Alarm manager, network integration, dashboard behavior, testing, debugging, and final integration were developed collaboratively |

## Future Work

### Hardware Improvements

- Custom protective enclosure.
- Better speaker system.
- Improved power management.
- More robust wiring or custom PCB.

### Software Improvements

- More distinctive alarm sounds.
- Improved dashboard user interface.
- More robust network recovery.
- Event logging for alarms, sensor errors, and connectivity.
- Better visualization of historical sensor data.

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

## Final Notes

The project successfully demonstrates an integrated embedded system combining
sensing, networking, user interaction, and audio feedback while providing a solid
foundation for future extensions.
