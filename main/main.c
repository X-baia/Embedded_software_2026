#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// we include the headers for our modules to initialize them and use their functions
#include "i2c_sensors.h"
#include "audio_player.h"
#include "user_button.h"
#include "alarm_manager.h"
#include "wifi_manager.h"
#include "network_client.h"

// tag created for logging purposes, helps identify which module the log messages belong to
static const char *TAG = "MAIN";

// we initialize the NVS (Non-Volatile Storage) flash, which is used to store data that persists across reboots, such as Wi-Fi credentials or user settings
static void init_nvs(void) {
    esp_err_t err = nvs_flash_init();

    // when we first boot or update the partition we check for specific errors, and in case the partition is full or corrupted or the NVS version changes: we ersae the partition and initialize it again
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
}

// this is the entry point for the main application, which runs after the system boots 
// and starts the FreeRTOS scheduler implicitly once app_main returns
void app_main(void) {
    ESP_LOGI(TAG, "=== Smart Alarm Booting Up ===");

    // we set up persistent storage and alarm state before the hardware is initialized
    init_nvs();
    alarm_manager_init();

    // the hardware and peripheral subsystems get initialized
    init_i2c_master();   // sets up the I2C bus so sensors can communicate
    init_audio_player(); // sets up the speaker/audio system
    init_user_button();  // sets up the physical button as an input

    // we use ESP_ERROR_CHECK to verify the the wifi manager starts properly
    ESP_ERROR_CHECK(wifi_manager_start());

    // we launch independent FreeRTOS tasks for each subsystem
    // the tasks will run concurrently after app_main returns
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
    xTaskCreate(alarm_manager_task, "alarm_task", 4096, NULL, 5, NULL);
    xTaskCreate(network_client_task, "network_client", 8192, NULL, 5, NULL);
}
