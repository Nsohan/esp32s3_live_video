#include "audio_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "i2s_audio.h"
#include "sdcard.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static const char *TAG = "audio_player";

#define MP3_STREAM_BUF_SIZE   4096

typedef enum {
    CMD_PLAY_FILE,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP,
    CMD_PLAY_CLICK,
    CMD_PLAY_HAPPY,
    CMD_PLAY_TEST_TONE
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t type;
    char filepath[256];
} audio_cmd_t;

static QueueHandle_t s_audio_cmd_queue = NULL;
static TaskHandle_t s_audio_task_handle = NULL;
static audio_player_state_t s_player_state = AUDIO_STATE_IDLE;
static char s_current_track_name[128] = "No Track";

static void extract_filename(const char *path, char *dest, size_t dest_sz)
{
    const char *p = strrchr(path, '/');
    if (!p) p = strrchr(path, '\\');
    p = (p != NULL) ? p + 1 : path;
    strncpy(dest, p, dest_sz - 1);
    dest[dest_sz - 1] = '\0';
}

#include "esp_heap_caps.h"

#define PSRAM_STREAM_BUF_SIZE   (128 * 1024)
#define STAGING_BUF_SIZE        (8 * 1024)
#define SD_READ_CHUNK_SIZE      (8 * 1024)

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
} psram_ring_t;

static bool ring_init(psram_ring_t *rb, size_t size)
{
    rb->buffer = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rb->buffer) {
        rb->buffer = (uint8_t *)malloc(size / 4); // fallback to internal SRAM if PSRAM unavailable
        rb->capacity = size / 4;
    } else {
        rb->capacity = size;
    }
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    return (rb->buffer != NULL);
}

static void ring_free(psram_ring_t *rb)
{
    if (rb->buffer) {
        free(rb->buffer);
        rb->buffer = NULL;
    }
    rb->count = 0;
}

static size_t ring_write(psram_ring_t *rb, const uint8_t *data, size_t len)
{
    size_t space = rb->capacity - rb->count;
    if (len > space) len = space;
    if (len == 0) return 0;

    size_t first_part = rb->capacity - rb->head;
    if (first_part > len) first_part = len;

    memcpy(rb->buffer + rb->head, data, first_part);
    if (len > first_part) {
        memcpy(rb->buffer, data + first_part, len - first_part);
    }
    rb->head = (rb->head + len) % rb->capacity;
    rb->count += len;
    return len;
}

static size_t ring_read(psram_ring_t *rb, uint8_t *data, size_t len)
{
    if (len > rb->count) len = rb->count;
    if (len == 0) return 0;

    size_t first_part = rb->capacity - rb->tail;
    if (first_part > len) first_part = len;

    memcpy(data, rb->buffer + rb->tail, first_part);
    if (len > first_part) {
        memcpy(data + first_part, rb->buffer, len - first_part);
    }
    rb->tail = (rb->tail + len) % rb->capacity;
    rb->count -= len;
    return len;
}

static size_t ring_free_space(psram_ring_t *rb)
{
    return rb->capacity - rb->count;
}

