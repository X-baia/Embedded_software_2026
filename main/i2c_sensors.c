#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "i2c_sensors.h"
#include "alarm_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

//I2C -> communication protocol used by sensors to send datato the ESP32. allows multiple devices to share the same bus with unique addresses.

static const char *TAG = "SENSORS";

#define I2C_MASTER_NUM I2C_NUM_0 //which internal engine on the ESP32 handles the connection
#define AHT20_ADDR 0x38 //address assigned to the AHT20 sensor (temperature and reliability)

//ENS160 measures air quality
#define ENS160_ADDR_LOW 0x52 //can use either 0x52 or 0x53
#define ENS160_ADDR_HIGH 0x53
#define ENS160_PART_ID 0x0160 //unique ID burned into the sensor to verify it's the correct device
#define ENS160_REG_PART_ID 0x00 //where the custom ID in the sensor is stored
#define ENS160_REG_OPMODE 0x10 //register to change the sensor's status
#define ENS160_REG_DATA_AQI 0x21 
#define ENS160_REG_DATA_TVOC 0x22
#define ENS160_REG_DATA_ECO2 0x24 //finalized read data is stored
#define ENS160_OPMODE_STANDARD 0x02 //command code sent to wake up the sensor
#define ENS160_REPROBE_PERIOD_MS 10000 //time wait before trying to reconnect to the sensor if it fails

static uint8_t s_ens160_addr; //saves which of the 2 registers (0x52 or 0x53) the ENS160 is using

//sends a zero-length write to the given address to see if a device responds with an ACK
static bool i2c_probe_address(uint8_t device_addr){
    uint8_t unused = 0;
    return i2c_master_write_to_device(I2C_MASTER_NUM, device_addr, &unused, 0, pdMS_TO_TICKS(50)) == ESP_OK;
}

//loops through all possible I2C addresses, prints all the devices that respond
static void log_i2c_devices(void){
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_probe_address(addr)) {
            ESP_LOGI(TAG, "I2C device detected at 0x%02X", addr);
        }
    }
}

//reads data from a specific register on an I2C device. first writes the register address, then reads the response
static esp_err_t i2c_read_register(uint8_t device_addr, uint8_t reg_addr, uint8_t *data, size_t data_len){
    return i2c_master_write_read_device(
        I2C_MASTER_NUM,
        device_addr,
        &reg_addr,
        1,
        data,
        data_len,
        pdMS_TO_TICKS(100));
}

//overwrites a specific register inside a sensor. it sends both the register address and the new value in a single write operation
static esp_err_t i2c_write_register(uint8_t device_addr, uint8_t reg_addr, uint8_t value){
    uint8_t data[] = {reg_addr, value};
    return i2c_master_write_to_device(I2C_MASTER_NUM, device_addr, data, sizeof(data), pdMS_TO_TICKS(100));
}


//checks and set up the ENS160 sensor. it tries both possible addresses (0x52 and 0x53) to find the sensor, then verifies the part ID and wakes it up from sleep mode. if any step fails, it returns false and will try again later.
static bool ens160_init(void){
    const uint8_t addresses[] = {ENS160_ADDR_LOW, ENS160_ADDR_HIGH};

    for (size_t index = 0; index < sizeof(addresses) / sizeof(addresses[0]); index++) {
        uint8_t addr = addresses[index];
        uint8_t part_id_data[2] = {0};
        esp_err_t err = i2c_read_register(addr, ENS160_REG_PART_ID, part_id_data, sizeof(part_id_data));
        if (err != ESP_OK) {
            continue;
        }

        uint16_t part_id = (uint16_t)part_id_data[0] | ((uint16_t)part_id_data[1] << 8);
        if (part_id != ENS160_PART_ID) {
            ESP_LOGW(TAG, "Unexpected ENS160 part id at 0x%02X: 0x%04X", addr, part_id);
            continue;
        }

        err = i2c_write_register(addr, ENS160_REG_OPMODE, ENS160_OPMODE_STANDARD); //wake up the sensor from sleep mode and set it to standard mode
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ENS160 standard mode failed at 0x%02X: %s", addr, esp_err_to_name(err));
            alarm_manager_set_error("ENS160 init failed");
            return false;
        }

        s_ens160_addr = addr;
        vTaskDelay(pdMS_TO_TICKS(20)); //waits for the sensor to wake up
        ESP_LOGI(TAG, "ENS160 air quality sensor initialized at 0x%02X", s_ens160_addr);
        return true;
    }

    ESP_LOGW(TAG, "ENS160 air quality sensor not detected at 0x%02X or 0x%02X", ENS160_ADDR_LOW, ENS160_ADDR_HIGH);
    return false;
}

