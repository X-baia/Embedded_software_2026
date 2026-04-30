#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "alarm_manager.h"
#include "user_button.h"

static const char *TAG = "BUTTON";
#define BUTTON_PIN GPIO_NUM_4

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
    int last_level = 1;

    while(1) {
        int level = gpio_get_level(BUTTON_PIN);

        // Because of the pull-up, the pin reads 0 when pressed.
        if (last_level == 1 && level == 0) {
            ESP_LOGI(TAG, "Button Pressed!");
            alarm_manager_snooze();
            vTaskDelay(pdMS_TO_TICKS(300)); 
        }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(50)); // Check every 50ms
    }
}