static bool play_wav_file(FILE *f)
{
    uint8_t header[12];
    if (fread(header, 1, 12, f) != 12) {
        ESP_LOGE(TAG, "Invalid WAV header");
        return false;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a valid RIFF/WAVE file");
        return false;
    }

    uint16_t channels = 2;
    uint32_t sample_rate = 44100;
    uint16_t bits = 16;
    bool found_fmt = false;
    bool found_data = false;

    // Scan chunks (robust against ID3 metadata, JUNK, LIST chunks)
    while (!found_data) {
        uint8_t chunk_hdr[8];
        if (fread(chunk_hdr, 1, 8, f) != 8) {
            break;
        }

        uint32_t chunk_size = *(uint32_t *)(chunk_hdr + 4);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            uint8_t fmt_buf[16];
            size_t to_read = (chunk_size < sizeof(fmt_buf)) ? chunk_size : sizeof(fmt_buf);
            if (fread(fmt_buf, 1, to_read, f) != to_read) {
                break;
            }
            channels = *(uint16_t *)(fmt_buf + 2);
            sample_rate = *(uint32_t *)(fmt_buf + 4);
            bits = *(uint16_t *)(fmt_buf + 14);
            if (chunk_size > to_read) {
                fseek(f, chunk_size - to_read, SEEK_CUR);
            }
            found_fmt = true;
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            found_data = true;
            break;
        } else {
            // Skip other metadata/junk chunks
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) {
        ESP_LOGE(TAG, "Could not find valid fmt or data chunk in WAV file");
        return false;
    }

    ESP_LOGI(TAG, "WAV Info: %lu Hz, %d bits, %d ch", (unsigned long)sample_rate, bits, channels);
    i2s_audio_set_params(sample_rate, bits, channels);

    psram_ring_t ring;
    if (!ring_init(&ring, PSRAM_STREAM_BUF_SIZE)) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM ring buffer for WAV");
        return false;
    }

    uint8_t *sd_temp = (uint8_t *)malloc(SD_READ_CHUNK_SIZE);
    uint8_t *out_chunk = (uint8_t *)malloc(4096);
    if (!sd_temp || !out_chunk) {
        if (sd_temp) free(sd_temp);
        if (out_chunk) free(out_chunk);
        ring_free(&ring);
        return false;
    }

    bool eof_reached = false;
    // Pre-buffer from SD
    while (!eof_reached && ring_free_space(&ring) >= SD_READ_CHUNK_SIZE) {
        size_t r = fread(sd_temp, 1, SD_READ_CHUNK_SIZE, f);
        if (r > 0) ring_write(&ring, sd_temp, r);
        if (r < SD_READ_CHUNK_SIZE) eof_reached = true;
    }

    while (s_player_state != AUDIO_STATE_IDLE) {
        if (s_player_state == AUDIO_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Check if new command arrived
        audio_cmd_t next_cmd;
        if (xQueuePeek(s_audio_cmd_queue, &next_cmd, 0) == pdTRUE) {
            break;
        }

        // Background stream fill from SD
        if (!eof_reached && ring_free_space(&ring) >= SD_READ_CHUNK_SIZE) {
            size_t r = fread(sd_temp, 1, SD_READ_CHUNK_SIZE, f);
            if (r > 0) ring_write(&ring, sd_temp, r);
            if (r < SD_READ_CHUNK_SIZE) eof_reached = true;
        }

        size_t to_play = ring_read(&ring, out_chunk, 4096);
        if (to_play == 0 && eof_reached) break;

        if (to_play > 0) {
            size_t written = 0;
            i2s_audio_write(out_chunk, to_play, &written, 150);
        }
    }

    free(sd_temp);
    free(out_chunk);
    ring_free(&ring);
    return true;
}

static void skip_id3v2_tag(FILE *f)
{
    uint8_t hdr[10];
    long start_pos = ftell(f);
    if (fread(hdr, 1, 10, f) != 10) {
        fseek(f, start_pos, SEEK_SET);
        return;
    }

    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        // ID3v2 tag detected: size is a 28-bit synchsafe integer
        uint32_t tag_size = ((hdr[6] & 0x7F) << 21) |
                            ((hdr[7] & 0x7F) << 14) |
                            ((hdr[8] & 0x7F) << 7)  |
                            (hdr[9] & 0x7F);
        size_t total_skip = 10 + tag_size;
        if (hdr[5] & 0x10) total_skip += 10; // Footer present

        ESP_LOGI(TAG, "ID3v2 tag detected (Version 2.%d.%d, size: %zu bytes). Skipping metadata...",
                 hdr[3], hdr[4], total_skip);
        fseek(f, start_pos + total_skip, SEEK_SET);
    } else {
        fseek(f, start_pos, SEEK_SET);
    }
}

