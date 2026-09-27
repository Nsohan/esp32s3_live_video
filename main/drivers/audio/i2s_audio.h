#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// ─── MAX98357A I2S DAC Pin Configuration ──────────────────
#define I2S_AUDIO_PIN_BCLK   47  // Bit Clock (Clean high-speed GPIO, replaces GPIO 0 boot strapping pin)
#define I2S_AUDIO_PIN_WS     48  // Word Select / Left-Right Clock (LRC)
#define I2S_AUDIO_PIN_DOUT   21  // Data In on MAX98357A (ESP32 Data Out)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize I2S Audio transmitter channel for MAX98357A DAC
 * @param sample_rate Audio sample rate in Hz (e.g. 44100, 22050, 16000)
 * @param bits_per_sample Bits per sample (16 or 32)
 * @param channels Number of audio channels (1 = Mono, 2 = Stereo)
 */
esp_err_t i2s_audio_init(uint32_t sample_rate, uint16_t bits_per_sample, uint8_t channels);

/**
 * @brief Dynamically update I2S sample rate and channels to match track format
 */
esp_err_t i2s_audio_set_params(uint32_t sample_rate, uint16_t bits_per_sample, uint8_t channels);

/**
 * @brief Write raw PCM audio data chunk to I2S DMA buffer
 */
esp_err_t i2s_audio_write(const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms);

/**
 * @brief Set Master Audio Output Volume (0 to 100%)
 */
void i2s_audio_set_volume(uint8_t volume_percent);

/**
 * @brief Get Current Audio Volume (0 to 100%)
 */
uint8_t i2s_audio_get_volume(void);

/**
 * @brief Play a synthetic test tone/sine beep (useful for UI clicks & alert beeps)
 */
void i2s_audio_play_beep(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief Deinitialize I2S Audio driver
 */
void i2s_audio_deinit(void);

#ifdef __cplusplus
}
#endif
