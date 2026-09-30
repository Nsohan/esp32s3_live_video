#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wake_word_callback_t)(const char *wake_word_name);

/**
 * @brief Initialize the Wake Word service and load models from the 'model' partition.
 */
esp_err_t wake_word_service_init(void);

/**
 * @brief Start the background Wake Word listening task (pinned to Core 1).
 */
esp_err_t wake_word_service_start(void);

/**
 * @brief Stop the background Wake Word task.
 */
void wake_word_service_stop(void);

/**
 * @brief Temporarily pause listening (e.g. while recorder is active).
 */
void wake_word_service_pause(void);

/**
 * @brief Resume listening.
 */
void wake_word_service_resume(void);

/**
 * @brief Check if wake word detection is running.
 */
bool wake_word_service_is_running(void);

/**
 * @brief Register custom callback when wake word is detected.
 */
void wake_word_service_set_callback(wake_word_callback_t cb);

#ifdef __cplusplus
}
#endif