static bool play_mp3_file(FILE *f)
{
    skip_id3v2_tag(f);

    mp3dec_t *mp3d = (mp3dec_t *)calloc(1, sizeof(mp3dec_t));
    if (!mp3d) {
        ESP_LOGE(TAG, "Failed to allocate MP3 decoder state");
        return false;
    }
    mp3dec_init(mp3d);

    psram_ring_t ring;
    if (!ring_init(&ring, PSRAM_STREAM_BUF_SIZE)) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM ring buffer");
        free(mp3d);
        return false;
    }

    uint8_t *staging_buf = (uint8_t *)malloc(STAGING_BUF_SIZE);
    mp3d_sample_t *pcm_out = (mp3d_sample_t *)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t));
    uint8_t *sd_temp = (uint8_t *)malloc(SD_READ_CHUNK_SIZE);

    if (!staging_buf || !pcm_out || !sd_temp) {
        ESP_LOGE(TAG, "Failed to allocate MP3 decode buffers");
        if (staging_buf) free(staging_buf);
        if (pcm_out) free(pcm_out);
        if (sd_temp) free(sd_temp);
        ring_free(&ring);
        free(mp3d);
        return false;
    }

    // 1. Initial Pre-buffering: Fill PSRAM ring buffer from SD
    bool eof_reached = false;
    while (!eof_reached && ring_free_space(&ring) >= SD_READ_CHUNK_SIZE) {
        size_t r = fread(sd_temp, 1, SD_READ_CHUNK_SIZE, f);
        if (r > 0) {
            ring_write(&ring, sd_temp, r);
        }
        if (r < SD_READ_CHUNK_SIZE) {
            eof_reached = true;
        }
    }
    ESP_LOGI(TAG, "Pre-buffered %zu bytes into PSRAM ring buffer", ring.count);

    size_t staging_bytes = 0;
    bool is_first_frame = true;

    while (s_player_state != AUDIO_STATE_IDLE) {
        if (s_player_state == AUDIO_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Check if next command is pending
        audio_cmd_t next_cmd;
        if (xQueuePeek(s_audio_cmd_queue, &next_cmd, 0) == pdTRUE) {
            break;
        }

        // Top up PSRAM ring buffer from SD card in background
        if (!eof_reached && ring_free_space(&ring) >= SD_READ_CHUNK_SIZE) {
            size_t r = fread(sd_temp, 1, SD_READ_CHUNK_SIZE, f);
            if (r > 0) {
                ring_write(&ring, sd_temp, r);
            }
            if (r < SD_READ_CHUNK_SIZE) {
                eof_reached = true;
            }
        }

        // Top up staging buffer from PSRAM ring buffer
        if (staging_bytes < (STAGING_BUF_SIZE / 2) && ring.count > 0) {
            size_t pull = STAGING_BUF_SIZE - staging_bytes;
            size_t got = ring_read(&ring, staging_buf + staging_bytes, pull);
            staging_bytes += got;
        }

        if (staging_bytes == 0 && eof_reached) {
            break; // End of stream reached
        }

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(mp3d, staging_buf, staging_bytes, pcm_out, &info);

        if (info.frame_bytes > 0) {
            staging_bytes -= info.frame_bytes;
            memmove(staging_buf, staging_buf + info.frame_bytes, staging_bytes);

            if (samples > 0 && info.hz > 0) {
                if (is_first_frame) {
                    ESP_LOGI(TAG, "MP3 Stream: %d Hz, %d Channels, %d kbps",
                             info.hz, info.channels, info.bitrate_kbps);
                    is_first_frame = false;
                }
                i2s_audio_set_params(info.hz, 16, info.channels);

                size_t pcm_bytes = samples * info.channels * sizeof(mp3d_sample_t);
                size_t written = 0;
                i2s_audio_write(pcm_out, pcm_bytes, &written, 150);
            }
        } else {
            // Find next sync word (0xFF 0xEx)
            size_t skip = 1;
            for (size_t i = 1; i + 1 < staging_bytes; i++) {
                if (staging_buf[i] == 0xFF && (staging_buf[i + 1] & 0xE0) == 0xE0) {
                    skip = i;
                    break;
                }
            }
            if (staging_bytes > skip) {
                staging_bytes -= skip;
                memmove(staging_buf, staging_buf + skip, staging_bytes);
            } else {
                staging_bytes = 0;
            }
            if (eof_reached && staging_bytes == 0) {
                break;
            }
        }
    }

    free(staging_buf);
    free(pcm_out);
    free(sd_temp);
    ring_free(&ring);
    free(mp3d);
    return true;
}

