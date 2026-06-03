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
#define DEFAULT_COMFORT_MIN_TEMPERATURE 18.0f
#define DEFAULT_COMFORT_MAX_TEMPERATURE 26.0f
#define COMFORT_ALERT_AQI_THRESHOLD 3
#define COMFORT_ALERT_TRACK 5
#define COMFORT_ALERT_DURATION_MS 5000
#define COMFORT_ALERT_REPEAT_INTERVAL_MS (30 * 60 * 1000)
#define ALARM_RING_TIMEOUT_MS 60000

static SemaphoreHandle_t s_lock;
static alarm_status_t s_state;
static bool s_audio_is_playing;
static uint8_t s_audio_volume;
static uint16_t s_audio_track;
static bool s_comfort_alert_active;
static int64_t s_comfort_alert_until_uptime_ms;
static int64_t s_next_comfort_alert_check_uptime_ms;

static int oldest_active_alarm_index_locked(void);

//helper functions
static void copy_text(char *dest, size_t dest_size, const char *src) { //copies safely string avoiding program crashes, used when alarm label updated
    if (dest_size == 0) {
        return;
    }
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, dest_size, "%s", src);
}

static bool time_is_valid(time_t now) { //check if internal clock knows a reasonable time
    return now > VALID_EPOCH_THRESHOLD;
}

static bool alarm_schedule_equal(const alarm_entry_t *left, const alarm_entry_t *right) { //compares 2 alarms entries to check if they are the same, to verify if an alarm has been modifed
    return left->enabled == right->enabled &&
           left->hour == right->hour &&
           left->minute == right->minute &&
           left->snooze_minutes == right->snooze_minutes &&
           left->track == right->track &&
           left->days_mask == right->days_mask &&
           strcmp(left->label, right->label) == 0;
}

static int alarm_day_key(const struct tm *local_time) { //generates key based on the exact day, to track when alarm last ringed and avoid ringing multiple times
    return (local_time->tm_year * 1000) + local_time->tm_yday;
}

static bool alarm_runs_today(const alarm_entry_t *alarm, const struct tm *local_time) { //check if an alarm is scheduled to run today based on the days mask and current day of week
    uint8_t days_mask = alarm->days_mask == 0 ? ALARM_DAYS_EVERYDAY : alarm->days_mask;
    return (days_mask & (1U << local_time->tm_wday)) != 0;
}

static void normalize_comfort_range(float *min_temperature, float *max_temperature) { //keeps the selected comfort range inside the same limits used by the web dashboard
    if (min_temperature == NULL || max_temperature == NULL) {
        return;
    }

    if (*min_temperature < -20.0f || *min_temperature > 60.0f) {
        *min_temperature = DEFAULT_COMFORT_MIN_TEMPERATURE;
    }
    if (*max_temperature < -20.0f || *max_temperature > 60.0f) {
        *max_temperature = DEFAULT_COMFORT_MAX_TEMPERATURE;
    }
    if (*min_temperature > *max_temperature) {
        float tmp = *min_temperature;
        *min_temperature = *max_temperature;
        *max_temperature = tmp;
    }
}

static bool comfort_condition_active_locked(void) { //checks whether the current sensor values are outside the user comfort range
    bool temperature_outside_range = s_state.environment_valid &&
                                     (s_state.temperature_c < s_state.comfort_min_temperature ||
                                      s_state.temperature_c > s_state.comfort_max_temperature);
    bool air_quality_bad = s_state.air_quality_valid && s_state.air_quality_index > COMFORT_ALERT_AQI_THRESHOLD;
    return temperature_outside_range || air_quality_bad;
}

static void expire_comfort_alert_locked(int64_t now_ms) { //clears the alert once its five-second playback window has finished
    if (s_comfort_alert_active && now_ms >= s_comfort_alert_until_uptime_ms) {
        s_comfort_alert_active = false;
        s_comfort_alert_until_uptime_ms = 0;
    }
}

