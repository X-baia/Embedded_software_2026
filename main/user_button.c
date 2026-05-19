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
#define LONG_PRESS_MS 4000
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
    bool press_snoozed = false;
    bool long_press_handled = false;

    while(1) {
        raw_level = gpio_get_level(BUTTON_PIN);
        TickType_t now_tick = xTaskGetTickCount();

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
                press_snoozed = true;
                long_press_handled = false;
                ESP_LOGI(TAG, "Button pressed");
                alarm_manager_snooze();
            } else if (press_start_tick != 0) {
                uint32_t press_ms = (uint32_t)((now_tick - press_start_tick) * portTICK_PERIOD_MS);

                ESP_LOGI(TAG, "Button released after %lu ms%s",
                         (unsigned long)press_ms,
                         long_press_handled ? ", alarm stopped" : (press_snoozed ? ", alarm snoozed" : ""));
                press_start_tick = 0;
                press_snoozed = false;
                long_press_handled = false;
            }
        }

        if (stable_level == 0 && press_start_tick != 0 && !long_press_handled &&
            (now_tick - press_start_tick) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
            ESP_LOGI(TAG, "Button held for %d ms, stopping alarm", LONG_PRESS_MS);
            alarm_manager_stop();
            long_press_handled = true;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
