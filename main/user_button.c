#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "alarm_manager.h"
#include "user_button.h"

static const char *TAG = "BUTTON";
#define BUTTON_PIN GPIO_NUM_4
#define LONG_PRESS_MS 2000

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

    int last_level = 1;
    TickType_t press_start_tick = 0;
    bool long_press_handled = false;

    while(1) {
        int level = gpio_get_level(BUTTON_PIN);

        // Because of the pull-up, the pin reads 0 when pressed.
        if (last_level == 1 && level == 0) {
            press_start_tick = xTaskGetTickCount();
            long_press_handled = false;
            ESP_LOGI(TAG, "Button pressed");
        } else if (level == 0 && !long_press_handled &&
                   (xTaskGetTickCount() - press_start_tick) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
            ESP_LOGI(TAG, "Button held for %d ms, stopping alarm", LONG_PRESS_MS);
            alarm_manager_stop();
            long_press_handled = true;
        } else if (last_level == 0 && level == 1) {
            if (!long_press_handled) {
                ESP_LOGI(TAG, "Button clicked, snoozing alarm");
                alarm_manager_snooze();
            }
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(50)); // Check every 50ms
    }
}
