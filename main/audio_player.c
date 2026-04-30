#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "audio_player.h"

static const char *TAG = "AUDIO";

#define DFPLAYER_UART_PORT UART_NUM_1
#define DFPLAYER_BAUD_RATE 9600
#define DFPLAYER_TX_PIN GPIO_NUM_17
#define DFPLAYER_RX_PIN GPIO_NUM_16

void init_audio_player(void) {
    uart_config_t uart_config = {
        .baud_rate = DFPLAYER_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(DFPLAYER_UART_PORT, &uart_config);
    uart_set_pin(DFPLAYER_UART_PORT, DFPLAYER_TX_PIN, DFPLAYER_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(DFPLAYER_UART_PORT, 256, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "Audio UART Initialized on TX:17, RX:16");
}