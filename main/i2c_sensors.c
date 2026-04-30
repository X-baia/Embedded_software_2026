#include <stdio.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "i2c_sensors.h"
#include "alarm_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SENSORS";

#define I2C_MASTER_NUM I2C_NUM_0
#define AHT20_ADDR 0x38 

void init_i2c_master(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = 21,
        .scl_io_num = 22,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    ESP_LOGI(TAG, "I2C Initialized");
}

void sensor_task(void *pvParameters) {
    uint8_t data[6];
    
    while(1) {
        // 1. Trigger Measurement
        uint8_t trigger_cmd[] = {0xAC, 0x33, 0x00};
        esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, AHT20_ADDR, trigger_cmd, 3, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 trigger failed: %s", esp_err_to_name(err));
            alarm_manager_set_error("AHT20 trigger failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        
        // 2. Wait for measurement to complete (AHT20 needs ~80ms)
        vTaskDelay(pdMS_TO_TICKS(100));

        // 3. Read 6 bytes of data
        err = i2c_master_read_from_device(I2C_MASTER_NUM, AHT20_ADDR, data, 6, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 read failed: %s", esp_err_to_name(err));
            alarm_manager_set_error("AHT20 read failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // 4. Convert raw data to Temperature and Humidity
        // These formulas come from the AHT20 Datasheet
        uint32_t humidity_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
        float humidity = (float)humidity_raw * 100 / 1048576;

        uint32_t temp_raw = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
        float temperature = (float)temp_raw * 200 / 1048576 - 50;

        ESP_LOGI(TAG, "Temp: %.2f°C | Humidity: %.2f%%", temperature, humidity);
        alarm_manager_update_environment(temperature, humidity);

        vTaskDelay(pdMS_TO_TICKS(2000)); // Read every 2 seconds
    }
}
