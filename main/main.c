#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "wifi.h"
#include "http_stream.h"

static const char *TAG = "camera_test";

// ─── PUT YOUR CREDENTIALS HERE ───────────────────────────
#define WIFI_SSID "ANONYMOUS"
#define WIFI_PASSWORD "soh@nsoh@n"
// ─────────────────────────────────────────────────────────

// Your original working pin mapping
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
        .frame_size = FRAMESIZE_SVGA, // Changed from VGA to SVGA (800x600)
        .jpeg_quality = 10,           // Better quality (lower number = better)
        .fb_count = 1,                // Changed from 2 to 1 for stability
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera init FAILED: 0x%x", err);
        return err;
    }

    // Apply OV2640-specific settings for better JPEG output
    sensor_t *s = esp_camera_sensor_get();

    // OV2640 specific settings
    s->set_framesize(s, FRAMESIZE_SVGA);
    s->set_quality(s, 10);
    s->set_brightness(s, 1);     // +1 brightness
    s->set_contrast(s, 1);       // +1 contrast
    s->set_saturation(s, 1);     // +1 saturation
    s->set_special_effect(s, 0); // No effect
    s->set_whitebal(s, 1);       // Enable white balance
    s->set_awb_gain(s, 1);       // Auto white balance gain
    s->set_wb_mode(s, 0);        // Auto WB mode
    s->set_exposure_ctrl(s, 1);  // Auto exposure
    s->set_aec2(s, 1);           // AEC2 enable
    s->set_ae_level(s, 0);       // Auto exposure level
    s->set_aec_value(s, 300);    // Exposure value
    s->set_gain_ctrl(s, 1);      // Auto gain
    s->set_agc_gain(s, 0);       // AGC gain
    s->set_gainceiling(s, GAINCEILING_2X);
    s->set_bpc(s, 0);      // Bad pixel correction off
    s->set_wpc(s, 1);      // White pixel correction on
    s->set_raw_gma(s, 1);  // Raw gamma
    s->set_lenc(s, 1);     // Lens correction
    s->set_dcw(s, 1);      // DCW enable
    s->set_colorbar(s, 0); // Colorbar off

    ESP_LOGI(TAG, "Camera init SUCCESS!");
    return ESP_OK;
}

void app_main(void)
{
    // 1. Init camera
    ESP_ERROR_CHECK(camera_init());

    // 2. Let camera stabilize
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 3. Test a few frames first
    ESP_LOGI(TAG, "Testing frame capture...");
    int valid_frames = 0;
    for (int i = 0; i < 5; i++)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb)
        {
            if (fb->len > 0 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8)
            {
                ESP_LOGI(TAG, "✓ Frame %d: Valid JPEG! Size: %d bytes", i + 1, fb->len);
                valid_frames++;
            }
            else
            {
                ESP_LOGW(TAG, "✗ Frame %d: Invalid JPEG", i + 1);
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (valid_frames == 0)
    {
        ESP_LOGE(TAG, "No valid frames captured! Check camera configuration");
        return;
    }

    // 4. Connect WiFi
    wifi_init(WIFI_SSID, WIFI_PASSWORD);

    // 5. Wait for WiFi to stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 6. Start HTTP stream server
    start_camera_server();

    ESP_LOGI(TAG, "Stream ready! Open browser at http://[IP shown above]");
}