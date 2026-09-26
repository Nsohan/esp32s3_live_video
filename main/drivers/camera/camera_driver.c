#include "camera_driver.h"
#include "esp_log.h"

static const char *TAG = "camera_driver";

esp_err_t camera_driver_init(void)
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
        .frame_size = FRAMESIZE_QVGA, // 320x240
        .jpeg_quality = 12,           // Clean, sharp image with fast transmission
        .fb_count = 2,                // Double buffering
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST, // Always grab freshest frame (eliminates lag!)
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED: 0x%x", err);
        return err;
    }

    // Apply auto-tuning for vibrant, bright image in any room lighting
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_framesize(s, FRAMESIZE_QVGA);
        s->set_quality(s, 12);
        s->set_brightness(s, 0);       // -2 to 2 (0 = normal)
        s->set_contrast(s, 0);         // -2 to 2 (0 = normal)
        s->set_saturation(s, 0);       // -2 to 2 (0 = normal)
        s->set_special_effect(s, 0);   // No effect
        s->set_whitebal(s, 1);         // AWB ON
        s->set_awb_gain(s, 1);         // AWB Gain ON
        s->set_wb_mode(s, 0);          // Auto WB mode
        s->set_exposure_ctrl(s, 1);    // Auto Exposure ON (AEC)
        s->set_aec2(s, 0);             // Normal AEC DSP
        s->set_ae_level(s, 0);         // Auto exposure target level
        s->set_gain_ctrl(s, 1);        // Auto Gain Control ON (AGC)
        s->set_gainceiling(s, GAINCEILING_8X); // Allow gain to boost in darker environments
        s->set_bpc(s, 1);              // Black pixel correction ON
        s->set_wpc(s, 1);              // White pixel correction ON
        s->set_raw_gma(s, 1);          // Gamma correction ON
        s->set_lenc(s, 1);             // Lens correction ON
        s->set_dcw(s, 1);              // Downsize correction ON
        s->set_colorbar(s, 0);         // Test colorbar OFF
    }

    ESP_LOGI(TAG, "Camera driver initialized: QVGA (320x240) with Auto-Exposure & Auto-Gain!");
    return ESP_OK;
}
