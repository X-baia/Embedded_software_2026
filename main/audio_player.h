#pragma once

#include <stdint.h>

void init_audio_player(void);
void audio_player_set_volume(uint8_t volume);
void audio_player_play_alarm(uint8_t volume);
void audio_player_stop(void);