static void maybe_start_comfort_alert_locked(int64_t now_ms) { //starts a short alert, then waits 30 minutes before rechecking persistent bad conditions
    bool condition_active = comfort_condition_active_locked();
    if (!condition_active) {
        s_next_comfort_alert_check_uptime_ms = 0;
        return;
    }

    if (!s_comfort_alert_active &&
        (s_next_comfort_alert_check_uptime_ms == 0 || now_ms >= s_next_comfort_alert_check_uptime_ms)) {
        s_comfort_alert_active = true;
        s_comfort_alert_until_uptime_ms = now_ms + COMFORT_ALERT_DURATION_MS;
        s_next_comfort_alert_check_uptime_ms = now_ms + COMFORT_ALERT_REPEAT_INTERVAL_MS;
        ESP_LOGI(TAG, "Comfort alert triggered");
    }
}

static void resolve_audio_output_locked(time_t now, int64_t now_ms, bool *should_stop_audio, bool *should_play_audio, uint8_t *play_volume, uint16_t *play_track) { //keeps the DFPlayer aligned with the current alarm or comfort alert that should be audible
    (void)now;

    if (should_stop_audio == NULL || should_play_audio == NULL || play_volume == NULL || play_track == NULL) {
        return;
    }

    *should_stop_audio = false;
    *should_play_audio = false;
    *play_volume = 20;
    *play_track = 1;

    uint8_t desired_volume = 0;
    uint16_t desired_track = 0;
    bool desired_audio = false;

    if (s_comfort_alert_active && now_ms < s_comfort_alert_until_uptime_ms) {
        desired_audio = true;
        desired_volume = 20;
        desired_track = COMFORT_ALERT_TRACK;
    } else {
        int alarm_index = oldest_active_alarm_index_locked();
        if (alarm_index >= 0) {
            const alarm_entry_t *alarm = &s_state.alarms[alarm_index];
            desired_audio = true;
            desired_volume = (uint8_t)alarm->volume;
            desired_track = (uint16_t)alarm->track;
        }
    }

    if (!desired_audio) {
        if (s_audio_is_playing) {
            s_audio_is_playing = false;
            s_audio_volume = 0;
            s_audio_track = 0;
            *should_stop_audio = true;
        }
        return;
    }

    if (!s_audio_is_playing || s_audio_volume != desired_volume || s_audio_track != desired_track) {
        s_audio_is_playing = true;
        s_audio_volume = desired_volume;
        s_audio_track = desired_track;
        *should_play_audio = true;
        *play_volume = desired_volume;
        *play_track = desired_track;
    }
}

static int oldest_active_alarm_index_locked(void) { //finds the active alarm that started first, so snooze only affects the alarm the user most likely meant to silence
    int selected_index = -1;
    int64_t selected_alarm_epoch = 0;

    for (int i = 0; i < s_state.alarm_count; i++) {
        alarm_entry_t *alarm = &s_state.alarms[i];
        if (!alarm->active) {
            continue;
        }

        if (selected_index < 0 || alarm->last_alarm_epoch < selected_alarm_epoch) {
            selected_index = i;
            selected_alarm_epoch = alarm->last_alarm_epoch;
        }
    }

    return selected_index;
}

static int first_relevant_alarm_index_locked(void) { //prefers the alarm that is actually ringing, and if nothing is ringing yet it falls back to the first enabled alarm
    int active_alarm_index = oldest_active_alarm_index_locked();
    if (active_alarm_index >= 0) {
        return active_alarm_index;
    }

    for (int i = 0; i < s_state.alarm_count; i++) {
        if (s_state.alarms[i].enabled) {
            return i;
        }
    }
    return s_state.alarm_count > 0 ? 0 : -1;
}

