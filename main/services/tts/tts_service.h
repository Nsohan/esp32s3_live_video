#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TTS_VOICE_ROBOT = 0,    // Classic retro robot (pitch: 64, speed: 72, mouth: 128, throat: 128)
    TTS_VOICE_PET_LITTLE,   // High-pitched cute pet (pitch: 84, speed: 82, mouth: 150, throat: 140)
    TTS_VOICE_DEEP_BOT,     // Deep sci-fi bot (pitch: 42, speed: 68, mouth: 110, throat: 105)
    TTS_VOICE_ELF,          // Fast squeaky elf (pitch: 92, speed: 95, mouth: 160, throat: 150)
} tts_voice_preset_t;

/**
 * @brief Initialize background TTS FreeRTOS worker queue & task
 */
esp_err_t tts_service_init(void);

/**
 * @brief Enqueue text to be synthesized and spoken through MAX98357A I2S DAC
 * @param text Plain English text string (e.g. "Hello! I am PetBot.")
 */
esp_err_t tts_speak(const char *text);

/**
 * @brief Check if TTS is currently synthesizing or speaking
 */
bool tts_is_busy(void);

/**
 * @brief Set voice parameters preset
 */
void tts_set_preset(tts_voice_preset_t preset);

/**
 * @brief Custom voice tweaking
 */
void tts_set_voice(uint8_t pitch, uint8_t speed, uint8_t mouth, uint8_t throat);

#ifdef __cplusplus
}
#endif
