#include "network_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm_manager.h"
#include "app_config.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

static const char *TAG = "NET_CLIENT";

#define HTTP_RESPONSE_BUFFER_SIZE 2048
#define HTTP_URL_BUFFER_SIZE 256

typedef struct {
    char data[HTTP_RESPONSE_BUFFER_SIZE];
    int length;
    bool overflow;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && response != NULL && evt->data_len > 0) {
        int available = (int)sizeof(response->data) - response->length - 1;
        if (evt->data_len > available) {
            response->overflow = true;
            return ESP_FAIL;
        }

        memcpy(response->data + response->length, evt->data, evt->data_len);
        response->length += evt->data_len;
        response->data[response->length] = '\0';
    }

    return ESP_OK;
}

static void build_url(char *buffer, size_t buffer_size, const char *path)
{
    size_t base_len = strlen(APP_SERVER_URL);
    bool base_has_slash = base_len > 0 && APP_SERVER_URL[base_len - 1] == '/';
    bool path_has_slash = path[0] == '/';

    if (base_has_slash && path_has_slash) {
        snprintf(buffer, buffer_size, "%s%s", APP_SERVER_URL, path + 1);
    } else if (!base_has_slash && !path_has_slash) {
        snprintf(buffer, buffer_size, "%s/%s", APP_SERVER_URL, path);
    } else {
        snprintf(buffer, buffer_size, "%s%s", APP_SERVER_URL, path);
    }
}

static const char *find_json_value(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *position = strstr(json, pattern);
    if (position == NULL) {
        return NULL;
    }

    position = strchr(position + strlen(pattern), ':');
    if (position == NULL) {
        return NULL;
    }

    position++;
    while (*position == ' ' || *position == '\t' || *position == '\r' || *position == '\n') {
        position++;
    }
    return position;
}

static bool json_get_bool(const char *json, const char *key, bool *out_value)
{
    const char *value = find_json_value(json, key);
    if (value == NULL || out_value == NULL) {
        return false;
    }

    if (strncmp(value, "true", 4) == 0) {
        *out_value = true;
        return true;
    }
    if (strncmp(value, "false", 5) == 0) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool json_get_int(const char *json, const char *key, int *out_value)
{
    const char *value = find_json_value(json, key);
    if (value == NULL || out_value == NULL) {
        return false;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
        return false;
    }

    *out_value = (int)parsed;
    return true;
}

static bool json_get_epoch(const char *json, const char *key, time_t *out_value)
{
    const char *value = find_json_value(json, key);
    if (value == NULL || out_value == NULL) {
        return false;
    }

    char *end = NULL;
    long long parsed = strtoll(value, &end, 10);
    if (end == value) {
        return false;
    }

    *out_value = (time_t)parsed;
    return true;
}

static bool json_get_string(const char *json, const char *key, char *out_value, size_t out_size)
{
    const char *value = find_json_value(json, key);
    if (value == NULL || out_value == NULL || out_size == 0 || *value != '"') {
        return false;
    }

    value++;
    size_t copied = 0;
    while (*value != '\0' && *value != '"') {
        if (*value == '\\' && value[1] != '\0') {
            value++;
        }
        if (copied + 1 >= out_size) {
            return false;
        }
        out_value[copied++] = *value++;
    }

    if (*value != '"') {
        return false;
    }

    out_value[copied] = '\0';
    return true;
}

static void json_escape(const char *input, char *output, size_t output_size)
{
    if (output_size == 0) {
        return;
    }

    if (input == NULL) {
        output[0] = '\0';
        return;
    }

    size_t written = 0;
    while (*input != '\0' && written + 1 < output_size) {
        if ((*input == '"' || *input == '\\') && written + 2 < output_size) {
            output[written++] = '\\';
            output[written++] = *input++;
        } else if ((unsigned char)*input >= 0x20) {
            output[written++] = *input++;
        } else {
            input++;
        }
    }
    output[written] = '\0';
}

static bool parse_alarm_time(const char *alarm_time, int *hour, int *minute)
{
    if (alarm_time == NULL || hour == NULL || minute == NULL) {
        return false;
    }

    int parsed_hour = -1;
    int parsed_minute = -1;
    if (sscanf(alarm_time, "%d:%d", &parsed_hour, &parsed_minute) != 2) {
        return false;
    }

    if (parsed_hour < 0 || parsed_hour > 23 || parsed_minute < 0 || parsed_minute > 59) {
        return false;
    }

    *hour = parsed_hour;
    *minute = parsed_minute;
    return true;
}

static esp_err_t fetch_alarm_settings(void)
{
    char url[HTTP_URL_BUFFER_SIZE];
    build_url(url, sizeof(url), "/api/alarm-settings");

    http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = APP_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        alarm_manager_set_error("Failed to initialize HTTP settings client");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Settings poll failed: %s", esp_err_to_name(err));
        alarm_manager_set_error("Settings poll failed");
        return err;
    }

    if (response.overflow) {
        alarm_manager_set_error("Settings response too large");
        return ESP_ERR_INVALID_SIZE;
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "Settings endpoint returned HTTP %d", status_code);
        alarm_manager_set_error("Settings endpoint returned error");
        return ESP_FAIL;
    }

    alarm_settings_t settings;
    alarm_manager_get_settings(&settings);

    bool enabled = false;
    if (json_get_bool(response.data, "enabled", &enabled)) {
        settings.enabled = enabled;
    }

    char alarm_time[8];
    if (json_get_string(response.data, "alarm_time", alarm_time, sizeof(alarm_time))) {
        int hour = settings.hour;
        int minute = settings.minute;
        if (parse_alarm_time(alarm_time, &hour, &minute)) {
            settings.hour = hour;
            settings.minute = minute;
        }
    }

    int snooze_minutes = settings.snooze_minutes;
    if (json_get_int(response.data, "snooze_minutes", &snooze_minutes)) {
        if (snooze_minutes >= 1 && snooze_minutes <= 120) {
            settings.snooze_minutes = snooze_minutes;
        }
    }

    time_t server_epoch = 0;
    json_get_epoch(response.data, "server_epoch", &server_epoch);

    alarm_manager_apply_settings(&settings, server_epoch);
    alarm_manager_set_error("");
    return ESP_OK;
}

