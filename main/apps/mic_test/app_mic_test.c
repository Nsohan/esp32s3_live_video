#include "app_mic_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "display_gfx.h"
#include "i2s_audio.h"
#include "audio_player.h"

static const char *TAG = "app_mic_test";

#define VU_SEGMENTS_COUNT  20
#define VU_START_X         20
#define VU_START_Y         98
#define VU_WIDTH           280
#define VU_HEIGHT          26

static uint32_t s_last_update_ms = 0;
static int s_prev_lit_segments = 0;
static int s_prev_peak_segment = 0;
static float s_decay_peak = 0.0f;
static float s_last_drawn_rms = -1.0f;
static int32_t s_last_drawn_peak = -1;
static float s_last_drawn_gain = -1.0f;
static bool s_last_drawn_mute = false;

void app_mic_test_init(void)
{
    ESP_LOGI(TAG, "Initializing Mic Test & Live Passthrough App...");

    // Stop any background music track
    audio_player_stop();

    // Start 16 kHz 32-bit full-duplex DSP passthrough (default 18x boost)
    i2s_audio_passthrough_start(18.0f);

    s_prev_lit_segments = 0;
    s_prev_peak_segment = 0;
    s_decay_peak = 0.0f;
    s_last_drawn_rms = -1.0f;
    s_last_drawn_peak = -1;
    s_last_drawn_gain = -1.0f;
    s_last_drawn_mute = false;
}

void app_mic_test_stop(void)
{
    ESP_LOGI(TAG, "Exiting Mic Test App, stopping passthrough...");
    i2s_audio_passthrough_stop();
}

