#include "network_client.h"

#include <stdbool.h>
#include <stdarg.h>
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

//fixed-size buffers keep HTTP and JSON handling deterministic on the embedded target
#define HTTP_RESPONSE_BUFFER_SIZE 4096
#define HTTP_URL_BUFFER_SIZE 256
#define STATUS_JSON_BUFFER_SIZE 4096

//accumulates an HTTP response body as ESP-IDF delivers it in data events
typedef struct {
    char data[HTTP_RESPONSE_BUFFER_SIZE];
    int length;
    bool overflow;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t *)evt->user_data;

    //copy each received chunk into the caller-provided response buffer
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
    //join base URL and endpoint path without producing a missing or duplicated slash
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
    //this lightweight parser searches for a top-level key pattern used by the local dashboard API
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
    //read JSON booleans such as true or false from the first matching key
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
    //parse integer fields used by alarm settings
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
    //epoch values may exceed plain int range, so parse them as long long first
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

static bool json_get_float(const char *json, const char *key, float *out_value)
{
    //parse decimal comfort-temperature values from the settings JSON
    const char *value = find_json_value(json, key);
    if (value == NULL || out_value == NULL) {
        return false;
    }

    char *end = NULL;
    float parsed = strtof(value, &end);
    if (end == value) {
        return false;
    }

    *out_value = parsed;
    return true;
}

static bool json_get_string(const char *json, const char *key, char *out_value, size_t out_size)
{
    //copy a quoted JSON string, handling simple escaped characters and enforcing the output size
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

static const char *json_find_matching(const char *start, char open_char, char close_char)
{
    //find the matching bracket/brace while ignoring delimiters that appear inside strings
    bool in_string = false;
    bool escaped = false;
    int depth = 0;

    for (const char *position = start; *position != '\0'; position++) {
        char ch = *position;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
        } else if (ch == open_char) {
            depth++;
        } else if (ch == close_char) {
            depth--;
            if (depth == 0) {
                return position;
            }
        }
    }

    return NULL;
}

static bool json_copy_object(const char *start, const char *end, char *out_value, size_t out_size)
{
    //copy one complete JSON object into a temporary null-terminated buffer for field parsing
    if (start == NULL || end == NULL || out_value == NULL || end < start) {
        return false;
    }

    size_t length = (size_t)(end - start + 1);
    if (length >= out_size) {
        return false;
    }

    memcpy(out_value, start, length);
    out_value[length] = '\0';
    return true;
}

static void append_jsonf(char *buffer, size_t buffer_size, size_t *offset, const char *format, ...)
{
    //append formatted JSON text and move the offset without writing past the end of the buffer
    if (*offset >= buffer_size) {
        return;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *offset, buffer_size - *offset, format, args);
    va_end(args);

    if (written < 0) {
        return;
    }

    size_t available = buffer_size - *offset;
    if ((size_t)written >= available) {
        *offset = buffer_size;
    } else {
        *offset += (size_t)written;
    }
}