static char *build_status_json(void)
{
    alarm_status_t status;
    alarm_manager_get_status(&status);

    char alarm_time[8];
    snprintf(alarm_time, sizeof(alarm_time), "%02d:%02d", status.alarm_hour, status.alarm_minute);

    char *json = malloc(1536);
    if (json == NULL) {
        return NULL;
    }

    char escaped_device_id[80];
    char escaped_last_error[280];
    json_escape(status.device_id, escaped_device_id, sizeof(escaped_device_id));
    json_escape(status.last_error, escaped_last_error, sizeof(escaped_last_error));

    time_t now = time(NULL);
    snprintf(json, 1536,
             "{"
             "\"device_id\":\"%s\","
             "\"wifi_connected\":%s,"
             "\"alarm_enabled\":%s,"
             "\"alarm_time\":\"%s\","
             "\"alarm_hour\":%d,"
             "\"alarm_minute\":%d,"
             "\"snooze_minutes\":%d,"
             "\"ringing\":%s,"
             "\"snoozed_until_epoch\":%lld,"
             "\"last_alarm_epoch\":%lld,"
             "\"last_settings_sync_epoch\":%lld,"
             "\"last_status_upload_epoch\":%lld,"
             "\"uptime_ms\":%lld,"
             "\"time_valid\":%s,"
             "\"environment_valid\":%s,"
             "\"temperature_c\":%.2f,"
             "\"humidity_percent\":%.2f,"
             "\"last_error\":\"%s\","
             "\"device_epoch\":%lld"
             "}",
             escaped_device_id,
             wifi_manager_is_connected() ? "true" : "false",
             status.alarm_enabled ? "true" : "false",
             alarm_time,
             status.alarm_hour,
             status.alarm_minute,
             status.snooze_minutes,
             status.ringing ? "true" : "false",
             (long long)status.snoozed_until_epoch,
             (long long)status.last_alarm_epoch,
             (long long)status.last_settings_sync_epoch,
             (long long)status.last_status_upload_epoch,
             (long long)status.uptime_ms,
             status.time_valid ? "true" : "false",
             status.environment_valid ? "true" : "false",
             status.environment_valid ? status.temperature_c : 0.0f,
             status.environment_valid ? status.humidity_percent : 0.0f,
             escaped_last_error,
             (long long)now);
    return json;
}

static esp_err_t upload_status(void)
{
    char url[HTTP_URL_BUFFER_SIZE];
    build_url(url, sizeof(url), "/api/status");

    char *payload = build_status_json();
    if (payload == NULL) {
        alarm_manager_set_error("Failed to build status JSON");
        return ESP_ERR_NO_MEM;
    }

    http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = APP_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(payload);
        alarm_manager_set_error("Failed to initialize HTTP status client");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(payload);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Status upload failed: %s", esp_err_to_name(err));
        alarm_manager_set_error("Status upload failed");
        return err;
    }

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Status endpoint returned HTTP %d", status_code);
        alarm_manager_set_error("Status endpoint returned error");
        return ESP_FAIL;
    }

    alarm_manager_mark_status_uploaded();
    alarm_manager_set_error("");
    return ESP_OK;
}

void network_client_task(void *pvParameters)
{
    (void)pvParameters;

    int64_t last_settings_poll_ms = 0;
    int64_t last_status_upload_ms = 0;

    ESP_LOGI(TAG, "Local dashboard API target: %s", APP_SERVER_URL);

    while (1) {
        if (wifi_manager_wait_connected(pdMS_TO_TICKS(5000)) != ESP_OK) {
            ESP_LOGW(TAG, "Waiting for Wi-Fi before syncing with local server");
            alarm_manager_set_error("Waiting for Wi-Fi connection");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        if (last_settings_poll_ms == 0 || now_ms - last_settings_poll_ms >= APP_SETTINGS_POLL_PERIOD_MS) {
            if (fetch_alarm_settings() == ESP_OK) {
                last_settings_poll_ms = now_ms;
            } else {
                last_settings_poll_ms = now_ms - APP_SETTINGS_POLL_PERIOD_MS + 3000;
            }
        }

        if (last_status_upload_ms == 0 || now_ms - last_status_upload_ms >= APP_STATUS_UPLOAD_PERIOD_MS) {
            if (upload_status() == ESP_OK) {
                last_status_upload_ms = now_ms;
            } else {
                last_status_upload_ms = now_ms - APP_STATUS_UPLOAD_PERIOD_MS + 3000;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
