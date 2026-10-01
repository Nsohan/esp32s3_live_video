#include "tts_service.h"
#include "sam/sam.h"
#include "sam/reciter.h"
#include "sam/debug.h"
#include "i2s_audio.h"
#include "audio_player.h"
#include "minimp3.h"
#include "esp_http_client.h"
#include "wake_word_service.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "tts_service";

typedef struct {
    char text[256];
    char lang[16];
    tts_mode_t mode;
} tts_msg_t;

static QueueHandle_t s_tts_queue = NULL;
static volatile bool s_tts_busy = false;

// Default voice parameters: Loona / Mascot style (curious, inquisitive mascot tone)
static uint8_t s_pitch = 42;
static uint8_t s_speed = 60;
static uint8_t s_mouth = 195;
static uint8_t s_throat = 168;

void tts_set_voice(uint8_t pitch, uint8_t speed, uint8_t mouth, uint8_t throat)
{
    s_pitch = pitch;
    s_speed = speed;
    s_mouth = mouth;
    s_throat = throat;
}

void tts_set_preset(tts_voice_preset_t preset)
{
    switch (preset) {
        case TTS_VOICE_LOONA_MASCOT:
            // Loona / Mascot: Inquisitive, cute creature tone with raised formants & high pitch ("N? Are?" curious tone)
            tts_set_voice(42, 60, 195, 168);
            break;
        case TTS_VOICE_HUMAN_MALE:
            // Normal human male voice: balanced pitch and standard vocal tract
            tts_set_voice(65, 72, 128, 128);
            break;
        case TTS_VOICE_HUMAN_FEMALE:
            // Normal human female voice: higher pitch and elevated formants
            tts_set_voice(50, 70, 152, 145);
            break;
        case TTS_VOICE_PET_LITTLE:
            tts_set_voice(84, 82, 150, 140);
            break;
        case TTS_VOICE_DEEP_BOT:
            tts_set_voice(42, 68, 110, 105);
            break;
        case TTS_VOICE_ELF:
            tts_set_voice(92, 95, 160, 150);
            break;
        case TTS_VOICE_ROBOT:
        default:
            tts_set_voice(64, 72, 128, 128);
            break;
    }
}

bool tts_is_busy(void)
{
    return s_tts_busy;
}

esp_err_t tts_play_pcm(const void *pcm_data, size_t len, uint32_t sample_rate)
{
    if (!pcm_data || len == 0) return ESP_ERR_INVALID_ARG;
    s_tts_busy = true;
    i2s_audio_set_params(sample_rate, 16, 1);
    size_t written = 0;
    i2s_audio_write(pcm_data, len, &written, 500);
    s_tts_busy = false;
    return ESP_OK;
}

