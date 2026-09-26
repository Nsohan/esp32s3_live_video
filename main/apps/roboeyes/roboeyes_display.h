#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief Set eye primary color (RGB565)
 */
void roboeyes_set_color(uint16_t color_rgb565);

/**
 * @brief Trigger a specific mood immediately
 */
void roboeyes_trigger_mood(int mood_index);

#ifdef __cplusplus
}
#endif
