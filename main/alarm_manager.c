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

static bool alarm_schedule_equal(const alarm_entry_t *left, const alarm_entry_t *right)
{
    return left->enabled == right->enabled &&
           left->hour == right->hour &&
           left->minute == right->minute &&
           left->snooze_minutes == right->snooze_minutes &&
           left->track == right->track &&
           left->days_mask == right->days_mask &&
           strcmp(left->label, right->label) == 0;
}

static int alarm_day_key(const struct tm *local_time)
{
    return (local_time->tm_year * 1000) + local_time->tm_yday;
}

static bool alarm_runs_today(const alarm_entry_t *alarm, const struct tm *local_time)
{
    uint8_t days_mask = alarm->days_mask == 0 ? ALARM_DAYS_EVERYDAY : alarm->days_mask;
    return (days_mask & (1U << local_time->tm_wday)) != 0;
}

static int first_relevant_alarm_index_locked(void)
{
    for (int i = 0; i < s_state.alarm_count; i++) {
        if (s_state.alarms[i].active) {
            return i;
        }
    }
    for (int i = 0; i < s_state.alarm_count; i++) {
        if (s_state.alarms[i].enabled) {
            return i;
        }
    }
    return s_state.alarm_count > 0 ? 0 : -1;
}

static void refresh_alarm_summary_locked(void)
{
    int active_count = 0;
    bool any_enabled = false;
    int64_t next_snoozed_until = 0;
    int64_t latest_alarm_epoch = 0;

    for (int i = 0; i < s_state.alarm_count; i++) {
        alarm_entry_t *alarm = &s_state.alarms[i];
        if (alarm->enabled) {
            any_enabled = true;
        }
        if (alarm->active) {
            active_count++;
        }
        if (alarm->snoozed_until_epoch > 0 &&
            (next_snoozed_until == 0 || alarm->snoozed_until_epoch < next_snoozed_until)) {
            next_snoozed_until = alarm->snoozed_until_epoch;
        }
        if (alarm->last_alarm_epoch > latest_alarm_epoch) {
            latest_alarm_epoch = alarm->last_alarm_epoch;
        }
    }

    int selected_index = first_relevant_alarm_index_locked();
    s_state.alarm_enabled = any_enabled;
    s_state.ringing = active_count > 0;
    s_state.active_alarm_count = active_count;
    s_state.snoozed_until_epoch = next_snoozed_until;
    s_state.last_alarm_epoch = latest_alarm_epoch;

    if (selected_index >= 0) {
        const alarm_entry_t *selected = &s_state.alarms[selected_index];
        s_state.alarm_hour = selected->hour;
        s_state.alarm_minute = selected->minute;
        s_state.snooze_minutes = selected->snooze_minutes;
        s_state.volume = selected->volume;
        s_state.track = selected->track;
    } else {
        s_state.alarm_hour = APP_DEFAULT_ALARM_HOUR;
        s_state.alarm_minute = APP_DEFAULT_ALARM_MINUTE;
        s_state.snooze_minutes = APP_DEFAULT_SNOOZE_MINUTES;
        s_state.volume = 20;
        s_state.track = 1;
    }
}

static alarm_entry_t normalized_alarm(const alarm_entry_t *source, int index)
{
    alarm_entry_t alarm = {0};
    if (source != NULL) {
        alarm = *source;
    }

    if (alarm.hour < 0 || alarm.hour > 23) {
        alarm.hour = APP_DEFAULT_ALARM_HOUR;
    }
    if (alarm.minute < 0 || alarm.minute > 59) {
        alarm.minute = APP_DEFAULT_ALARM_MINUTE;
    }
    if (alarm.snooze_minutes < 1 || alarm.snooze_minutes > 120) {
        alarm.snooze_minutes = APP_DEFAULT_SNOOZE_MINUTES;
    }
    if (alarm.volume < 0 || alarm.volume > 30) {
        alarm.volume = 20;
    }
    if (alarm.track < 1 || alarm.track > 255) {
        alarm.track = 1;
    }
    if (alarm.days_mask == 0 || (alarm.days_mask & ~ALARM_DAYS_EVERYDAY) != 0) {
        alarm.days_mask = ALARM_DAYS_EVERYDAY;
    }
    if (alarm.label[0] == '\0') {
        snprintf(alarm.label, sizeof(alarm.label), "Alarm %d", index + 1);
    }

    return alarm;
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

    s_state.alarm_count = 1;
    s_state.alarms[0].enabled =
#ifdef CONFIG_SMART_ALARM_DEFAULT_ENABLED
        true;
#else
        false;
#endif
    s_state.alarms[0].hour = APP_DEFAULT_ALARM_HOUR;
    s_state.alarms[0].minute = APP_DEFAULT_ALARM_MINUTE;
    s_state.alarms[0].snooze_minutes = APP_DEFAULT_SNOOZE_MINUTES;
    s_state.alarms[0].volume = 20;
    s_state.alarms[0].track = 1;
    s_state.alarms[0].days_mask = ALARM_DAYS_EVERYDAY;
    s_state.alarms[0].last_fired_day_key = -1;
    copy_text(s_state.alarms[0].label, sizeof(s_state.alarms[0].label), "Alarm 1");
    refresh_alarm_summary_locked();

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
    bool was_ringing = s_state.ringing;
    alarm_entry_t previous[ALARM_MAX_COUNT];
    int previous_count = s_state.alarm_count;
    memcpy(previous, s_state.alarms, sizeof(previous));

    int next_count = settings->count;
    if (next_count < 0) {
        next_count = 0;
    }
    if (next_count > ALARM_MAX_COUNT) {
        next_count = ALARM_MAX_COUNT;
    }

    memset(s_state.alarms, 0, sizeof(s_state.alarms));
    s_state.alarm_count = next_count;
    for (int i = 0; i < next_count; i++) {
        alarm_entry_t next_alarm = normalized_alarm(&settings->alarms[i], i);
        next_alarm.active = false;
        next_alarm.snoozed_until_epoch = 0;
        next_alarm.snoozed_until_uptime_ms = 0;
        next_alarm.last_alarm_epoch = 0;
        next_alarm.last_fired_day_key = -1;

        if (i < previous_count && alarm_schedule_equal(&previous[i], &next_alarm)) {
            next_alarm.active = previous[i].enabled && previous[i].active;
            next_alarm.snoozed_until_epoch = previous[i].enabled ? previous[i].snoozed_until_epoch : 0;
            next_alarm.snoozed_until_uptime_ms = previous[i].enabled ? previous[i].snoozed_until_uptime_ms : 0;
            next_alarm.last_alarm_epoch = previous[i].last_alarm_epoch;
            next_alarm.last_fired_day_key = previous[i].last_fired_day_key;
        }

        if (!next_alarm.enabled) {
            next_alarm.active = false;
            next_alarm.snoozed_until_epoch = 0;
            next_alarm.snoozed_until_uptime_ms = 0;
        }

        s_state.alarms[i] = next_alarm;
    }

    s_state.last_settings_sync_epoch = time_is_valid(server_epoch) ? server_epoch : time(NULL);
    refresh_alarm_summary_locked();
    should_stop_audio = was_ringing && !s_state.ringing;
    unlock_state();

    if (should_stop_audio) {
        audio_player_stop();
    }

    ESP_LOGI(TAG, "Alarm settings updated: %d alarm(s)", next_count);
}

