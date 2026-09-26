#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start HTTP MJPEG camera streaming and remote control server on port 80
 */
void start_camera_server(void);

#ifdef __cplusplus
}
#endif
