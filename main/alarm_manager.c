#include "alarm_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "app_config.h"
#include "audio_player.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ALARM";

#define VALID_EPOCH_THRESHOLD 1600000000

static SemaphoreHandle_t s_lock;
static alarm_status_t s_state;
static int s_last_alarm_day = -1;

static void copy_text(char *dest, size_t dest_size, const char *src)
{
    if (dest_size == 0) {
        return;
    }
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, dest_size, "%s", src);
}

static bool time_is_valid(time_t now)
{
    return now > VALID_EPOCH_THRESHOLD;
}

static void lock_state(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

void alarm_manager_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    memset(&s_state, 0, sizeof(s_state));
    copy_text(s_state.device_id, sizeof(s_state.device_id), APP_DEVICE_ID);
    s_state.alarm_enabled =
#ifdef CONFIG_SMART_ALARM_DEFAULT_ENABLED
        true;
#else
        false;
#endif
    s_state.alarm_hour = APP_DEFAULT_ALARM_HOUR;
    s_state.alarm_minute = APP_DEFAULT_ALARM_MINUTE;
    s_state.snooze_minutes = APP_DEFAULT_SNOOZE_MINUTES;

    setenv("TZ", APP_TIMEZONE, 1);
    tzset();

    ESP_LOGI(TAG, "Alarm defaults: %s %02d:%02d, snooze %d min, TZ=%s",
             s_state.alarm_enabled ? "enabled" : "disabled",
             s_state.alarm_hour,
             s_state.alarm_minute,
             s_state.snooze_minutes,
             APP_TIMEZONE);
}

void alarm_manager_apply_settings(const alarm_settings_t *settings, time_t server_epoch)
{
    if (settings == NULL) {
        return;
    }

    bool should_stop_audio = false;

    if (time_is_valid(server_epoch)) {
        struct timeval tv = {
            .tv_sec = server_epoch,
            .tv_usec = 0,
        };
        if (settimeofday(&tv, NULL) == 0) {
            ESP_LOGI(TAG, "Clock synchronized from local server: %lld", (long long)server_epoch);
        } else {
            ESP_LOGW(TAG, "Failed to synchronize local clock");
        }
    }

    lock_state();
    bool schedule_changed = s_state.alarm_hour != settings->hour ||
                            s_state.alarm_minute != settings->minute ||
                            (!s_state.alarm_enabled && settings->enabled);
    s_state.alarm_enabled = settings->enabled;
    s_state.alarm_hour = settings->hour;
    s_state.alarm_minute = settings->minute;
    s_state.snooze_minutes = settings->snooze_minutes;
    s_state.last_settings_sync_epoch = time_is_valid(server_epoch) ? server_epoch : time(NULL);
    if (schedule_changed) {
        s_last_alarm_day = -1;
    }

    if (!settings->enabled && s_state.ringing) {
        s_state.ringing = false;
        s_state.snoozed_until_epoch = 0;
        should_stop_audio = true;
    }
    unlock_state();

    if (should_stop_audio) {
        audio_player_stop();
    }

    ESP_LOGI(TAG, "Alarm settings updated: %s %02d:%02d, snooze %d min",
             settings->enabled ? "enabled" : "disabled",
             settings->hour,
             settings->minute,
             settings->snooze_minutes);
}

void alarm_manager_get_settings(alarm_settings_t *out_settings)
{
    if (out_settings == NULL) {
        return;
    }

    lock_state();
    out_settings->enabled = s_state.alarm_enabled;
    out_settings->hour = s_state.alarm_hour;
    out_settings->minute = s_state.alarm_minute;
    out_settings->snooze_minutes = s_state.snooze_minutes;
    unlock_state();
}

void alarm_manager_get_status(alarm_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }

    lock_state();
    *out_status = s_state;
    out_status->uptime_ms = esp_timer_get_time() / 1000;
    out_status->time_valid = time_is_valid(time(NULL));
    unlock_state();
}

void alarm_manager_update_environment(float temperature_c, float humidity_percent)
{
    lock_state();
    s_state.temperature_c = temperature_c;
    s_state.humidity_percent = humidity_percent;
    s_state.environment_valid = true;
    unlock_state();
}

void alarm_manager_mark_status_uploaded(void)
{
    lock_state();
    s_state.last_status_upload_epoch = time(NULL);
    unlock_state();
}

void alarm_manager_set_error(const char *message)
{
    lock_state();
    copy_text(s_state.last_error, sizeof(s_state.last_error), message);
    unlock_state();
}

void alarm_manager_snooze(void)
{
    bool should_stop_audio = false;
    time_t now = time(NULL);

    lock_state();
    if (s_state.ringing) {
        int snooze_seconds = s_state.snooze_minutes * 60;
        if (snooze_seconds <= 0) {
            snooze_seconds = 300;
        }

        s_state.ringing = false;
        s_state.snoozed_until_epoch = time_is_valid(now) ? (int64_t)now + snooze_seconds : 0;
        should_stop_audio = true;
        ESP_LOGI(TAG, "Alarm snoozed for %d minutes", s_state.snooze_minutes);
    }
    unlock_state();

    if (should_stop_audio) {
        audio_player_stop();
    }
}

void alarm_manager_stop(void)
{
    bool should_stop_audio = false;

    lock_state();
    if (s_state.ringing) {
        should_stop_audio = true;
    }
    s_state.ringing = false;
    s_state.snoozed_until_epoch = 0;
    unlock_state();

    if (should_stop_audio) {
        audio_player_stop();
    }
}

void alarm_manager_task(void *pvParameters)
{
    (void)pvParameters;

    while (1) {
        bool should_start_audio = false;
        time_t now = time(NULL);

        lock_state();
        s_state.time_valid = time_is_valid(now);

        if (s_state.time_valid && !s_state.ringing) {
            if (s_state.snoozed_until_epoch > 0 && now >= s_state.snoozed_until_epoch) {
                s_state.ringing = true;
                s_state.snoozed_until_epoch = 0;
                s_state.last_alarm_epoch = now;
                should_start_audio = true;
            } else if (s_state.alarm_enabled && s_state.snoozed_until_epoch == 0) {
                struct tm local_now;
                localtime_r(&now, &local_now);
                int today_key = (local_now.tm_year * 1000) + local_now.tm_yday;

                if (local_now.tm_hour == s_state.alarm_hour &&
                    local_now.tm_min == s_state.alarm_minute &&
                    s_last_alarm_day != today_key) {
                    s_state.ringing = true;
                    s_state.last_alarm_epoch = now;
                    s_last_alarm_day = today_key;
                    should_start_audio = true;
                }
            }
        }
        unlock_state();

        if (should_start_audio) {
            ESP_LOGI(TAG, "Alarm ringing");
            audio_player_play_alarm();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
