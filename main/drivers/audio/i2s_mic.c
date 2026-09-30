#include "i2s_mic.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2s_mic";

// INMP441 gives 24-bit data left-justified in a 32-bit slot.
// >>16 = exact 16-bit; >>14 = 4x digital gain (clamped) so quiet voices are visible.
#define MIC_SHIFT 14

static i2s_chan_handle_t s_rx_chan = NULL;

esp_err_t i2s_mic_init(void)
{
    if (s_rx_chan) return ESP_OK;

    ESP_LOGI(TAG, "Init INMP441: SCK=%d WS=%d SD=%d @ %d Hz",
             I2S_MIC_PIN_SCK, I2S_MIC_PIN_WS, I2S_MIC_PIN_SD, I2S_MIC_SAMPLE_RATE);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 256;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   // L/R pin tied to GND = left

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_MIC_SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_MIC_PIN_SCK,
            .ws   = (gpio_num_t)I2S_MIC_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_MIC_PIN_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "channel_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "INMP441 ready");
    return ESP_OK;
}

esp_err_t i2s_mic_read(int16_t *dst, size_t samples, size_t *samples_read, uint32_t timeout_ms)
{
    if (!s_rx_chan) return ESP_ERR_INVALID_STATE;

    int32_t raw[256];
    size_t done = 0;

    while (done < samples) {
        size_t want = samples - done;
        if (want > 256) want = 256;

        size_t bytes = 0;
        esp_err_t ret = i2s_channel_read(s_rx_chan, raw, want * sizeof(int32_t),
                                         &bytes, timeout_ms);
        size_t got = bytes / sizeof(int32_t);
        for (size_t i = 0; i < got; i++) {
            int32_t v = raw[i] >> MIC_SHIFT;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            dst[done + i] = (int16_t)v;
        }
        done += got;
        if (ret != ESP_OK) {
            if (samples_read) *samples_read = done;
            return ret;
        }
    }

    if (samples_read) *samples_read = done;
    return ESP_OK;
}

static TaskHandle_t s_level_task_handle = NULL;
static volatile bool s_level_monitor_running = false;
static volatile int s_latest_rms = 0;

int i2s_mic_get_latest_rms(void)
{
    return s_latest_rms;
}

bool i2s_mic_is_level_monitor_running(void)
{
    return s_level_monitor_running;
}

static void level_task(void *arg)
{
    int16_t buf[512];
    int tick = 0;

    ESP_LOGI(TAG, "Mic Level Monitor started");

    while (s_level_monitor_running) {
        size_t n = 0;
        if (i2s_mic_read(buf, 512, &n, 200) != ESP_OK || n == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += (double)buf[i] * buf[i];
        int rms = (int)sqrt(sum / n);
        s_latest_rms = rms;

        // 512 samples @16k = 32 ms; log about 4x per second
        if (++tick >= 8) {
            tick = 0;
            int bars = rms / 400;
            if (bars > 30) bars = 30;
            char meter[32];
            memset(meter, '#', bars);
            meter[bars] = 0;
            ESP_LOGI(TAG, "RMS %5d |%s", rms, meter);
        }
    }

    ESP_LOGI(TAG, "Mic Level Monitor stopped");
    s_level_task_handle = NULL;
    vTaskDelete(NULL);
}

void i2s_mic_start_level_monitor(void)
{
    if (s_level_monitor_running) return;
    s_level_monitor_running = true;
    xTaskCreate(level_task, "mic_level", 4096, NULL, 4, &s_level_task_handle);
}

void i2s_mic_stop_level_monitor(void)
{
    if (!s_level_monitor_running) return;
    s_level_monitor_running = false;
    for (int i = 0; i < 30 && s_level_task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void i2s_mic_deinit(void)
{
    i2s_mic_stop_level_monitor();
    if (!s_rx_chan) return;
    i2s_channel_disable(s_rx_chan);
    i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;
}