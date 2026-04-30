#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Include our custom modules
#include "i2c_sensors.h"
#include "audio_player.h"
#include "user_button.h"
#include "alarm_manager.h"
#include "wifi_manager.h"
#include "network_client.h"

static const char *TAG = "MAIN";

static void init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Smart Alarm Booting Up ===");

    init_nvs();
    alarm_manager_init();

    // 1. Initialize Hardware
    init_i2c_master();
    init_audio_player();
    init_user_button();
    ESP_ERROR_CHECK(wifi_manager_start());

    // 2. Start Independent Tasks
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
    xTaskCreate(alarm_manager_task, "alarm_task", 4096, NULL, 5, NULL);
    xTaskCreate(network_client_task, "network_client", 8192, NULL, 5, NULL);
}