esp_err_t tts_speak_mode(const char *text, tts_mode_t mode, const char *lang)
{
    if (!text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;
    if (!s_tts_queue) {
        ESP_LOGE(TAG, "TTS service not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    tts_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    size_t tlen = strlen(text);
    if (tlen >= sizeof(msg.text)) tlen = sizeof(msg.text) - 1;
    memcpy(msg.text, text, tlen);
    msg.text[tlen] = '\0';

    msg.mode = mode;
    if (lang && strlen(lang) > 0) {
        strncpy(msg.lang, lang, sizeof(msg.lang) - 1);
    } else {
        strcpy(msg.lang, "en");
    }

    if (xQueueSend(s_tts_queue, &msg, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "TTS queue full, dropping message");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t tts_speak(const char *text)
{
    return tts_speak_mode(text, TTS_MODE_OFFLINE_SAM, NULL);
}

static void tts_synthesize_sam(const char *text)
{
    ESP_LOGI(TAG, "Synthesizing SAM offline: \"%s\"", text);

    char input[256];
    memset(input, 0, sizeof(input));
    size_t mlen = strlen(text);
    if (mlen >= sizeof(input) - 2) mlen = sizeof(input) - 2;
    memcpy(input, text, mlen);
    input[mlen] = '\0';

    for (int i = 0; input[i] != 0; i++) {
        input[i] = toupper((unsigned char)input[i]);
    }

    strncat(input, "[", sizeof(input) - strlen(input) - 1);

    int phoneme_res = TextToPhonemes((unsigned char *)input);
    if (!phoneme_res) {
        ESP_LOGE(TAG, "TextToPhonemes failed for: %s", text);
        return;
    }

    SetSpeed(s_speed);
    SetPitch(s_pitch);
    SetMouth(s_mouth);
    SetThroat(s_throat);
    SetInput(input);

    if (!SAMMain()) {
        ESP_LOGE(TAG, "SAMMain failed");
        FreeBuffer();
        return;
    }

    char *buf = GetBuffer();
    int sample_count = GetBufferLength() / 50;

    if (buf && sample_count > 0) {
        ESP_LOGI(TAG, "SAM generated %d samples at 22050 Hz", sample_count);
        i2s_audio_set_params(22050, 16, 1);

        int16_t chunk[512];
        int offset = 0;
        while (offset < sample_count) {
            int chunk_len = (sample_count - offset < 512) ? (sample_count - offset) : 512;
            for (int i = 0; i < chunk_len; i++) {
                uint8_t u8 = (uint8_t)buf[offset + i];
                chunk[i] = (int16_t)(((int32_t)u8 - 128) * 256);
            }

            size_t written = 0;
            i2s_audio_write(chunk, chunk_len * sizeof(int16_t), &written, 500);
            offset += chunk_len;
        }

        memset(chunk, 0, sizeof(chunk));
        size_t written = 0;
        i2s_audio_write(chunk, 256 * sizeof(int16_t), &written, 200);
    }

    FreeBuffer();
}

static void tts_speak_cloud_internal(const char *text, const char *lang)
{
    if (!lang || strlen(lang) == 0) lang = "en-female";

    // Determine target language and gender
    const char *target_lang = "en";
    bool is_male = false;

    if (strstr(lang, "male") && !strstr(lang, "female")) {
        is_male = true;
    }
    if (strstr(lang, "bn")) {
        target_lang = "bn";
    } else if (strstr(lang, "es")) {
        target_lang = "es";
    } else if (strstr(lang, "fr")) {
        target_lang = "fr";
    } else if (strstr(lang, "ja")) {
        target_lang = "ja";
    } else if (strstr(lang, "en-gb")) {
        target_lang = "en-gb";
    } else {
        target_lang = "en";
    }

    // URL encode text
    char encoded[300] = {0};
    int enc_pos = 0;
    for (int i = 0; text[i] != '\0' && enc_pos < (int)sizeof(encoded) - 4; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            encoded[enc_pos++] = c;
        } else if (c == ' ') {
            encoded[enc_pos++] = '+';
        } else {
            snprintf(encoded + enc_pos, 4, "%%%02X", c);
            enc_pos += 3;
        }
    }

    char url[384];
    snprintf(url, sizeof(url),
             "http://translate.google.com/translate_tts?ie=UTF-8&q=%s&tl=%s&client=gtx",
             encoded, target_lang);

    ESP_LOGI(TAG, "Requesting Cloud Natural Speech: %s (lang=%s, gender=%s)",
             url, target_lang, is_male ? "male" : "female");

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init cloud TTS client, falling back to SAM");
        tts_synthesize_sam(text);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not reach cloud TTS (err=0x%x), falling back to SAM", err);
        esp_http_client_cleanup(client);
        tts_synthesize_sam(text);
        return;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "Cloud TTS returned HTTP %d, falling back to SAM", status);
        esp_http_client_cleanup(client);
        tts_synthesize_sam(text);
        return;
    }

    // Allocate 22KB decoder structure dynamically on HEAP to avoid FreeRTOS stack overflow!
    mp3dec_t *mp3d = (mp3dec_t *)calloc(1, sizeof(mp3dec_t));
    uint8_t *in_buf = (uint8_t *)malloc(4096);
    mp3d_sample_t *pcm_out = (mp3d_sample_t *)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t));
    if (!mp3d || !in_buf || !pcm_out) {
        ESP_LOGE(TAG, "Failed to allocate MP3 decode heap memory, falling back to SAM");
        if (in_buf) free(in_buf);
        if (pcm_out) free(pcm_out);
        if (mp3d) free(mp3d);
        esp_http_client_cleanup(client);
        tts_synthesize_sam(text);
        return;
    }

    mp3dec_init(mp3d);

    int bytes_in_buf = 0;
    bool is_first_frame = true;

    while (1) {
        if (bytes_in_buf < 2048) {
            int r = esp_http_client_read(client, (char *)in_buf + bytes_in_buf, 4096 - bytes_in_buf);
            if (r > 0) {
                bytes_in_buf += r;
            } else if (r == 0 && bytes_in_buf == 0) {
                break;
            } else if (r < 0) {
                ESP_LOGW(TAG, "Stream read error: %d", r);
                break;
            }
        }

        if (bytes_in_buf == 0) break;

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(mp3d, in_buf, bytes_in_buf, pcm_out, &info);
        if (info.frame_bytes > 0) {
            bytes_in_buf -= info.frame_bytes;
            if (bytes_in_buf > 0) {
                memmove(in_buf, in_buf + info.frame_bytes, bytes_in_buf);
            }
            if (samples > 0) {
                if (is_first_frame) {
                    uint32_t play_rate = info.hz;
                    if (is_male) {
                        play_rate = (uint32_t)(info.hz * 0.82f); // Pitch down to adult male frequency
                    }
                    ESP_LOGI(TAG, "Cloud speech stream: %lu Hz (orig %d Hz), %d ch (gender=%s)",
                             (unsigned long)play_rate, info.hz, info.channels, is_male ? "male" : "female");
                    i2s_audio_set_params(play_rate, 16, info.channels);
                    is_first_frame = false;
                }
                size_t pcm_bytes = samples * info.channels * sizeof(mp3d_sample_t);
                size_t written = 0;
                i2s_audio_write(pcm_out, pcm_bytes, &written, 500);
            }
        } else {
            // Find next sync word (0xFF 0xEx) to skip padding
            int skip = 1;
            for (int i = 1; i + 1 < bytes_in_buf; i++) {
                if ((in_buf[i] == 0xFF) && ((in_buf[i + 1] & 0xE0) == 0xE0)) {
                    skip = i;
                    break;
                }
            }
            bytes_in_buf -= skip;
            if (bytes_in_buf > 0) {
                memmove(in_buf, in_buf + skip, bytes_in_buf);
            } else {
                bytes_in_buf = 0;
            }
        }
    }

    free(in_buf);
    free(pcm_out);
    free(mp3d);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "Cloud speech playback complete");
}

