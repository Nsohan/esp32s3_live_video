#include "app_common.h"
#include "display_gfx.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "audio_player.h"
#include "wifi_service.h"
#include "http_stream.h"
#include "battery_service.h"

#define STATUS_BAR_W 320
#define STATUS_BAR_H 22
#define SWAP_BYTES(x) ((uint16_t)((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF)))

// Off-screen status bar composition buffer (14 KB)
static uint16_t s_status_bar_buf[STATUS_BAR_W * STATUS_BAR_H];

// Cache for dirty check to prevent unnecessary redraws
static char s_last_time_str[16] = "";
static int s_last_wifi_connected = -1;
static int s_last_bat_pct = -1;
static int s_last_bat_charging = -1;
static int s_last_audio_playing = -1;
static int s_last_stream_active = -1;

void app_common_draw_header(const char *title)
{
    gfx_fill_rect(0, 0, 320, 26, 0x10A2);
    gfx_fill_round_rect(4, 3, 58, 20, 4, 0x2124);
    gfx_draw_icon(14, 13, ICON_BACK, COLOR_WHITE);
    gfx_draw_string(24, 8, "BACK", COLOR_WHITE, 0x2124, 1);
    gfx_draw_string_centered(70, 8, 180, title, COLOR_CYAN, 0x10A2, 1);
    gfx_fill_rect(0, 26, 320, 1, 0x31A6);
}

// ─── Fast In-Memory Status Bar Compositor ───────────────────
static inline void sb_set_pixel(int x, int y, uint16_t be_color)
{
    if (x >= 0 && x < STATUS_BAR_W && y >= 0 && y < STATUS_BAR_H) {
        s_status_bar_buf[y * STATUS_BAR_W + x] = be_color;
    }
}

static void sb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    uint16_t be = SWAP_BYTES(color);
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > STATUS_BAR_W) x2 = STATUS_BAR_W;
    if (y2 > STATUS_BAR_H) y2 = STATUS_BAR_H;

    for (int r = y; r < y2; r++) {
        for (int c = x; c < x2; c++) {
            s_status_bar_buf[r * STATUS_BAR_W + c] = be;
        }
    }
}

static void sb_draw_char(int x, int y, char c, uint16_t color, uint16_t bg)
{
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font8x16[c - 32];
    uint16_t be_color = SWAP_BYTES(color);
    uint16_t be_bg = SWAP_BYTES(bg);

    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < 0 || py >= STATUS_BAR_H) continue;
        uint8_t line = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= STATUS_BAR_W) continue;
            s_status_bar_buf[py * STATUS_BAR_W + px] = (line & (0x80 >> col)) ? be_color : be_bg;
        }
    }
}

static void sb_draw_string(int x, int y, const char *str, uint16_t color, uint16_t bg)
{
    while (str && *str) {
        sb_draw_char(x, y, *str, color, bg);
        x += 8;
        str++;
    }
}

static void sb_draw_string_centered(int x, int y, int w, const char *str, uint16_t color, uint16_t bg)
{
    if (!str) return;
    int str_len = strlen(str);
    int total_w = str_len * 8;
    int start_x = x + (w - total_w) / 2;
    sb_draw_string(start_x, y, str, color, bg);
}

static void sb_draw_battery(int x, int y, int w, int h, int percentage, bool is_charging)
{
    if (percentage < 0) percentage = 0;
    if (percentage > 100) percentage = 100;

    int body_w = w - 3;
    uint16_t be_white = SWAP_BYTES(COLOR_WHITE);

    // Shell border
    for (int c = x; c < x + body_w; c++) {
        sb_set_pixel(c, y, be_white);
        sb_set_pixel(c, y + h - 1, be_white);
    }
    for (int r = y; r < y + h; r++) {
        sb_set_pixel(x, r, be_white);
        sb_set_pixel(x + body_w - 1, r, be_white);
    }

    // Terminal pip
    for (int r = y + 2; r < y + h - 2; r++) {
        for (int c = x + body_w; c < x + w; c++) {
            sb_set_pixel(c, r, be_white);
        }
    }

    // Inner fill
    int max_inner_w = body_w - 4;
    int inner_h = h - 4;
    int fill_w = (max_inner_w * percentage) / 100;

    uint16_t fill_color;
    if (percentage > 40) {
        fill_color = COLOR_GREEN;
    } else if (percentage >= 20) {
        fill_color = COLOR_YELLOW;
    } else {
        fill_color = COLOR_RED;
    }

    if (fill_w > 0) {
        sb_fill_rect(x + 2, y + 2, fill_w, inner_h, fill_color);
    }

    if (max_inner_w > fill_w) {
        sb_fill_rect(x + 2 + fill_w, y + 2, max_inner_w - fill_w, inner_h, 0x0861);
    }

    if (is_charging) {
        sb_fill_rect(x + body_w / 2 - 1, y + 1, 3, h - 2, COLOR_WHITE);
    }
}