void app_mic_test_draw(void)
{
    display_fill_screen(COLOR_BG_DARK);

    // 1. Top Navigation Bar
    app_common_draw_header("MIC & SPEAKER TEST");

    // 2. Info Card (Wiring & Latency status)
    gfx_fill_round_rect(10, 32, 300, 48, 4, COLOR_CARD_BG);
    gfx_draw_round_rect(10, 32, 300, 48, 4, COLOR_CARD_BORDER);

    gfx_draw_string(20, 38, "REAL-TIME AUDIO LOOPBACK", COLOR_CYAN_ACCENT, COLOR_CARD_BG, 1);
    gfx_draw_string(20, 52, "INMP441 (45) -> MAX98357A (21)", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
    gfx_draw_string(20, 64, "Latency: ~16ms | Rate: 16 kHz", COLOR_GREEN_ACCENT, COLOR_CARD_BG, 1);

    // 3. Central VU Meter Frame Card
    gfx_fill_round_rect(10, 86, 300, 88, 4, COLOR_CARD_BG);
    gfx_draw_round_rect(10, 86, 300, 88, 4, COLOR_CARD_BORDER);

    gfx_draw_string(20, 92, "MICROPHONE INPUT LEVEL (VU)", COLOR_WHITE, COLOR_CARD_BG, 1);

    // Draw initial blank VU meter segments
    int seg_w = (VU_WIDTH - (VU_SEGMENTS_COUNT * 2)) / VU_SEGMENTS_COUNT;
    for (int i = 0; i < VU_SEGMENTS_COUNT; i++) {
        int x = VU_START_X + i * (seg_w + 2);
        gfx_fill_rect(x, 108, seg_w, 20, 0x18C3); // Dim background slot
    }

    // VU dB ticks
    gfx_draw_string(20, 132, "-40dB", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
    gfx_draw_string_centered(160, 132, 40, "-12dB", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
    gfx_draw_string(270, 132, " 0dB", COLOR_RED_ACCENT, COLOR_CARD_BG, 1);

    // Dynamic numeric telemetry row
    gfx_draw_string(20, 154, "RMS: 0.0     Peak: 0", COLOR_WHITE, COLOR_CARD_BG, 1);

    // 4. Control Buttons (Gain - / + and Mute Toggle)
    float gain = i2s_audio_passthrough_get_gain();
    bool muted = i2s_audio_passthrough_is_muted();

    // Button: [ - BOOST ]
    gfx_fill_round_rect(10, 182, 65, 48, 4, COLOR_CARD_BG);
    gfx_draw_round_rect(10, 182, 65, 48, 4, COLOR_CARD_BORDER);
    gfx_draw_string_centered(10, 192, 65, "-", COLOR_CYAN_ACCENT, COLOR_CARD_BG, 2);
    gfx_draw_string_centered(10, 212, 65, "BOOST", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);

    // Gain Badge (Center)
    gfx_fill_round_rect(82, 182, 76, 48, 4, COLOR_CARD_BG);
    gfx_draw_round_rect(82, 182, 76, 48, 4, COLOR_CARD_BORDER);
    gfx_draw_string_centered(82, 190, 76, "GAIN", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
    char gain_str[16];
    snprintf(gain_str, sizeof(gain_str), "%.1fx", gain);
    gfx_draw_string_centered(82, 208, 76, gain_str, COLOR_YELLOW_ACCENT, COLOR_CARD_BG, 1);

    // Button: [ + BOOST ]
    gfx_fill_round_rect(165, 182, 65, 48, 4, COLOR_CARD_BG);
    gfx_draw_round_rect(165, 182, 65, 48, 4, COLOR_CARD_BORDER);
    gfx_draw_string_centered(165, 192, 65, "+", COLOR_CYAN_ACCENT, COLOR_CARD_BG, 2);
    gfx_draw_string_centered(165, 212, 65, "BOOST", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);

    // Button: [ MUTE / LIVE ]
    uint16_t mute_btn_bg = muted ? COLOR_RED_ACCENT : COLOR_GREEN_ACCENT;
    gfx_fill_round_rect(238, 182, 72, 48, 4, mute_btn_bg);
    gfx_draw_round_rect(238, 182, 72, 48, 4, COLOR_WHITE);
    gfx_draw_string_centered(238, 192, 72, muted ? "MUTED" : "LIVE", COLOR_WHITE, mute_btn_bg, 1);
    gfx_draw_string_centered(238, 208, 72, muted ? "SPK OFF" : "SPK ON", COLOR_WHITE, mute_btn_bg, 1);

    s_prev_lit_segments = 0;
    s_prev_peak_segment = 0;
    s_last_drawn_gain = gain;
    s_last_drawn_mute = muted;
}

void app_mic_test_update(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (now - s_last_update_ms < 35) { // ~30 FPS refresh rate
        return;
    }
    s_last_update_ms = now;

    float rms = 0.0f;
    int32_t peak = 0;
    i2s_audio_passthrough_get_levels(&rms, &peak);

    // Smooth peak decay
    if (rms > s_decay_peak) {
        s_decay_peak = rms;
    } else {
        s_decay_peak -= 60.0f;
        if (s_decay_peak < 0.0f) s_decay_peak = 0.0f;
    }

    // Map RMS to VU segments (0 to ~2500 RMS)
    int lit_count = (int)((rms / 2200.0f) * VU_SEGMENTS_COUNT);
    if (lit_count > VU_SEGMENTS_COUNT) lit_count = VU_SEGMENTS_COUNT;
    if (lit_count < 0) lit_count = 0;

    int peak_seg = (int)((s_decay_peak / 2200.0f) * VU_SEGMENTS_COUNT);
    if (peak_seg >= VU_SEGMENTS_COUNT) peak_seg = VU_SEGMENTS_COUNT - 1;

    // Draw VU Meter Segments
    int seg_w = (VU_WIDTH - (VU_SEGMENTS_COUNT * 2)) / VU_SEGMENTS_COUNT;
    for (int i = 0; i < VU_SEGMENTS_COUNT; i++) {
        int x = VU_START_X + i * (seg_w + 2);
        uint16_t color;

        if (i < lit_count || i == peak_seg) {
            if (i < 12) {
                color = COLOR_GREEN_ACCENT; // Green
            } else if (i < 16) {
                color = COLOR_YELLOW_ACCENT; // Yellow
            } else {
                color = COLOR_RED_ACCENT; // Red overload
            }
        } else {
            color = 0x18C3; // Dim off
        }

        gfx_fill_rect(x, 108, seg_w, 20, color);
    }
    s_prev_lit_segments = lit_count;
    s_prev_peak_segment = peak_seg;

    // Update Telemetry Text if changed
    if (fabs(rms - s_last_drawn_rms) > 5.0f || peak != s_last_drawn_peak) {
        s_last_drawn_rms = rms;
        s_last_drawn_peak = peak;

        char telem_str[64];
        snprintf(telem_str, sizeof(telem_str), "RMS: %5.1f   Peak: %5ld  ", rms, (long)peak);
        gfx_draw_string(20, 154, telem_str, COLOR_WHITE, COLOR_CARD_BG, 1);
    }
}

bool app_mic_test_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw)
{
    if (app_common_is_back_pressed(tx, ty)) {
        app_mic_test_stop();
        if (next_state) *next_state = STATE_APP_MENU;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }

    // Touch: [ - BOOST ] (X: 10..75, Y: 182..230)
    if (tx >= 10 && tx <= 75 && ty >= 182 && ty <= 230) {
        float gain = i2s_audio_passthrough_get_gain();
        gain -= 2.0f;
        if (gain < 2.0f) gain = 2.0f;
        i2s_audio_passthrough_set_gain(gain);

        // Update Gain badge display
        char gain_str[16];
        snprintf(gain_str, sizeof(gain_str), "%.1fx", gain);
        gfx_fill_round_rect(84, 204, 72, 22, 2, COLOR_CARD_BG);
        gfx_draw_string_centered(82, 208, 76, gain_str, COLOR_YELLOW_ACCENT, COLOR_CARD_BG, 1);
        return true;
    }

    // Touch: [ + BOOST ] (X: 165..230, Y: 182..230)
    if (tx >= 165 && tx <= 230 && ty >= 182 && ty <= 230) {
        float gain = i2s_audio_passthrough_get_gain();
        gain += 2.0f;
        if (gain > 30.0f) gain = 30.0f;
        i2s_audio_passthrough_set_gain(gain);

        // Update Gain badge display
        char gain_str[16];
        snprintf(gain_str, sizeof(gain_str), "%.1fx", gain);
        gfx_fill_round_rect(84, 204, 72, 22, 2, COLOR_CARD_BG);
        gfx_draw_string_centered(82, 208, 76, gain_str, COLOR_YELLOW_ACCENT, COLOR_CARD_BG, 1);
        return true;
    }

    // Touch: [ MUTE / LIVE ] (X: 238..310, Y: 182..230)
    if (tx >= 238 && tx <= 310 && ty >= 182 && ty <= 230) {
        bool current_muted = i2s_audio_passthrough_is_muted();
        bool new_muted = !current_muted;
        i2s_audio_passthrough_set_mute(new_muted);

        // Redraw Mute button
        uint16_t mute_btn_bg = new_muted ? COLOR_RED_ACCENT : COLOR_GREEN_ACCENT;
        gfx_fill_round_rect(238, 182, 72, 48, 4, mute_btn_bg);
        gfx_draw_round_rect(238, 182, 72, 48, 4, COLOR_WHITE);
        gfx_draw_string_centered(238, 192, 72, new_muted ? "MUTED" : "LIVE", COLOR_WHITE, mute_btn_bg, 1);
        gfx_draw_string_centered(238, 208, 72, new_muted ? "SPK OFF" : "SPK ON", COLOR_WHITE, mute_btn_bg, 1);
        return true;
    }

    return false;
}