static void json_escape(const char *input, char *output, size_t output_size)
{
    //escape characters that could break JSON strings and drop control characters
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
    //the dashboard sends alarm time as HH:MM; reject malformed or out-of-range values
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

static alarm_entry_t default_alarm_entry(int index)
{
    //default values provide a complete alarm even when some JSON fields are omitted
    alarm_entry_t alarm = {0};
    alarm.enabled = false;
    alarm.hour = APP_DEFAULT_ALARM_HOUR;
    alarm.minute = APP_DEFAULT_ALARM_MINUTE;
    alarm.snooze_minutes = APP_DEFAULT_SNOOZE_MINUTES;
    alarm.volume = 20;
    alarm.track = 1;
    alarm.days_mask = ALARM_DAYS_EVERYDAY;
    alarm.last_fired_day_key = -1;
    snprintf(alarm.label, sizeof(alarm.label), "Alarm %d", index + 1);
    return alarm;
}

static bool parse_alarm_object(const char *object_json, alarm_entry_t *alarm, int index)
{
    //parse one alarm object, starting from defaults and overriding only valid fields
    if (object_json == NULL || alarm == NULL) {
        return false;
    }

    alarm_entry_t parsed = default_alarm_entry(index);

    bool enabled = parsed.enabled;
    if (json_get_bool(object_json, "enabled", &enabled)) {
        parsed.enabled = enabled;
    }

    char alarm_time[8];
    if (json_get_string(object_json, "alarm_time", alarm_time, sizeof(alarm_time))) {
        parse_alarm_time(alarm_time, &parsed.hour, &parsed.minute);
    }

    int snooze_minutes = parsed.snooze_minutes;
    if (json_get_int(object_json, "snooze_minutes", &snooze_minutes) &&
        snooze_minutes >= 1 && snooze_minutes <= 120) {
        parsed.snooze_minutes = snooze_minutes;
    }

    int volume = parsed.volume;
    if (json_get_int(object_json, "volume", &volume) &&
        volume >= 0 && volume <= 30) {
        parsed.volume = volume;
    }

    int track = parsed.track;
    if (json_get_int(object_json, "track", &track) &&
        track >= 1 && track <= 255) {
        parsed.track = track;
    }

    int days_mask = parsed.days_mask;
    if (json_get_int(object_json, "days_mask", &days_mask) &&
        days_mask >= 1 && days_mask <= ALARM_DAYS_EVERYDAY) {
        parsed.days_mask = (uint8_t)days_mask;
    }

    json_get_string(object_json, "label", parsed.label, sizeof(parsed.label));
    if (parsed.label[0] == '\0') {
        snprintf(parsed.label, sizeof(parsed.label), "Alarm %d", index + 1);
    }

    *alarm = parsed;
    return true;
}

static bool parse_alarms_array(const char *json, alarm_settings_t *settings)
{
    //parse the modern multi-alarm response format from the dashboard
    const char *array = find_json_value(json, "alarms");
    if (array == NULL || *array != '[' || settings == NULL) {
        return false;
    }

    const char *array_end = json_find_matching(array, '[', ']');
    if (array_end == NULL) {
        return false;
    }

    alarm_settings_t parsed = {0};
    //walk object-by-object through the array until the configured maximum is reached
    const char *position = array + 1;
    while (position < array_end && parsed.count < ALARM_MAX_COUNT) {
        const char *object_start = strchr(position, '{');
        if (object_start == NULL || object_start >= array_end) {
            break;
        }

        const char *object_end = json_find_matching(object_start, '{', '}');
        if (object_end == NULL || object_end > array_end) {
            break;
        }

        char object_json[768];
        if (json_copy_object(object_start, object_end, object_json, sizeof(object_json)) &&
            parse_alarm_object(object_json, &parsed.alarms[parsed.count], parsed.count)) {
            parsed.count++;
        }

        position = object_end + 1;
    }

    *settings = parsed;
    return true;
}

static void parse_legacy_alarm_settings(const char *json, alarm_settings_t *settings)
{
    //older dashboard responses stored a single alarm at the root; map that into alarm slot zero
    alarm_entry_t alarm = settings->count > 0 ? settings->alarms[0] : default_alarm_entry(0);

    bool enabled = alarm.enabled;
    if (json_get_bool(json, "enabled", &enabled)) {
        alarm.enabled = enabled;
    }

    char alarm_time[8];
    if (json_get_string(json, "alarm_time", alarm_time, sizeof(alarm_time))) {
        parse_alarm_time(alarm_time, &alarm.hour, &alarm.minute);
    }

    int snooze_minutes = alarm.snooze_minutes;
    if (json_get_int(json, "snooze_minutes", &snooze_minutes) &&
        snooze_minutes >= 1 && snooze_minutes <= 120) {
        alarm.snooze_minutes = snooze_minutes;
    }

    int volume = alarm.volume;
    if (json_get_int(json, "volume", &volume) &&
        volume >= 0 && volume <= 30) {
        alarm.volume = volume;
    }

    int track = alarm.track;
    if (json_get_int(json, "track", &track) &&
        track >= 1 && track <= 255) {
        alarm.track = track;
    }

    settings->count = 1;
    settings->alarms[0] = alarm;
}

static void parse_comfort_settings(const char *json, alarm_settings_t *settings)
{
    if (json == NULL || settings == NULL) {
        return;
    }

    //the device keeps the last valid comfort range so the dashboard can update one bound at a time
    float min_temperature = settings->comfort_min_temperature;
    float max_temperature = settings->comfort_max_temperature;

    if (json_get_float(json, "comfort_min_temperature", &min_temperature) &&
        (min_temperature < -20.0f || min_temperature > 60.0f)) {
        min_temperature = 18.0f;
    }
    if (json_get_float(json, "comfort_max_temperature", &max_temperature) &&
        (max_temperature < -20.0f || max_temperature > 60.0f)) {
        max_temperature = 26.0f;
    }
    if (min_temperature > max_temperature) {
        float temp = min_temperature;
        min_temperature = max_temperature;
        max_temperature = temp;
    }

    settings->comfort_min_temperature = min_temperature;
    settings->comfort_max_temperature = max_temperature;
}

static esp_err_t fetch_alarm_settings(void)
{
    //poll the dashboard for alarm and comfort configuration
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

    //initialize a short-lived HTTP client for this GET request
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

    //start from current settings so partial responses can preserve existing comfort values
    alarm_settings_t settings;
    alarm_manager_get_settings(&settings);

    //prefer the multi-alarm format, but keep compatibility with the old single-alarm format
    if (!parse_alarms_array(response.data, &settings)) {
        parse_legacy_alarm_settings(response.data, &settings);
    }
    parse_comfort_settings(response.data, &settings);

    time_t server_epoch = 0;
    json_get_epoch(response.data, "server_epoch", &server_epoch);

    alarm_manager_apply_settings(&settings, server_epoch);
    alarm_manager_set_error("");
    return ESP_OK;
}

static char *build_status_json(void)
{
    //convert the local alarm manager state into the dashboard status payload
    alarm_status_t status;
    alarm_manager_get_status(&status);

    char alarm_time[8];
    snprintf(alarm_time, sizeof(alarm_time), "%02d:%02d", status.alarm_hour, status.alarm_minute);

    //allocate on the heap because the JSON buffer is too large for a small task stack
    char *json = malloc(STATUS_JSON_BUFFER_SIZE);
    if (json == NULL) {
        return NULL;
    }

    char escaped_device_id[80];
    char escaped_last_error[280];
    json_escape(status.device_id, escaped_device_id, sizeof(escaped_device_id));
    json_escape(status.last_error, escaped_last_error, sizeof(escaped_last_error));

    time_t now = time(NULL);

    size_t offset = 0;
    //write the fixed status fields first, then append the alarms array below
    append_jsonf(json, STATUS_JSON_BUFFER_SIZE, &offset,
                 "{"
                 "\"device_id\":\"%s\","
                 "\"wifi_connected\":%s,"
                 "\"alarm_enabled\":%s,"
                 "\"alarm_time\":\"%s\","
                 "\"alarm_hour\":%d,"
                 "\"alarm_minute\":%d,"
                 "\"snooze_minutes\":%d,"
                 "\"volume\":%d,"
                 "\"track\":%d,"
                 "\"ringing\":%s,"
                 "\"alarm_count\":%d,"
                 "\"active_alarm_count\":%d,"
                 "\"comfort_min_temperature\":%.2f,"
                 "\"comfort_max_temperature\":%.2f,"
                 "\"snoozed_until_epoch\":%lld,"
                 "\"last_alarm_epoch\":%lld,"
                 "\"last_settings_sync_epoch\":%lld,"
                 "\"last_status_upload_epoch\":%lld,"
                 "\"uptime_ms\":%lld,"
                 "\"time_valid\":%s,"
                 "\"environment_valid\":%s,"
                 "\"temperature_c\":%.2f,"
                 "\"humidity_percent\":%.2f,"
                 "\"air_quality_valid\":%s,"
                 "\"air_quality_index\":%d,"
                 "\"eco2_ppm\":%d,"
                 "\"tvoc_ppb\":%d,"
                 "\"last_error\":\"%s\","
                 "\"device_epoch\":%lld,"
                 "\"alarms\":[",
                 escaped_device_id,
                 wifi_manager_is_connected() ? "true" : "false",
                 status.alarm_enabled ? "true" : "false",
                 alarm_time,
                 status.alarm_hour,
                 status.alarm_minute,
                 status.snooze_minutes,
                 status.volume,
                 status.track,
                 status.ringing ? "true" : "false",
                 status.alarm_count,
                 status.active_alarm_count,
                 status.comfort_min_temperature,
                 status.comfort_max_temperature,
                 (long long)status.snoozed_until_epoch,
                 (long long)status.last_alarm_epoch,
                 (long long)status.last_settings_sync_epoch,
                 (long long)status.last_status_upload_epoch,
                 (long long)status.uptime_ms,
                 status.time_valid ? "true" : "false",
                 status.environment_valid ? "true" : "false",
                 status.environment_valid ? status.temperature_c : 0.0f,
                 status.environment_valid ? status.humidity_percent : 0.0f,
                 status.air_quality_valid ? "true" : "false",
                 status.air_quality_valid ? status.air_quality_index : 0,
                 status.air_quality_valid ? status.eco2_ppm : 0,
                 status.air_quality_valid ? status.tvoc_ppb : 0,
                 escaped_last_error,
                 (long long)now);

    //append each configured alarm as one JSON object
    for (int i = 0; i < status.alarm_count && i < ALARM_MAX_COUNT; i++) {
        const alarm_entry_t *alarm = &status.alarms[i];
        char escaped_label[ALARM_LABEL_SIZE * 2];
        char item_time[8];
        json_escape(alarm->label, escaped_label, sizeof(escaped_label));
        snprintf(item_time, sizeof(item_time), "%02d:%02d", alarm->hour, alarm->minute);
        append_jsonf(json, STATUS_JSON_BUFFER_SIZE, &offset,
                     "%s{"
                     "\"label\":\"%s\","
                     "\"enabled\":%s,"
                     "\"alarm_time\":\"%s\","
                     "\"snooze_minutes\":%d,"
                     "\"volume\":%d,"
                     "\"track\":%d,"
                     "\"days_mask\":%u,"
                     "\"active\":%s,"
                     "\"snoozed_until_epoch\":%lld,"
                     "\"last_alarm_epoch\":%lld"
                     "}",
                     i == 0 ? "" : ",",
                     escaped_label,
                     alarm->enabled ? "true" : "false",
                     item_time,
                     alarm->snooze_minutes,
                     alarm->volume,
                     alarm->track,
                     (unsigned)alarm->days_mask,
                     alarm->active ? "true" : "false",
                     (long long)alarm->snoozed_until_epoch,
                     (long long)alarm->last_alarm_epoch);
    }

    append_jsonf(json, STATUS_JSON_BUFFER_SIZE, &offset, "]}");
    return json;
}

static esp_err_t upload_status(void)
{
    //send the latest device status to the dashboard
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

    //use a short-lived HTTP client for the POST so resources are released after each upload
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

    //zero means "never done", forcing an immediate first settings poll and status upload
    int64_t last_settings_poll_ms = 0;
    int64_t last_status_upload_ms = 0;

    ESP_LOGI(TAG, "Local dashboard API target: %s", APP_SERVER_URL);

    while (1) {
        //network traffic is skipped until Wi-Fi is connected
        if (wifi_manager_wait_connected(pdMS_TO_TICKS(5000)) != ESP_OK) {
            ESP_LOGW(TAG, "Waiting for Wi-Fi before syncing with local server");
            alarm_manager_set_error("Waiting for Wi-Fi connection");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        //poll settings periodically; failed polls are retried soon instead of waiting a full period
        if (last_settings_poll_ms == 0 || now_ms - last_settings_poll_ms >= APP_SETTINGS_POLL_PERIOD_MS) {
            if (fetch_alarm_settings() == ESP_OK) {
                last_settings_poll_ms = now_ms;
            } else {
                last_settings_poll_ms = now_ms - APP_SETTINGS_POLL_PERIOD_MS + 3000;
            }
        }

        //upload status periodically; failed uploads are also retried after a short delay
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
