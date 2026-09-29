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
static char s_current_track_name[256] = "No Track";
static audio_player_finish_cb_t s_finish_cb = NULL;

void audio_player_set_finish_callback(audio_player_finish_cb_t cb)
{
    s_finish_cb = cb;
}

static void extract_filename(const char *path, char *dest, size_t dest_sz)
{
    if (!path || !dest || dest_sz == 0) return;
    const char *p = strrchr(path, '/');
    if (!p) p = strrchr(path, '\\');
    p = (p != NULL) ? p + 1 : path;
    size_t i = 0;
    while (i + 1 < dest_sz && p[i] != '\0') {
        dest[i] = p[i];
        i++;
    }
    dest[i] = '\0';
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

        ESP_LOGI(TAG, "ID3v2 metadata detected (size: %zu bytes). Fast-seeking to audio...", total_skip);
        fseek(f, start_pos + total_skip, SEEK_SET);
    } else {
        fseek(f, start_pos, SEEK_SET);
    }
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

    ESP_LOGI(TAG, "WAV Stream: %lu Hz, %d bits, %d ch", (unsigned long)sample_rate, bits, channels);
    i2s_audio_set_params(sample_rate, bits, channels);

    bool natural_eof = false;
    uint8_t buf[2048];
    while (s_player_state != AUDIO_STATE_IDLE) {
        if (s_player_state == AUDIO_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Check if new command arrived
        audio_cmd_t next_cmd;
        if (xQueuePeek(s_audio_cmd_queue, &next_cmd, 0) == pdTRUE) {
            return false;
        }

        size_t r = fread(buf, 1, sizeof(buf), f);
        if (r == 0) {
            natural_eof = true;
            break;
        }

        size_t written = 0;
        i2s_audio_write(buf, r, &written, 500);
    }
    return natural_eof && (s_player_state != AUDIO_STATE_IDLE);
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

    uint8_t *input_buf = malloc(MP3_STREAM_BUF_SIZE);
    mp3d_sample_t *pcm_out = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t));

    if (!input_buf || !pcm_out) {
        ESP_LOGE(TAG, "Failed to allocate MP3 decode buffers");
        if (input_buf) free(input_buf);
        if (pcm_out) free(pcm_out);
        free(mp3d);
        return false;
    }

    int bytes_left = 0;
    bool is_first_frame = true;
    bool natural_eof = false;

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

        // Refill stream buffer with next batch of bytes from SD
        if (bytes_left < (MP3_STREAM_BUF_SIZE / 2)) {
            size_t read_cnt = fread(input_buf + bytes_left, 1, MP3_STREAM_BUF_SIZE - bytes_left, f);
            bytes_left += read_cnt;
            if (bytes_left == 0) {
                natural_eof = true;
                break; // End of file reached
            }
        }

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(mp3d, input_buf, bytes_left, pcm_out, &info);

        if (info.frame_bytes > 0) {
            bytes_left -= info.frame_bytes;
            if (bytes_left > 0) {
                memmove(input_buf, input_buf + info.frame_bytes, bytes_left);
            }

            if (samples > 0) {
                if (is_first_frame) {
                    ESP_LOGI(TAG, "MP3 Stream: %d Hz, %d Channels, %d kbps",
                             info.hz, info.channels, info.bitrate_kbps);
                    i2s_audio_set_params(info.hz, 16, info.channels);
                    is_first_frame = false;
                }

                size_t pcm_bytes = samples * info.channels * sizeof(mp3d_sample_t);
                size_t written = 0;
                i2s_audio_write(pcm_out, pcm_bytes, &written, 500);
            }
        } else {
            // Find next sync word (0xFF 0xEx) to skip any non-audio padding
            int skip = 1;
            for (int i = 1; i + 1 < bytes_left; i++) {
                if (input_buf[i] == 0xFF && (input_buf[i + 1] & 0xE0) == 0xE0) {
                    skip = i;
                    break;
                }
            }
            bytes_left -= skip;
            if (bytes_left > 0) {
                memmove(input_buf, input_buf + skip, bytes_left);
            } else {
                if (feof(f)) {
                    natural_eof = true;
                    break;
                }
            }
        }
    }

    free(input_buf);
    free(pcm_out);
    free(mp3d);
    return natural_eof && (s_player_state != AUDIO_STATE_IDLE);
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

                    bool completed = false;
                    const char *ext = strrchr(cmd.filepath, '.');
                    if (ext && strcasecmp(ext, ".wav") == 0) {
                        completed = play_wav_file(f);
                    } else {
                        completed = play_mp3_file(f);
                    }

                    fclose(f);
                    s_player_state = AUDIO_STATE_IDLE;
                    ESP_LOGI(TAG, "Finished playback of '%s' (completed=%d)", s_current_track_name, (int)completed);

                    if (completed && s_finish_cb) {
                        s_finish_cb();
                    }
                    break;
                }

                case CMD_PLAY_CLICK:
                    i2s_audio_play_beep(1800, 25);
                    break;

                case CMD_PLAY_HAPPY:
                    // Cheerful multi-tone chirp
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

    // Initialize I2S Audio Driver default (44.1 kHz, 16-bit, stereo)
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
    if (s_player_state == AUDIO_STATE_PLAYING) return;
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
