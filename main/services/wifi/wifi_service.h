#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi station and connect in background
 * @param ssid WiFi SSID
 * @param password WiFi Password
 */
void wifi_service_init(const char *ssid, const char *password);

/**
 * @brief Get the current assigned IP address string
 * @return String pointer (e.g. "192.168.1.50" or "Connecting...")
 */
const char* wifi_service_get_ip_string(void);

// Compatibility aliases
#define wifi_init(ssid, pwd) wifi_service_init(ssid, pwd)
#define wifi_get_ip_string() wifi_service_get_ip_string()

#ifdef __cplusplus
}
#endif