void alarm_manager_get_settings(alarm_settings_t *out_settings)
{
    if (out_settings == NULL) {
        return;
    }

    lock_state();
    out_settings->count = s_state.alarm_count;
    memcpy(out_settings->alarms, s_state.alarms, sizeof(out_settings->alarms));
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

void alarm_manager_update_air_quality(bool valid, int air_quality_index, int eco2_ppm, int tvoc_ppb)
{
    lock_state();
    s_state.air_quality_valid = valid;
    s_state.air_quality_index = valid ? air_quality_index : 0;
    s_state.eco2_ppm = valid ? eco2_ppm : 0;
    s_state.tvoc_ppb = valid ? tvoc_ppb : 0;
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
    int64_t now_ms = esp_timer_get_time() / 1000;

    lock_state();
    if (s_state.ringing) {
        for (int i = 0; i < s_state.alarm_count; i++) {
            alarm_entry_t *alarm = &s_state.alarms[i];
            if (!alarm->active) {
                continue;
            }

            int snooze_seconds = alarm->snooze_minutes * 60;
            if (snooze_seconds <= 0) {
                snooze_seconds = 300;
            }

            alarm->active = false;
            alarm->snoozed_until_epoch = time_is_valid(now) ? (int64_t)now + snooze_seconds : 0;
            alarm->snoozed_until_uptime_ms = now_ms + ((int64_t)snooze_seconds * 1000);
            ESP_LOGI(TAG, "Alarm '%s' snoozed for %d minutes", alarm->label, alarm->snooze_minutes);
        }

        refresh_alarm_summary_locked();
        should_stop_audio = true;
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
    for (int i = 0; i < s_state.alarm_count; i++) {
        s_state.alarms[i].active = false;
        s_state.alarms[i].snoozed_until_epoch = 0;
        s_state.alarms[i].snoozed_until_uptime_ms = 0;
    }
    refresh_alarm_summary_locked();
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
        uint8_t play_volume = 20;
        uint16_t play_track = 1;
        time_t now = time(NULL);
        int64_t now_ms = esp_timer_get_time() / 1000;

        lock_state();
        s_state.time_valid = time_is_valid(now);

        bool was_ringing = s_state.ringing;
        bool newly_active = false;
        struct tm local_now = {0};
        int today_key = -1;
        if (s_state.time_valid) {
            localtime_r(&now, &local_now);
            today_key = alarm_day_key(&local_now);
        }

        for (int i = 0; i < s_state.alarm_count; i++) {
            alarm_entry_t *alarm = &s_state.alarms[i];
            if (!alarm->enabled || alarm->active) {
                continue;
            }

            if (alarm->snoozed_until_uptime_ms > 0 && now_ms >= alarm->snoozed_until_uptime_ms) {
                alarm->active = true;
                alarm->snoozed_until_epoch = 0;
                alarm->snoozed_until_uptime_ms = 0;
                alarm->last_alarm_epoch = s_state.time_valid ? now : 0;
                newly_active = true;
                play_volume = (uint8_t)alarm->volume;
                play_track = (uint16_t)alarm->track;
                ESP_LOGI(TAG, "Snoozed alarm '%s' ringing with track %d", alarm->label, alarm->track);
                continue;
            }

            if (!s_state.time_valid) {
                continue;
            }

            if (alarm->snoozed_until_uptime_ms == 0 &&
                alarm_runs_today(alarm, &local_now) &&
                local_now.tm_hour == alarm->hour &&
                local_now.tm_min == alarm->minute &&
                alarm->last_fired_day_key != today_key) {
                alarm->active = true;
                alarm->last_alarm_epoch = now;
                alarm->last_fired_day_key = today_key;
                newly_active = true;
                play_volume = (uint8_t)alarm->volume;
                play_track = (uint16_t)alarm->track;
                ESP_LOGI(TAG, "Alarm '%s' ringing with track %d", alarm->label, alarm->track);
            }
        }

        refresh_alarm_summary_locked();
        should_start_audio = newly_active && !was_ringing && s_state.ringing;
        unlock_state();

        if (should_start_audio) {
            audio_player_play_alarm(play_volume, play_track);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