static void sb_draw_wifi(int cx, int cy, bool connected)
{
    uint16_t col = connected ? COLOR_GREEN_ACCENT : COLOR_TEXT_DIM;
    sb_fill_rect(cx - 6, cy - 5, 13, 2, col);
    sb_fill_rect(cx - 4, cy - 2, 9, 2, col);
    sb_fill_rect(cx - 1, cy + 2, 3, 3, col);
}

static void sb_draw_music_icon(int cx, int cy, uint16_t color)
{
    sb_fill_rect(cx - 7, cy + 2, 5, 4, color);
    sb_fill_rect(cx + 2, cy, 5, 4, color);
    sb_fill_rect(cx - 3, cy - 6, 2, 9, color);
    sb_fill_rect(cx + 6, cy - 8, 2, 9, color);
    sb_fill_rect(cx - 3, cy - 8, 11, 2, color);
}

static void sb_draw_stream_dot(int cx, int cy)
{
    sb_fill_rect(cx - 3, cy - 3, 7, 7, COLOR_RED);
}

// ─── Main Public Status Bar Render Function ─────────────────
void app_common_draw_status_bar(bool force_redraw)
{
    // 1. Gather all current status values
    bool is_audio_playing = (audio_player_get_state() == AUDIO_STATE_PLAYING);
    bool is_streaming = http_stream_is_active();
    bool wifi_connected = wifi_service_is_connected();
    uint8_t bat_pct = battery_service_get_percentage();
    bool bat_charging = battery_service_is_charging();

    time_t now_sec;
    time(&now_sec);
    struct tm timeinfo;
    localtime_r(&now_sec, &timeinfo);

    char time_str[16];
    if (timeinfo.tm_year >= (2020 - 1900)) {
        strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
    } else {
        uint32_t total_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        int hours = (total_sec / 3600) % 24;
        int mins = (total_sec / 60) % 60;
        snprintf(time_str, sizeof(time_str), "%02d:%02d", hours, mins);
    }

    // 2. Dirty check: has anything changed?
    bool changed = (strcmp(time_str, s_last_time_str) != 0) ||
                   (wifi_connected != s_last_wifi_connected) ||
                   (bat_pct != s_last_bat_pct) ||
                   (bat_charging != s_last_bat_charging) ||
                   (is_audio_playing != s_last_audio_playing) ||
                   (is_streaming != s_last_stream_active);

    if (!force_redraw && !changed) {
        return; // Nothing changed, skip redraw completely! Zero CPU, zero flicker!
    }

    // 3. Render into off-screen buffer (instant RAM composition)
    sb_fill_rect(0, 0, STATUS_BAR_W, STATUS_BAR_H - 1, 0x0861);
    sb_fill_rect(0, STATUS_BAR_H - 1, STATUS_BAR_W, 1, 0x2124);

    // Left: Active service icons (only shown when service active)
    int service_x = 12;
    if (is_audio_playing) {
        sb_draw_music_icon(service_x, 10, COLOR_CYAN_ACCENT);
        service_x += 18;
    }
    if (is_streaming) {
        sb_draw_stream_dot(service_x, 10);
        service_x += 16;
    }

    // Center: Time
    sb_draw_string_centered(120, 3, 80, time_str, COLOR_WHITE, 0x0861);

    // Right: WiFi
    sb_draw_wifi(246, 11, wifi_connected);

    // Right: Battery
    sb_draw_battery(262, 6, 20, 10, bat_pct, bat_charging);

    // Right: Battery % text
    char bat_str[10];
    snprintf(bat_str, sizeof(bat_str), "%u%%", (unsigned int)bat_pct);
    uint16_t pct_col = (bat_pct < 20) ? COLOR_RED : COLOR_WHITE;
    sb_draw_string(286, 3, bat_str, pct_col, 0x0861);

    // 4. Atomic single DMA push to display (ZERO blink/flicker)
    display_draw_bitmap_block(0, 0, STATUS_BAR_W, STATUS_BAR_H, s_status_bar_buf);

    // 5. Update cached state
    snprintf(s_last_time_str, sizeof(s_last_time_str), "%s", time_str);
    s_last_wifi_connected = wifi_connected;
    s_last_bat_pct = bat_pct;
    s_last_bat_charging = bat_charging;
    s_last_audio_playing = is_audio_playing;
    s_last_stream_active = is_streaming;
}

bool app_common_is_back_pressed(int tx, int ty)
{
    return (tx <= 70 && ty <= 30);
}