static void tts_task(void *arg)
{
    tts_msg_t msg;
    while (1) {
        if (xQueueReceive(s_tts_queue, &msg, portMAX_DELAY) == pdPASS) {
            s_tts_busy = true;
            wake_word_service_pause();
            if (msg.mode == TTS_MODE_CLOUD_NATURAL) {
                tts_speak_cloud_internal(msg.text, msg.lang);
            } else {
                tts_synthesize_sam(msg.text);
            }
            wake_word_service_resume();
            s_tts_busy = false;
        }
    }
}

esp_err_t tts_service_init(void)
{
    if (s_tts_queue != NULL) {
        return ESP_OK;
    }

    s_tts_queue = xQueueCreate(4, sizeof(tts_msg_t));
    if (!s_tts_queue) {
        ESP_LOGE(TAG, "Failed to create TTS queue");
        return ESP_ERR_NO_MEM;
    }

    // Pin TTS worker to Core 0 with 12KB stack (mp3dec_t is now on heap)
    BaseType_t ret = xTaskCreatePinnedToCore(
        tts_task,
        "tts_task",
        12288,
        NULL,
        3,
        NULL,
        0
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TTS task");
        vQueueDelete(s_tts_queue);
        s_tts_queue = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TTS Service initialized successfully (SAM + Cloud Natural Voice)");
    return ESP_OK;
}

