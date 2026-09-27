#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED
} audio_player_state_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Audio Player background service and FreeRTOS task
 */
esp_err_t audio_player_init(void);

/**
 * @brief Play an MP3 or WAV audio file asynchronously from SD Card or Flash
 * @param filepath Full POSIX path (e.g. "/sdcard/music/Aniket Prantor.mp3" or "/sdcard/sounds/confirm.mp3")
 */
esp_err_t audio_player_play_file(const char *filepath);

/**
 * @brief Pause currently playing audio stream
 */
void audio_player_pause(void);

/**
 * @brief Resume paused audio stream
 */
void audio_player_resume(void);

/**
 * @brief Toggle between Play and Pause
 */
void audio_player_toggle_play_pause(void);

/**
 * @brief Stop audio playback immediately
 */
void audio_player_stop(void);

/**
 * @brief Get current playback status
 */
audio_player_state_t audio_player_get_state(void);

/**
 * @brief Get filename of the currently loaded track
 */
const char* audio_player_get_current_track_name(void);

/**
 * @brief Set master playback volume (0 to 100%)
 */
void audio_player_set_volume(uint8_t volume);

/**
 * @brief Get current volume level
 */
uint8_t audio_player_get_volume(void);

/**
 * @brief Play quick UI button click sound effect
 */
void audio_player_play_ui_click(void);

/**
 * @brief Play joyful pet chirp / happy robot sound
 */
void audio_player_play_happy_sound(void);

/**
 * @brief Play continuous 1000Hz test tone for speaker testing
 */
void audio_player_play_test_tone(void);

#ifdef __cplusplus
}
#endif
