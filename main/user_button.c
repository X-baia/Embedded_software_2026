#include "esp_log.h"
#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "alarm_manager.h"
#include "user_button.h"

static const char *TAG = "BUTTON";
#define BUTTON_PIN GPIO_NUM_4
#define DOUBLE_PRESS_MS 2000
#define DEBOUNCE_MS 80
#define POLL_MS 20

void init_user_button(void) {
    gpio_config_t btn_config = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE 
    };
    gpio_config(&btn_config);
    ESP_LOGI(TAG, "Button initialized on GPIO 4");
}

void button_task(void *pvParameters) {
    (void)pvParameters;

    int raw_level = gpio_get_level(BUTTON_PIN);
    int last_raw_level = raw_level;
    int stable_level = raw_level;
    TickType_t last_raw_change_tick = xTaskGetTickCount();
    TickType_t press_start_tick = 0;
    TickType_t last_press_tick = 0;
    bool alarm_stopped_on_press = false;

    while(1) {
        raw_level = gpio_get_level(BUTTON_PIN);
        TickType_t now_tick = xTaskGetTickCount();

        if (last_press_tick != 0 &&
            (now_tick - last_press_tick) > pdMS_TO_TICKS(DOUBLE_PRESS_MS)) {
            last_press_tick = 0;
        }

        if (raw_level != last_raw_level) {
            last_raw_level = raw_level;
            last_raw_change_tick = now_tick;
        }

        if (raw_level != stable_level &&
            (now_tick - last_raw_change_tick) >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
            stable_level = raw_level;

            // Because of the pull-up, the pin reads 0 when pressed.
            if (stable_level == 0) {
                press_start_tick = now_tick;
                alarm_stopped_on_press = false;

                if (last_press_tick != 0 &&
                    (now_tick - last_press_tick) <= pdMS_TO_TICKS(DOUBLE_PRESS_MS)) {
                    ESP_LOGI(TAG, "Button double press detected within %d ms, stopping alarm", DOUBLE_PRESS_MS);
                    alarm_manager_stop();
                    last_press_tick = 0;
                    alarm_stopped_on_press = true;
                } else {
                    ESP_LOGI(TAG, "Button pressed, snoozing alarm");
                    alarm_manager_snooze();
                    last_press_tick = now_tick;
                }
            } else if (press_start_tick != 0) {
                uint32_t press_ms = (uint32_t)((now_tick - press_start_tick) * portTICK_PERIOD_MS);

                ESP_LOGI(TAG, "Button released after %lu ms%s",
                         (unsigned long)press_ms,
                         alarm_stopped_on_press ? ", alarm stopped" : ", alarm snoozed");
                press_start_tick = 0;
                alarm_stopped_on_press = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