static void audio_worker_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio Player Background Task started");
    audio_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_audio_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch (cmd.type) {
                case CMD_PLAY_FILE: {
                    extract_filename(cmd.filepath, s_current_track_name, sizeof(s_current_track_name));
                    ESP_LOGI(TAG, "Opening audio file: '%s'...", cmd.filepath);

                    FILE *f = fopen(cmd.filepath, "rb");
                    if (!f) {
                        ESP_LOGE(TAG, "Failed to open file: %s", cmd.filepath);
                        s_player_state = AUDIO_STATE_IDLE;
                        break;
                    }

                    s_player_state = AUDIO_STATE_PLAYING;

                    const char *ext = strrchr(cmd.filepath, '.');
                    if (ext && strcasecmp(ext, ".wav") == 0) {
                        play_wav_file(f);
                    } else {
                        play_mp3_file(f);
                    }

                    fclose(f);
                    s_player_state = AUDIO_STATE_IDLE;
                    ESP_LOGI(TAG, "Finished playback of '%s'", s_current_track_name);
                    break;
                }

                case CMD_PLAY_CLICK:
                    i2s_audio_play_beep(1800, 25);
                    break;

                case CMD_PLAY_HAPPY:
                    // Play a cheerful multi-tone Loona chirp
                    i2s_audio_play_beep(880, 60);
                    vTaskDelay(pdMS_TO_TICKS(15));
                    i2s_audio_play_beep(1174, 80);
                    vTaskDelay(pdMS_TO_TICKS(15));
                    i2s_audio_play_beep(1760, 120);
                    break;

                case CMD_PLAY_TEST_TONE:
                    ESP_LOGI(TAG, "Playing 1kHz test tone (1.0 sec)...");
                    i2s_audio_play_beep(1000, 1000);
                    break;

                case CMD_PAUSE:
                    s_player_state = AUDIO_STATE_PAUSED;
                    break;

                case CMD_RESUME:
                    s_player_state = AUDIO_STATE_PLAYING;
                    break;

                case CMD_STOP:
                    s_player_state = AUDIO_STATE_IDLE;
                    break;
            }
        }
    }
}

esp_err_t audio_player_init(void)
{
    if (s_audio_cmd_queue != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Audio Player Service...");

    // Initialize I2S Audio Driver default
    i2s_audio_init(44100, 16, 2);

    s_audio_cmd_queue = xQueueCreate(8, sizeof(audio_cmd_t));
    if (!s_audio_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create audio command queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        audio_worker_task,
        "audio_worker",
        8192,
        NULL,
        5, // High priority for smooth jitter-free audio
        &s_audio_task_handle,
        0  // Core 0
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio worker task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio Player Service initialized successfully!");
    return ESP_OK;
}

esp_err_t audio_player_play_file(const char *filepath)
{
    if (!filepath || strlen(filepath) == 0) return ESP_ERR_INVALID_ARG;

    audio_cmd_t cmd = {
        .type = CMD_PLAY_FILE,
    };
    strncpy(cmd.filepath, filepath, sizeof(cmd.filepath) - 1);

    // Stop current track and queue new one
    s_player_state = AUDIO_STATE_IDLE;
    xQueueReset(s_audio_cmd_queue);
    xQueueSend(s_audio_cmd_queue, &cmd, portMAX_DELAY);
    return ESP_OK;
}

void audio_player_pause(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) {
        s_player_state = AUDIO_STATE_PAUSED;
    }
}

void audio_player_resume(void)
{
    if (s_player_state == AUDIO_STATE_PAUSED) {
        s_player_state = AUDIO_STATE_PLAYING;
    }
}

void audio_player_toggle_play_pause(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) {
        s_player_state = AUDIO_STATE_PAUSED;
    } else if (s_player_state == AUDIO_STATE_PAUSED) {
        s_player_state = AUDIO_STATE_PLAYING;
    }
}

void audio_player_stop(void)
{
    s_player_state = AUDIO_STATE_IDLE;
    audio_cmd_t cmd = {.type = CMD_STOP};
    xQueueSend(s_audio_cmd_queue, &cmd, 0);
}

audio_player_state_t audio_player_get_state(void)
{
    return s_player_state;
}

const char* audio_player_get_current_track_name(void)
{
    return s_current_track_name;
}

void audio_player_set_volume(uint8_t volume)
{
    i2s_audio_set_volume(volume);
}

uint8_t audio_player_get_volume(void)
{
    return i2s_audio_get_volume();
}

void audio_player_play_ui_click(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) return; // Don't interrupt music with clicks
    audio_cmd_t cmd = {.type = CMD_PLAY_CLICK};
    xQueueSend(s_audio_cmd_queue, &cmd, 0);
}

void audio_player_play_happy_sound(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) return;
    audio_cmd_t cmd = {.type = CMD_PLAY_HAPPY};
    xQueueSend(s_audio_cmd_queue, &cmd, 0);
}

void audio_player_play_test_tone(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) return;
    audio_cmd_t cmd = {.type = CMD_PLAY_TEST_TONE};
    xQueueSend(s_audio_cmd_queue, &cmd, 0);
}
