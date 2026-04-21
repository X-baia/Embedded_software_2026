#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// This tag will help us identify our messages in the serial monitor
static const char *TAG = "SMART_ALARM";

// Placeholder Task for your AHT20, BMP280, and ENS160
void sensor_task(void *pvParameters) {
    while (1) {
        ESP_LOGI(TAG, "Sensor task is alive... waiting to read I2C.");
        vTaskDelay(pdMS_TO_TICKS(2000)); // Delay for 2 seconds
    }
}

// Placeholder Task for your Wi-Fi and Web Server
void web_server_task(void *pvParameters) {
    while (1) {
        ESP_LOGI(TAG, "Web server task is alive... waiting for Wi-Fi.");
        vTaskDelay(pdMS_TO_TICKS(3000)); // Delay for 3 seconds
    }
}

// The main entry point of the ESP32
void app_main(void) {
    ESP_LOGI(TAG, "=== Smart Alarm Booting Up ===");

    // Spin up the independent tasks
    xTaskCreate(sensor_task, "sensor_task", 2048, NULL, 5, NULL);
    xTaskCreate(web_server_task, "web_server_task", 4096, NULL, 5, NULL);
}