static void refresh_alarm_summary_locked(void) { //updates summary fields in the state based on the current alarms, to avoid having to calculate them multiple times when showing the clock screen or responding to status requests
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

static alarm_entry_t normalized_alarm(const alarm_entry_t *source, int index) { //if inputs are invalid, it rewrites them back to safe default values 
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

static void lock_state(void){ //locks the state mutex, to protect concurrent access to the state from multiple tasks
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_state(void){ //unlocks the state mutex
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}


//functions used in other files
void alarm_manager_init(void){ //prepares system for first use, initializing mutex, state and timezone
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    memset(&s_state, 0, sizeof(s_state));
    copy_text(s_state.device_id, sizeof(s_state.device_id), APP_DEVICE_ID);
    s_state.comfort_min_temperature = DEFAULT_COMFORT_MIN_TEMPERATURE;
    s_state.comfort_max_temperature = DEFAULT_COMFORT_MAX_TEMPERATURE;
    s_audio_is_playing = false;
    s_audio_volume = 0;
    s_audio_track = 0;
    s_comfort_alert_active = false;
    s_comfort_alert_until_uptime_ms = 0;
    s_next_comfort_alert_check_uptime_ms = 0;

    s_state.alarm_count = 1;
    s_state.alarms[0].enabled =
#ifdef CONFIG_SMART_ALARM_DEFAULT_ENABLED
        true;
#else
        false;
#endif
    //default values, if no alarm configured or if internet not connected
    s_state.alarms[0].hour = APP_DEFAULT_ALARM_HOUR;
    s_state.alarms[0].minute = APP_DEFAULT_ALARM_MINUTE;
    s_state.alarms[0].snooze_minutes = APP_DEFAULT_SNOOZE_MINUTES;
    s_state.alarms[0].volume = 20;
    s_state.alarms[0].track = 1;
    s_state.alarms[0].days_mask = ALARM_DAYS_EVERYDAY;
    s_state.alarms[0].last_fired_day_key = -1;
    s_state.alarms[0].active_since_uptime_ms = 0;
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

void alarm_manager_apply_settings(const alarm_settings_t *settings, time_t server_epoch){ //synchronizes clock if server time valid, updates alarm settings.
    if (settings == NULL) {
        return;
    }

    bool should_stop_audio = false;
    bool should_play_audio = false;
    uint8_t play_volume = 20;
    uint16_t play_track = 1;
    int64_t now_ms = esp_timer_get_time() / 1000;

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
    alarm_entry_t previous[ALARM_MAX_COUNT];
    int previous_count = s_state.alarm_count;
    memcpy(previous, s_state.alarms, sizeof(previous));

    float next_comfort_min = settings->comfort_min_temperature;
    float next_comfort_max = settings->comfort_max_temperature;
    normalize_comfort_range(&next_comfort_min, &next_comfort_max);

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
        next_alarm.active_since_uptime_ms = 0;
        next_alarm.snoozed_until_epoch = 0;
        next_alarm.snoozed_until_uptime_ms = 0;
        next_alarm.last_alarm_epoch = 0;
        next_alarm.last_fired_day_key = -1;

        if (i < previous_count && alarm_schedule_equal(&previous[i], &next_alarm)) {
            next_alarm.active = previous[i].enabled && previous[i].active;
            next_alarm.active_since_uptime_ms = previous[i].enabled ? previous[i].active_since_uptime_ms : 0;
            next_alarm.snoozed_until_epoch = previous[i].enabled ? previous[i].snoozed_until_epoch : 0;
            next_alarm.snoozed_until_uptime_ms = previous[i].enabled ? previous[i].snoozed_until_uptime_ms : 0;
            next_alarm.last_alarm_epoch = previous[i].last_alarm_epoch;
            next_alarm.last_fired_day_key = previous[i].last_fired_day_key;
        }

        if (!next_alarm.enabled) {
            next_alarm.active = false;
            next_alarm.active_since_uptime_ms = 0;
            next_alarm.snoozed_until_epoch = 0;
            next_alarm.snoozed_until_uptime_ms = 0;
        }

        s_state.alarms[i] = next_alarm;
    }

    s_state.comfort_min_temperature = next_comfort_min;
    s_state.comfort_max_temperature = next_comfort_max;
    s_state.last_settings_sync_epoch = time_is_valid(server_epoch) ? server_epoch : time(NULL);
    expire_comfort_alert_locked(now_ms);
    maybe_start_comfort_alert_locked(now_ms);
    refresh_alarm_summary_locked();
    resolve_audio_output_locked(time(NULL), now_ms, &should_stop_audio, &should_play_audio, &play_volume, &play_track);
    unlock_state();

    if (should_play_audio) {
        audio_player_play_alarm(play_volume, play_track);
    }
    if (should_stop_audio) {
        audio_player_stop();
    }

    ESP_LOGI(TAG, "Alarm settings updated: %d alarm(s)", next_count);
}

void alarm_manager_get_settings(alarm_settings_t *out_settings){ //used by the display screen code to draw the text interface for the alarms, it copies the current alarm settings
    if (out_settings == NULL) {
        return;
    }

    lock_state();
    out_settings->count = s_state.alarm_count;
    out_settings->comfort_min_temperature = s_state.comfort_min_temperature;
    out_settings->comfort_max_temperature = s_state.comfort_max_temperature;
    memcpy(out_settings->alarms, s_state.alarms, sizeof(out_settings->alarms));
    unlock_state();
}

void alarm_manager_get_status(alarm_status_t *out_status){
    if (out_status == NULL) {
        return;
    }

    lock_state();
    *out_status = s_state;
    out_status->uptime_ms = esp_timer_get_time() / 1000;
    out_status->time_valid = time_is_valid(time(NULL));
    unlock_state();
}

void alarm_manager_update_environment(float temperature_c, float humidity_percent){ //stores local physical measurements
    bool should_stop_audio = false;
    bool should_play_audio = false;
    uint8_t play_volume = 20;
    uint16_t play_track = 1;
    int64_t now_ms = esp_timer_get_time() / 1000;

    lock_state();
    s_state.temperature_c = temperature_c;
    s_state.humidity_percent = humidity_percent;
    s_state.environment_valid = true;
    expire_comfort_alert_locked(now_ms);
    maybe_start_comfort_alert_locked(now_ms);
    resolve_audio_output_locked(time(NULL), now_ms, &should_stop_audio, &should_play_audio, &play_volume, &play_track);
    unlock_state();

    if (should_play_audio) {
        audio_player_play_alarm(play_volume, play_track);
    } else if (should_stop_audio) {
        audio_player_stop();
    }
}

void alarm_manager_update_air_quality(bool valid, int air_quality_index, int eco2_ppm, int tvoc_ppb){ //stores local physical measurements
    bool should_stop_audio = false;
    bool should_play_audio = false;
    uint8_t play_volume = 20;
    uint16_t play_track = 1;
    int64_t now_ms = esp_timer_get_time() / 1000;

    lock_state();
    s_state.air_quality_valid = valid;
    s_state.air_quality_index = valid ? air_quality_index : 0;
    s_state.eco2_ppm = valid ? eco2_ppm : 0;
    s_state.tvoc_ppb = valid ? tvoc_ppb : 0;
    expire_comfort_alert_locked(now_ms);
    maybe_start_comfort_alert_locked(now_ms);
    resolve_audio_output_locked(time(NULL), now_ms, &should_stop_audio, &should_play_audio, &play_volume, &play_track);
    unlock_state();

    if (should_play_audio) {
        audio_player_play_alarm(play_volume, play_track);
    } else if (should_stop_audio) {
        audio_player_stop();
    }
}

void alarm_manager_mark_status_uploaded(void){ //keeps track of the timestamp indicating when data was last uploaded to the internet.
    lock_state();
    s_state.last_status_upload_epoch = time(NULL);
    unlock_state();
}

void alarm_manager_set_error(const char *message){ //stores the last error message, to be shown on the clock screen if needed
    lock_state();
    copy_text(s_state.last_error, sizeof(s_state.last_error), message);
    unlock_state();
}

void alarm_manager_snooze(void){ //computes when it has to ring again based on the snooze duration and current time
    bool should_stop_audio = false;
    bool should_play_audio = false;
    uint8_t play_volume = 20;
    uint16_t play_track = 1;
    time_t now = time(NULL);
    int64_t now_ms = esp_timer_get_time() / 1000;

    lock_state();
    if (s_state.ringing) {
        // We only snooze one alarm here, because other active alarms should still be able to ring.
        int alarm_index = oldest_active_alarm_index_locked();
        if (alarm_index >= 0) {
            alarm_entry_t *alarm = &s_state.alarms[alarm_index];
            int snooze_seconds = alarm->snooze_minutes * 60;
            if (snooze_seconds <= 0) {
                snooze_seconds = 300;
            }

            alarm->active = false;
            alarm->active_since_uptime_ms = 0;
            alarm->snoozed_until_epoch = time_is_valid(now) ? (int64_t)now + snooze_seconds : 0;
            alarm->snoozed_until_uptime_ms = now_ms + ((int64_t)snooze_seconds * 1000);
            ESP_LOGI(TAG, "Alarm '%s' snoozed for %d minutes", alarm->label, alarm->snooze_minutes);
        }

        refresh_alarm_summary_locked();
        expire_comfort_alert_locked(now_ms);
        resolve_audio_output_locked(now, now_ms, &should_stop_audio, &should_play_audio, &play_volume, &play_track);
    }
    unlock_state();

    if (should_play_audio) {
        audio_player_play_alarm(play_volume, play_track);
    } else if (should_stop_audio) {
        audio_player_stop();
    }
}

void alarm_manager_stop(void){ //stops the currently ringing item without cancelling unrelated snoozed alarms
    bool should_stop_audio = false;
    bool should_play_audio = false;
    uint8_t play_volume = 20;
    uint16_t play_track = 1;

    lock_state();

    int alarm_index = oldest_active_alarm_index_locked();
    if (alarm_index >= 0) {
        alarm_entry_t *alarm = &s_state.alarms[alarm_index];
        alarm->active = false;
        alarm->active_since_uptime_ms = 0;
        should_stop_audio = true;
        ESP_LOGI(TAG, "Alarm '%s' stopped", alarm->label);
    } else if (s_comfort_alert_active) {
        s_comfort_alert_active = false;
        s_comfort_alert_until_uptime_ms = 0;
        should_stop_audio = true;
    }

    refresh_alarm_summary_locked();
    resolve_audio_output_locked(time(NULL), esp_timer_get_time() / 1000, &should_stop_audio, &should_play_audio, &play_volume, &play_track);
    unlock_state();

    if (should_play_audio) {
        audio_player_play_alarm(play_volume, play_track);
    }
    if (should_stop_audio) {
        audio_player_stop();
    }
}

void alarm_manager_task(void *pvParameters){ //every second, it checks the time, starts alarms when they are due, auto-snoozes them after one minute of ringing, and keeps the short comfort alert in sync with the sensor readings.
    (void)pvParameters;

    while (1) {
        bool should_stop_audio = false;
        bool should_play_audio = false;
        uint8_t play_volume = 20;
        uint16_t play_track = 1;
        time_t now = time(NULL);
        int64_t now_ms = esp_timer_get_time() / 1000;

        lock_state();
        s_state.time_valid = time_is_valid(now);
        struct tm local_now = {0};
        int today_key = -1;
        if (s_state.time_valid) {
            localtime_r(&now, &local_now);
            today_key = alarm_day_key(&local_now);
        }

        for (int i = 0; i < s_state.alarm_count; i++) {
            alarm_entry_t *alarm = &s_state.alarms[i];
            if (!alarm->enabled) {
                continue;
            }

            if (alarm->active) {
                if (alarm->active_since_uptime_ms > 0 &&
                    now_ms >= alarm->active_since_uptime_ms + ALARM_RING_TIMEOUT_MS) {
                    int snooze_seconds = alarm->snooze_minutes * 60;
                    if (snooze_seconds <= 0) {
                        snooze_seconds = 300;
                    }

                    alarm->active = false;
                    alarm->active_since_uptime_ms = 0;
                    alarm->snoozed_until_epoch = time_is_valid(now) ? (int64_t)now + snooze_seconds : 0;
                    alarm->snoozed_until_uptime_ms = now_ms + ((int64_t)snooze_seconds * 1000);
                    ESP_LOGI(TAG, "Alarm '%s' timed out after 1 minute and was snoozed for %d minutes", alarm->label, alarm->snooze_minutes);
                }
                continue;
            }

            if (alarm->snoozed_until_uptime_ms > 0 && now_ms >= alarm->snoozed_until_uptime_ms) {
                alarm->active = true;
                alarm->active_since_uptime_ms = now_ms;
                alarm->snoozed_until_epoch = 0;
                alarm->snoozed_until_uptime_ms = 0;
                alarm->last_alarm_epoch = s_state.time_valid ? now : 0;
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
                alarm->active_since_uptime_ms = now_ms;
                alarm->last_alarm_epoch = now;
                alarm->last_fired_day_key = today_key;
                play_volume = (uint8_t)alarm->volume;
                play_track = (uint16_t)alarm->track;
                ESP_LOGI(TAG, "Alarm '%s' ringing with track %d", alarm->label, alarm->track);
            }
        }

        expire_comfort_alert_locked(now_ms);
        maybe_start_comfort_alert_locked(now_ms);
        refresh_alarm_summary_locked();
        resolve_audio_output_locked(now, now_ms, &should_stop_audio, &should_play_audio, &play_volume, &play_track);
        unlock_state();

        if (should_play_audio) {
            audio_player_play_alarm(play_volume, play_track);
        } else if (should_stop_audio) {
            audio_player_stop();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