//reads the data from the sensor, performs bit shifting to convert the raw bytes into usable values
static bool ens160_read_air_quality(int *air_quality_index, int *eco2_ppm, int *tvoc_ppb){
    if (s_ens160_addr == 0) {
        return false;
    }

    uint8_t aqi_data = 0;
    uint8_t eco2_data[2] = {0};
    uint8_t tvoc_data[2] = {0};

    //read all the data from the sensors, if something fails logs the error and resets the sensor address to 0
    esp_err_t err = i2c_read_register(s_ens160_addr, ENS160_REG_DATA_AQI, &aqi_data, sizeof(aqi_data));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ENS160 AQI read failed at 0x%02X: %s", s_ens160_addr, esp_err_to_name(err));
        alarm_manager_set_error("ENS160 read failed");
        s_ens160_addr = 0;
        return false;
    }

    err = i2c_read_register(s_ens160_addr, ENS160_REG_DATA_ECO2, eco2_data, sizeof(eco2_data));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ENS160 eCO2 read failed at 0x%02X: %s", s_ens160_addr, esp_err_to_name(err));
        alarm_manager_set_error("ENS160 read failed");
        s_ens160_addr = 0;
        return false;
    }

    err = i2c_read_register(s_ens160_addr, ENS160_REG_DATA_TVOC, tvoc_data, sizeof(tvoc_data));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ENS160 TVOC read failed at 0x%02X: %s", s_ens160_addr, esp_err_to_name(err));
        alarm_manager_set_error("ENS160 read failed");
        s_ens160_addr = 0;
        return false;
    }

    //performs bit shifting
    int parsed_aqi = (int)aqi_data;
    int parsed_eco2 = (int)((uint16_t)eco2_data[0] | ((uint16_t)eco2_data[1] << 8));
    int parsed_tvoc = (int)((uint16_t)tvoc_data[0] | ((uint16_t)tvoc_data[1] << 8));

    if (parsed_aqi < 1 || parsed_aqi > 5) {
        ESP_LOGI(TAG, "ENS160 air quality data not ready yet");
        return false;
    }

    *air_quality_index = parsed_aqi;
    *eco2_ppm = parsed_eco2;
    *tvoc_ppb = parsed_tvoc;
    return true;
}

//configures physical pins for I2C communications, sets clock speed.
void init_i2c_master(void){
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
    log_i2c_devices();
}

//continuously reads data from the sensors, updates the alarm manager with the latest values, and handles any errors that may occur during communication
void sensor_task(void *pvParameters){
    (void)pvParameters;

    uint8_t data[6];
    bool ens160_available = ens160_init();
    TickType_t last_ens160_probe_tick = xTaskGetTickCount();
    
    while(1) {
        //trigger Measurement
        uint8_t trigger_cmd[] = {0xAC, 0x33, 0x00};
        esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, AHT20_ADDR, trigger_cmd, 3, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 trigger failed: %s", esp_err_to_name(err));
            alarm_manager_set_error("AHT20 trigger failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        
        //wait for measurement to complete
        vTaskDelay(pdMS_TO_TICKS(100));

        //read 6 bytes of data
        err = i2c_master_read_from_device(I2C_MASTER_NUM, AHT20_ADDR, data, 6, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 read failed: %s", esp_err_to_name(err));
            alarm_manager_set_error("AHT20 read failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        //convert raw data to temperature and humidity
        uint32_t humidity_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
        float humidity = (float)humidity_raw * 100 / 1048576;

        uint32_t temp_raw = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
        float temperature = (float)temp_raw * 200 / 1048576 - 50;

        ESP_LOGI(TAG, "Temp: %.2f°C | Humidity: %.2f%%", temperature, humidity);
        alarm_manager_update_environment(temperature, humidity);

        if (!ens160_available &&
            xTaskGetTickCount() - last_ens160_probe_tick >= pdMS_TO_TICKS(ENS160_REPROBE_PERIOD_MS)) {
            ens160_available = ens160_init();
            last_ens160_probe_tick = xTaskGetTickCount();
        }

        if (ens160_available) {
            int air_quality_index = 0;
            int eco2_ppm = 0;
            int tvoc_ppb = 0;
            if (ens160_read_air_quality(&air_quality_index, &eco2_ppm, &tvoc_ppb)) {
                ESP_LOGI(TAG,
                         "Air quality index: %d | eCO2: %d ppm | TVOC: %d ppb",
                         air_quality_index,
                         eco2_ppm,
                         tvoc_ppb);
                alarm_manager_update_air_quality(true, air_quality_index, eco2_ppm, tvoc_ppb);
            } else {
                alarm_manager_update_air_quality(false, 0, 0, 0);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); //read every 2 seconds
    }
}
