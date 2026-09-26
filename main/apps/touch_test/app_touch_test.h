#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render the 24-Block Interactive Touch Calibration & Diagnostics screen
 */
void app_touch_test_draw(void);

/**
 * @brief Reset calibration screen test state
 */
void app_touch_test_reset(void);

/**
 * @brief Handle touch events during calibration test
 */
bool app_touch_test_handle_touch(int tx, int ty, uint16_t rx, uint16_t ry, AppState *next_state);

#ifdef __cplusplus
}
#endif
