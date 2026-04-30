#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Include our custom modules
#include "i2c_sensors.h"
#include "audio_player.h"
#include "user_button.h"

static const char *TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "=== Smart Alarm Booting Up ===");

    // 1. Initialize Hardware
    init_i2c_master();
    init_audio_player();
    init_user_button();

    // 2. Start Independent Tasks
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
}