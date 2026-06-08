#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"

static const char *TAG = "WIFI";

//event-group bits used by other tasks to observe the connection state
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
//initial disconnects are logged as fast retries before switching to quieter background retries
#define WIFI_MAX_FAST_RETRIES 8

//the event group is the shared synchronization object for Wi-Fi readiness
static EventGroupHandle_t s_wifi_event_group;
//counts consecutive disconnects so logs can distinguish first retries from persistent failure
static int s_retry_count;

//converts ESP-IDF disconnect reason codes into readable log messages
static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_AUTHMODE_THRESHOLD";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    default:
        return "OTHER";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    //when the station interface starts, immediately begin the connection attempt
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    //on disconnect, clear the connected bit and ask ESP-IDF to reconnect
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_retry_count++;

        //the fail bit does not stop reconnecting; it only records that fast retries were exhausted
        if (s_retry_count <= WIFI_MAX_FAST_RETRIES) {
            ESP_LOGW(TAG,
                     "Wi-Fi disconnected: reason=%u (%s), retrying (%d/%d)",
                     event->reason,
                     wifi_disconnect_reason_name(event->reason),
                     s_retry_count,
                     WIFI_MAX_FAST_RETRIES);
        } else {
            ESP_LOGW(TAG,
                     "Wi-Fi still disconnected: reason=%u (%s), continuing background retries",
                     event->reason,
                     wifi_disconnect_reason_name(event->reason));
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

        esp_wifi_connect();
        return;
    }

    //getting an IP address means the station is usable by network clients
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected with IP " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_start(void)
{
    //warn early when menuconfig still contains placeholder Wi-Fi settings
    if (strlen(APP_WIFI_SSID) == 0 || strcmp(APP_WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
        ESP_LOGW(TAG, "Wi-Fi SSID is not configured. Set it in idf.py menuconfig.");
    }

    //create the synchronization object before registering handlers that may set its bits
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    //these ESP-IDF init calls may already be done by another module, so INVALID_STATE is accepted
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_sta();

    //initialize the Wi-Fi driver with ESP-IDF default buffer and task settings
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    //build station credentials from compile-time/app configuration
    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", APP_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", APP_WIFI_PASSWORD);
    //empty password means open network; otherwise require at least WPA/WPA2 PSK
    wifi_config.sta.threshold.authmode = strlen(APP_WIFI_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Connecting to Wi-Fi SSID '%s' (password length: %u)",
                 APP_WIFI_SSID,
                 (unsigned)strlen(APP_WIFI_PASSWORD));
    }
    return err;
}

bool wifi_manager_is_connected(void)
{
    //before start-up there is no event group, so the station cannot be connected
    if (s_wifi_event_group == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_wait_connected(TickType_t timeout_ticks)
{
    //callers should start Wi-Fi before waiting on the connection event group.
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    //wait only for the connected bit; it remains set until a disconnect event clears it.
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        timeout_ticks);

    return (bits & WIFI_CONNECTED_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}
