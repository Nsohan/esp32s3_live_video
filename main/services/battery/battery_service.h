#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize battery monitor service
 */
void battery_service_init(void);

/**
 * @brief Get current battery charge percentage (0 - 100)
 */
uint8_t battery_service_get_percentage(void);

/**
 * @brief Check if battery is currently charging
 */
bool battery_service_is_charging(void);

/**
 * @brief Set mock or calculated battery level (for test/simulation)
 */
void battery_service_set_level(uint8_t percentage, bool is_charging);

#ifdef __cplusplus
}
#endif
