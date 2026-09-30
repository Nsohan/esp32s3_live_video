#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROBOEYES_MODE_DEFAULT_IDLE = 0,
    ROBOEYES_MODE_HAPPY = 1,
    ROBOEYES_MODE_LAUGHING = 2,
    ROBOEYES_MODE_ANGRY = 3,
    ROBOEYES_MODE_TIRED = 4,
    ROBOEYES_MODE_CONFUSED = 5,
    ROBOEYES_MODE_SWEATING = 6,
    ROBOEYES_MODE_CURIOUS = 7,
    ROBOEYES_MODE_CYCLOPS = 8,
    ROBOEYES_MODE_WINKING = 9,
    ROBOEYES_MODE_MUSIC_LISTENING = 10,
    ROBOEYES_MODE_MAX_COUNT
} roboeyes_mode_t;

/**
 * @brief Starts the FreeRTOS background task that renders RoboEyes animations
 *        on the 320x240 ILI9341 display and cycles through eye expressions,
 *        moods, animations, and gazes.
 */
void roboeyes_start_cycling_task(void);

/**
 * @brief Enable or pause RoboEyes rendering to display
 */
void roboeyes_set_active(bool active);

/**
 * @brief Check if RoboEyes rendering is currently active
 */
bool roboeyes_is_active(void);

/**
 * @brief Trigger a specific mood immediately
 */
void roboeyes_trigger_mood(int mood_index);

/**
 * @brief Trigger music listening & grooving eye animation mode immediately
 */
void roboeyes_trigger_music_mode(void);

#ifdef __cplusplus
}
#endif

