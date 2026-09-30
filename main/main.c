#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Core Framework & App Drawer
#include "app_launcher.h"

// Drivers
#include "display.h"
#include "camera_driver.h"
#include "sdcard.h"
#include "i2s_audio.h"

// Apps
#include "roboeyes_display.h"

// Services & Audio
#include "audio_player.h"
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

    // 2. Mount MicroSD Card (Display SPI Slot, CS=14)
    ESP_LOGI(TAG, "Initializing MicroSD Card on Display SPI bus...");
    sdcard_init();

    // 3. Initialize I2S Audio Player Service (MAX98357A on GPIO 0, 48, 21)
    ESP_LOGI(TAG, "Initializing Audio Player Service...");
    audio_player_init();
    audio_player_play_happy_sound(); // Play welcome startup chime

    // 4. Start Interactive Smartphone App Launcher & Touch Navigation (Core 0)
    app_launcher_init();
    app_launcher_set_wifi_info(WIFI_SSID, "Connecting...");
    app_launcher_start_task();

    // 5. Connect WiFi in Background (Non-blocking) & start HTTP MJPEG server
    wifi_service_init(WIFI_SSID, WIFI_PASSWORD);

    ESP_LOGI(TAG, "PetBot App Launcher UI & Audio System started instantly!");
}