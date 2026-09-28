#include "i2s_audio.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2s_audio";

static i2s_chan_handle_t s_tx_chan = NULL;
static uint8_t s_volume = 80; // Default volume: 80%
static uint32_t s_current_rate = 44100;
static uint16_t s_current_bits = 16;
static uint8_t s_current_channels = 2;
static bool s_is_enabled = false;

esp_err_t i2s_audio_init(uint32_t sample_rate, uint16_t bits_per_sample, uint8_t channels)
{
    if (s_tx_chan != NULL) {
        ESP_LOGW(TAG, "I2S Audio already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing MAX98357A I2S Audio Driver...");
    ESP_LOGI(TAG, "Pins: BCLK=%d, WS/LRC=%d, DOUT=%d",
             I2S_AUDIO_PIN_BCLK, I2S_AUDIO_PIN_WS, I2S_AUDIO_PIN_DOUT);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256; // 256 frames * 2 ch * 2 bytes = 1024 bytes per DMA buffer (safe & aligned)
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    s_current_rate = sample_rate;
    s_current_bits = bits_per_sample;
    s_current_channels = channels;

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO);
    slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_AUDIO_PIN_BCLK,
            .ws   = (gpio_num_t)I2S_AUDIO_PIN_WS,
            .dout = (gpio_num_t)I2S_AUDIO_PIN_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init standard I2S mode: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    s_is_enabled = true;
    ESP_LOGI(TAG, "MAX98357A I2S Audio ready (%lu Hz, 16-bit, Stereo, Volume: %d%%)",
             (unsigned long)sample_rate, s_volume);

    return ESP_OK;
}

esp_err_t i2s_audio_set_params(uint32_t sample_rate, uint16_t bits_per_sample, uint8_t channels)
{
    if (!s_tx_chan) {
        return i2s_audio_init(sample_rate, bits_per_sample, channels);
    }

    if (s_current_rate == sample_rate && s_current_bits == bits_per_sample && s_current_channels == channels) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Reconfiguring I2S: Rate=%lu Hz, Bits=%d, Ch=%d",
             (unsigned long)sample_rate, bits_per_sample, channels);

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO);
    slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);

    i2s_channel_disable(s_tx_chan);
    // CRITICAL: In ESP-IDF, reconfigure slot BEFORE clock so clock divider uses correct slot parameters
    esp_err_t err_s = i2s_channel_reconfig_std_slot(s_tx_chan, &slot_cfg);
    if (err_s != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reconfig I2S slot: %s", esp_err_to_name(err_s));
    }
    esp_err_t err_c = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
    if (err_c != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reconfig I2S clock: %s", esp_err_to_name(err_c));
    }
    esp_err_t err_e = i2s_channel_enable(s_tx_chan);
    if (err_e != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-enable I2S channel: %s", esp_err_to_name(err_e));
    }

    s_current_rate = sample_rate;
    s_current_bits = bits_per_sample;
    s_current_channels = channels;
    return ESP_OK;
}

