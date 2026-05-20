#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define ALARM_MAX_COUNT 8
#define ALARM_LABEL_SIZE 24
#define ALARM_DAYS_EVERYDAY 0x7f

typedef struct {
    bool enabled;
    int hour;
    int minute;
    int snooze_minutes;
    int volume;
    int track;
    uint8_t days_mask;
    char label[ALARM_LABEL_SIZE];
    bool active;
    int64_t snoozed_until_epoch;
    int64_t snoozed_until_uptime_ms;
    int64_t last_alarm_epoch;
    int last_fired_day_key;
} alarm_entry_t;

typedef struct {
    int count;
    alarm_entry_t alarms[ALARM_MAX_COUNT];
} alarm_settings_t;

typedef struct {
    char device_id[32];
    bool alarm_enabled;
    int alarm_hour;
    int alarm_minute;
    int snooze_minutes;
    int volume;
    int track;
    bool ringing;
    int alarm_count;
    int active_alarm_count;
    alarm_entry_t alarms[ALARM_MAX_COUNT];
    int64_t snoozed_until_epoch;
    int64_t last_alarm_epoch;
    int64_t last_settings_sync_epoch;
    int64_t last_status_upload_epoch;
    int64_t uptime_ms;
    bool time_valid;
    bool environment_valid;
    float temperature_c;
    float humidity_percent;
    bool air_quality_valid;
    int air_quality_index;
    int eco2_ppm;
    int tvoc_ppb;
    char last_error[128];
} alarm_status_t;

void alarm_manager_init(void);
void alarm_manager_task(void *pvParameters);

void alarm_manager_apply_settings(const alarm_settings_t *settings, time_t server_epoch);
void alarm_manager_get_settings(alarm_settings_t *out_settings);
void alarm_manager_get_status(alarm_status_t *out_status);

void alarm_manager_update_environment(float temperature_c, float humidity_percent);
void alarm_manager_update_air_quality(bool valid, int air_quality_index, int eco2_ppm, int tvoc_ppb);
void alarm_manager_mark_status_uploaded(void);
void alarm_manager_set_error(const char *message);

void alarm_manager_snooze(void);
void alarm_manager_stop(void);
