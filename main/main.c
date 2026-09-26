#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "wifi.h"
#include "http_stream.h"
#include "display.h"
#include "roboeyes_display.h"
#include "app_launcher.h"

static const char *TAG = "petbot_main";

// ─── PUT YOUR CREDENTIALS HERE ───────────────────────────
#define WIFI_SSID "Anonymous"
#define WIFI_PASSWORD "soh@nsoh@n"
// ─────────────────────────────────────────────────────────

// Pin mapping for OV2640 camera
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

static esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_SVGA, // SVGA (800x600)
        .jpeg_quality = 10,           // Better quality
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera init FAILED: 0x%x", err);
        return err;
    }

    // Apply OV2640-specific settings
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_SVGA);
    s->set_quality(s, 10);
    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, 1);
    s->set_special_effect(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_ae_level(s, 0);
    s->set_aec_value(s, 300);
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, GAINCEILING_2X);
    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_dcw(s, 1);
    s->set_colorbar(s, 0);

    ESP_LOGI(TAG, "Camera init SUCCESS!");
    return ESP_OK;
}

void app_main(void)
{
    // 1. Init Display immediately
    ESP_LOGI(TAG, "Initializing 2.4\" TFT Display (ILI9341)...");
    if (display_init() == ESP_OK) {
        display_fill_screen(COLOR_BLACK);
    } else {
        ESP_LOGE(TAG, "Display initialization failed!");
    }

    // 2. Start RoboEyes Background Engine
    roboeyes_start_cycling_task();

    // 3. Start Interactive Smartphone App Launcher & Touch Navigation
    app_launcher_init();
    app_launcher_set_wifi_info(WIFI_SSID, "Connecting...");
    app_launcher_start_task();

    // 4. Init Camera
    ESP_ERROR_CHECK(camera_init());

    // 5. Connect WiFi in Background (Non-blocking)
    wifi_init(WIFI_SSID, WIFI_PASSWORD);

    ESP_LOGI(TAG, "PetBot App Launcher UI started instantly!");
}