esp_err_t i2s_audio_write(const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;

    if (s_volume == 0) {
        // Muted: send silence
        static const int16_t s_silence[256] = {0};
        size_t total_written = 0;
        while (total_written < size) {
            size_t chunk = (size - total_written) < sizeof(s_silence) ? (size - total_written) : sizeof(s_silence);
            size_t written = 0;
            esp_err_t err = i2s_channel_write(s_tx_chan, s_silence, chunk, &written, pdMS_TO_TICKS(timeout_ms));
            total_written += written;
            if (err != ESP_OK || written == 0) break;
        }
        if (bytes_written) *bytes_written = total_written;
        return ESP_OK;
    }

    const int16_t *samples = (const int16_t *)src;
    size_t total_samples = size / sizeof(int16_t);
    size_t processed = 0;
    size_t total_bytes_out = 0;

    // Buffer to stage scaled output samples (256 stereo pairs = 512 int16 = 1024 bytes)
    int16_t out_buf[512];

    if (s_current_channels == 1) {
        // Mono source: scale and duplicate to Left and Right
        while (processed < total_samples) {
            size_t chunk_mono = (total_samples - processed) < 256 ? (total_samples - processed) : 256;
            for (size_t i = 0; i < chunk_mono; i++) {
                int32_t scaled = ((int32_t)samples[processed + i] * s_volume) / 100;
                if (scaled > 32767) scaled = 32767;
                if (scaled < -32768) scaled = -32768;
                out_buf[i * 2]     = (int16_t)scaled; // Left
                out_buf[i * 2 + 1] = (int16_t)scaled; // Right
            }

            size_t bytes_to_write = chunk_mono * 2 * sizeof(int16_t);
            size_t written = 0;
            esp_err_t err = i2s_channel_write(s_tx_chan, out_buf, bytes_to_write, &written,
                                              timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
            total_bytes_out += (written / 2);
            if (err != ESP_OK) {
                if (bytes_written) *bytes_written = total_bytes_out;
                return err;
            }
            size_t samples_sent = written / (2 * sizeof(int16_t));
            processed += samples_sent;
            if (samples_sent == 0) break;
        }
    } else {
        // Stereo source: downmix (Left + Right) / 2 and replicate to both channels
        // Guarantees balanced mono output on MAX98357A regardless of SD_MODE pin wiring
        while (processed < total_samples) {
            size_t chunk_pairs = (total_samples - processed) / 2;
            if (chunk_pairs > 256) chunk_pairs = 256;
            if (chunk_pairs == 0) break;

            for (size_t i = 0; i < chunk_pairs; i++) {
                int32_t l = samples[processed + i * 2];
                int32_t r = samples[processed + i * 2 + 1];
                int32_t mono = (l + r) / 2;
                int32_t scaled = (mono * s_volume) / 100;
                if (scaled > 32767) scaled = 32767;
                if (scaled < -32768) scaled = -32768;
                out_buf[i * 2]     = (int16_t)scaled; // Left
                out_buf[i * 2 + 1] = (int16_t)scaled; // Right
            }

            size_t bytes_to_write = chunk_pairs * 2 * sizeof(int16_t);
            size_t written = 0;
            esp_err_t err = i2s_channel_write(s_tx_chan, out_buf, bytes_to_write, &written,
                                              timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
            total_bytes_out += written;
            if (err != ESP_OK) {
                if (bytes_written) *bytes_written = total_bytes_out;
                return err;
            }
            size_t pairs_sent = written / (2 * sizeof(int16_t));
            processed += (pairs_sent * 2);
            if (pairs_sent == 0) break;
        }
    }

    if (bytes_written) *bytes_written = total_bytes_out;
    return ESP_OK;
}

void i2s_audio_set_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) volume_percent = 100;
    s_volume = volume_percent;
    ESP_LOGI(TAG, "Audio Volume set to %d%%", s_volume);
}

uint8_t i2s_audio_get_volume(void)
{
    return s_volume;
}

void i2s_audio_play_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_tx_chan) {
        i2s_audio_init(44100, 16, 2);
    } else {
        i2s_audio_set_params(44100, 16, 2);
    }

    const uint32_t sample_rate = 44100;
    uint32_t total_samples = (sample_rate * duration_ms) / 1000;
    int16_t buf[256 * 2]; // Stereo buffer (Left + Right)

    uint32_t generated = 0;
    double phase_inc = 2.0 * M_PI * freq_hz / sample_rate;
    double phase = 0.0;

    while (generated < total_samples) {
        size_t samples_to_gen = (total_samples - generated) < 256 ? (total_samples - generated) : 256;
        for (size_t i = 0; i < samples_to_gen; i++) {
            // Apply a smooth envelope to avoid popping
            double amp = 1.0;
            if (generated + i < 150) amp = (double)(generated + i) / 150.0;
            else if (total_samples - (generated + i) < 150) amp = (double)(total_samples - (generated + i)) / 150.0;

            int16_t sample = (int16_t)(sin(phase) * 28000.0 * amp);
            buf[i * 2]     = sample; // Left
            buf[i * 2 + 1] = sample; // Right
            phase += phase_inc;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }

        size_t written = 0;
        i2s_audio_write(buf, samples_to_gen * 2 * sizeof(int16_t), &written, 150);
        generated += samples_to_gen;
    }
}

void i2s_audio_deinit(void)
{
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        s_is_enabled = false;
        ESP_LOGI(TAG, "I2S Audio driver deinitialized");
    }
}
