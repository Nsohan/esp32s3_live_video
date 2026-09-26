#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts the FreeRTOS background task that renders RoboEyes animations
 *        on the 320x240 ILI9341 display and cycles through all eye expressions,
 *        moods, animations, and gazes every 15 seconds.
 */
void roboeyes_start_cycling_task(void);

#ifdef __cplusplus
}
#endif
