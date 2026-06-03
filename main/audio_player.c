#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO";

#define DFPLAYER_UART_PORT UART_NUM_1
#define DFPLAYER_BAUD_RATE 9600
#define DFPLAYER_TX_PIN GPIO_NUM_17
#define DFPLAYER_RX_PIN GPIO_NUM_16
#define DFPLAYER_DEFAULT_VOLUME 20
#define DFPLAYER_DEFAULT_ALARM_TRACK 1
#define DFPLAYER_PLAY_MP3_FOLDER_COMMAND 0x12

// create the standard transmission packet
static void dfplayer_send_command(uint8_t command, uint16_t parameter) {
    uint8_t packet[10] = {
        0x7E,
        0xFF,
        0x06,
        command,
        0x00,
        (uint8_t)(parameter >> 8),
        (uint8_t)(parameter & 0xFF),
        0x00,
        0x00,
        0xEF,
    };

// checksum to avoid corruptions
    uint16_t checksum = 0;
    for (int i = 1; i < 7; i++) {
        checksum += packet[i];
    }
    checksum = (uint16_t)(0 - checksum);
    packet[7] = (uint8_t)(checksum >> 8);
    packet[8] = (uint8_t)(checksum & 0xFF);

    int written = uart_write_bytes(DFPLAYER_UART_PORT, (const char *)packet, sizeof(packet));
    if (written != (int)sizeof(packet)) {
        ESP_LOGW(TAG, "DFPlayer command 0x%02X write incomplete", command);
    }
}


// initialize the audio player, set the RX and TX, wait for half a second and set a default volume (20)
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

    vTaskDelay(pdMS_TO_TICKS(500));
    audio_player_set_volume(DFPLAYER_DEFAULT_VOLUME);
}

// set default volume at a maximum of 30
void audio_player_set_volume(uint8_t volume) {
    if (volume > 30) {
        volume = 30;
    }
    dfplayer_send_command(0x06, volume);
}

// play a track 
void audio_player_play_alarm(uint8_t volume, uint16_t track) {
    if (track == 0) {
        track = DFPLAYER_DEFAULT_ALARM_TRACK;
    }
    ESP_LOGI(TAG, "Playing MP3 folder alarm track %u at volume %u", (unsigned)track, (unsigned)volume);
    audio_player_set_volume(volume);
    dfplayer_send_command(DFPLAYER_PLAY_MP3_FOLDER_COMMAND, track);
}

// stop the player
void audio_player_stop(void) {
    ESP_LOGI(TAG, "Stopping audio");
    dfplayer_send_command(0x16, 0);
}
