#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start HTTP MJPEG camera streaming and remote control server on port 80
 */
void start_camera_server(void);

/**
 * @brief Check if a web client is currently streaming video
 */
bool http_stream_is_active(void);

#ifdef __cplusplus
}
#endif
