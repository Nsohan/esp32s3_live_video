#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Core Framework & App Drawer
#include "app_launcher.h"

// Drivers
#include "display.h"
#include "camera_driver.h"

// Apps
#include "roboeyes_display.h"

// Services
#include "wifi_service.h"

static const char *TAG = "petbot_main";

// ─── Put Your WiFi Credentials Here ───────────────────────
#define WIFI_SSID     "Anonymous"
#define WIFI_PASSWORD "soh@nsoh@n"
// ──────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "        PETBOT EMBEDDED SYSTEM          ");
    ESP_LOGI(TAG, "========================================");

    // 1. Initialize 2.4" TFT ILI9341 Display
    ESP_LOGI(TAG, "Initializing 2.4\" TFT Display (ILI9341)...");
    if (display_init() == ESP_OK) {
        display_fill_screen(COLOR_BLACK);
    } else {
        ESP_LOGE(TAG, "Display initialization failed!");
    }

    // 2. Start RoboEyes Background Engine Task (Core 1)
    roboeyes_start_cycling_task();

    // 3. Start Interactive Smartphone App Launcher & Touch Navigation (Core 0)
    app_launcher_init();
    app_launcher_set_wifi_info(WIFI_SSID, "Connecting...");
    app_launcher_start_task();

    // 4. Initialize OV2640 Camera Driver
    ESP_ERROR_CHECK(camera_driver_init());

    // 5. Connect WiFi in Background (Non-blocking) & start HTTP MJPEG server
    wifi_service_init(WIFI_SSID, WIFI_PASSWORD);

    ESP_LOGI(TAG, "PetBot App Launcher UI started instantly!");
}