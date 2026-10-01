#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TTS_VOICE_ROBOT = 0,        // Classic retro robot (pitch: 64, speed: 72, mouth: 128, throat: 128)
    TTS_VOICE_PET_LITTLE,       // High-pitched cute pet (pitch: 84, speed: 82, mouth: 150, throat: 140)
    TTS_VOICE_DEEP_BOT,         // Deep sci-fi bot (pitch: 42, speed: 68, mouth: 110, throat: 105)
    TTS_VOICE_ELF,              // Fast squeaky elf (pitch: 92, speed: 95, mouth: 160, throat: 150)
    TTS_VOICE_LOONA_MASCOT,     // Loona / Mascot style: Curious, inquisitive head-tilt tone (pitch: 42, speed: 60, mouth: 195, throat: 168)
    TTS_VOICE_HUMAN_MALE,       // Normal human male: Natural adult male voice (pitch: 65, speed: 72, mouth: 128, throat: 128)
    TTS_VOICE_HUMAN_FEMALE,     // Normal human female: Higher pitch & lighter formants (pitch: 50, speed: 70, mouth: 152, throat: 145)
} tts_voice_preset_t;

typedef enum {
    TTS_MODE_OFFLINE_SAM = 0,   // Offline SAM retro robot / Loona synth
    TTS_MODE_CLOUD_NATURAL = 1  // Browser-assisted / Google Cloud natural voice
} tts_mode_t;

/**
 * @brief Initialize background TTS FreeRTOS worker queue & task
 */
esp_err_t tts_service_init(void);

/**
 * @brief Enqueue text to be synthesized using offline SAM synth
 * @param text Plain English text string (e.g. "Hello! I am PetBot.")
 */
esp_err_t tts_speak(const char *text);

/**
 * @brief Enqueue speech synthesis using either offline SAM or cloud natural speech
 * @param text Plain text string
 * @param mode TTS_MODE_OFFLINE_SAM or TTS_MODE_CLOUD_NATURAL
 * @param lang Language code for cloud speech (e.g. "en", "es", "fr", "ja", "de")
 */
esp_err_t tts_speak_mode(const char *text, tts_mode_t mode, const char *lang);

/**
 * @brief Directly play raw PCM audio chunk (e.g. from browser mic walkie-talkie)
 * @param pcm_data Pointer to 16-bit signed mono PCM samples
 * @param len Byte length
 * @param sample_rate Sample rate (e.g. 16000)
 */
esp_err_t tts_play_pcm(const void *pcm_data, size_t len, uint32_t sample_rate);

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
