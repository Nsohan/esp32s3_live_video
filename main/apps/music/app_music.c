#include "app_music.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "display.h"
#include "display_gfx.h"
#include "audio_player.h"
#include "sdcard.h"

static const char *TAG = "app_music";

#define MAX_TRACKS 16
#define TRACK_NAME_MAX 64

typedef struct {
    char name[TRACK_NAME_MAX];
    char path[512];
} TrackItem;

static TrackItem s_playlist[MAX_TRACKS];
static int s_track_count = 0;
static int s_selected_idx = 0;
static int s_scroll_offset = 0;
static bool s_scanned = false;

static void scan_directory(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_track_count < MAX_TRACKS) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0)) {
            strncpy(s_playlist[s_track_count].name, entry->d_name, TRACK_NAME_MAX - 1);
            s_playlist[s_track_count].name[TRACK_NAME_MAX - 1] = '\0';
            snprintf(s_playlist[s_track_count].path, sizeof(s_playlist[s_track_count].path), "%s/%s", dir_path, entry->d_name);
            s_track_count++;
        }
    }
    closedir(dir);
}

void app_music_init(void)
{
    s_track_count = 0;
    s_selected_idx = 0;
    s_scroll_offset = 0;
    s_scanned = false;

    if (!sdcard_is_mounted()) {
        sdcard_init();
    }

    if (sdcard_is_mounted()) {
        scan_directory("/sdcard/sounds");
        scan_directory("/sdcard/music");
        s_scanned = true;
        ESP_LOGI(TAG, "Scanned %d tracks from SD card", s_track_count);
    }
}

static void draw_status_card(void)
{
    // Top Status & Volume Area (Y: 34 to 75)
    gfx_fill_round_rect(8, 34, 304, 42, 6, COLOR_CARD_BG);
    gfx_draw_round_rect(8, 34, 304, 42, 6, COLOR_CARD_BORDER);

    // Current Track Info
    const char *current_track = audio_player_get_current_track_name();
    audio_player_state_t state = audio_player_get_state();
    uint8_t vol = audio_player_get_volume();

    char track_display[64];
    snprintf(track_display, sizeof(track_display), "%.26s", current_track);
    gfx_draw_string(16, 42, track_display, COLOR_CYAN_ACCENT, COLOR_CARD_BG, 1);

    char state_str[48];
    snprintf(state_str, sizeof(state_str), "%s | Vol: %d%%",
             (state == AUDIO_STATE_PLAYING) ? "PLAYING" : (state == AUDIO_STATE_PAUSED) ? "PAUSED" : "IDLE",
             vol);
    gfx_draw_string(16, 58, state_str, COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
}

static void draw_track_list(void)
{
    // Track List or Speaker Test Panel (Y: 82 to 180)
    int visible_tracks = 4;
    int item_y = 82;

    if (s_track_count == 0) {
        // ─── Button 1: Test Speaker (Melody Chirp) ───
        gfx_fill_round_rect(8, 82, 304, 44, 8, COLOR_GREEN);
        gfx_draw_round_rect(8, 82, 304, 44, 8, COLOR_WHITE);
        gfx_draw_string_centered(8, 96, 304, ">> TEST SPEAKER (CHIRP) <<", COLOR_BLACK, COLOR_GREEN, 1);

        // ─── Button 2: Test Speaker (1kHz Pure Tone) ───
        gfx_fill_round_rect(8, 132, 304, 44, 8, 0xFD20); // Orange / Amber
        gfx_draw_round_rect(8, 132, 304, 44, 8, COLOR_WHITE);
        gfx_draw_string_centered(8, 146, 304, ">> PLAY 1000Hz BEEP TONE <<", COLOR_BLACK, 0xFD20, 1);
    } else {
        for (int i = 0; i < visible_tracks; i++) {
            int track_idx = s_scroll_offset + i;
            if (track_idx >= s_track_count) {
                gfx_fill_rect(8, item_y, 304, 22, COLOR_BG_DARK);
                item_y += 25;
                continue;
            }

            bool is_selected = (track_idx == s_selected_idx);
            uint16_t bg = is_selected ? 0x21A8 : COLOR_CARD_BG;
            uint16_t text_col = is_selected ? COLOR_WHITE : 0xCE59;

            gfx_fill_round_rect(8, item_y, 304, 22, 4, bg);
            if (is_selected) {
                gfx_draw_round_rect(8, item_y, 304, 22, 4, COLOR_CYAN_ACCENT);
            }

            char item_txt[64];
            snprintf(item_txt, sizeof(item_txt), "%2d. %.28s", track_idx + 1, s_playlist[track_idx].name);
            gfx_draw_string(14, item_y + 6, item_txt, text_col, bg, 1);

            item_y += 25;
        }
    }
}

static void draw_controls_bar(void)
{
    audio_player_state_t state = audio_player_get_state();

    // 1. Prev Button / Chirp
    gfx_fill_round_rect(8, 190, 42, 40, 6, COLOR_CARD_BG);
    gfx_draw_round_rect(8, 190, 42, 40, 6, COLOR_CARD_BORDER);
    gfx_draw_string_centered(8, 204, 42, "|<", COLOR_WHITE, COLOR_CARD_BG, 2);

    // 2. Play / Pause Button
    gfx_fill_round_rect(56, 190, 60, 40, 6, (state == AUDIO_STATE_PLAYING) ? COLOR_GREEN : COLOR_CYAN_ACCENT);
    gfx_draw_string_centered(56, 204, 60, (state == AUDIO_STATE_PLAYING) ? "||" : ">", COLOR_BLACK, (state == AUDIO_STATE_PLAYING) ? COLOR_GREEN : COLOR_CYAN_ACCENT, 2);

    // 3. Stop Button
    gfx_fill_round_rect(122, 190, 42, 40, 6, COLOR_CARD_BG);
    gfx_draw_round_rect(122, 190, 42, 40, 6, COLOR_CARD_BORDER);
    gfx_draw_string_centered(122, 204, 42, "[]", COLOR_RED_ACCENT, COLOR_CARD_BG, 2);

    // 4. Next Button
    gfx_fill_round_rect(170, 190, 42, 40, 6, COLOR_CARD_BG);
    gfx_draw_round_rect(170, 190, 42, 40, 6, COLOR_CARD_BORDER);
    gfx_draw_string_centered(170, 204, 42, ">|", COLOR_WHITE, COLOR_CARD_BG, 2);

    // 5. Vol Down [-]
    gfx_fill_round_rect(218, 190, 42, 40, 6, COLOR_CARD_BG);
    gfx_draw_round_rect(218, 190, 42, 40, 6, COLOR_CARD_BORDER);
    gfx_draw_string_centered(218, 204, 42, "-", COLOR_WHITE, COLOR_CARD_BG, 2);

    // 6. Vol Up [+]
    gfx_fill_round_rect(266, 190, 46, 40, 6, COLOR_CARD_BG);
    gfx_draw_round_rect(266, 190, 46, 40, 6, COLOR_CARD_BORDER);
    gfx_draw_string_centered(266, 204, 46, "+", COLOR_YELLOW, COLOR_CARD_BG, 2);
}

void app_music_draw(void)
{
    display_fill_screen(COLOR_BG_DARK);
    app_common_draw_header("SD MUSIC PLAYER");

    if (!s_scanned && sdcard_is_mounted()) {
        app_music_init();
    }

    draw_status_card();
    draw_track_list();
    draw_controls_bar();
}

bool app_music_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw)
{
    if (needs_redraw) *needs_redraw = false;

    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }

    // Touch on Track List or Speaker Test Buttons (Y: 80 to 180)
    if (ty >= 80 && ty <= 180) {
        if (s_track_count > 0) {
            int clicked_row = (ty - 82) / 25;
            int clicked_idx = s_scroll_offset + clicked_row;
            if (clicked_idx < s_track_count) {
                s_selected_idx = clicked_idx;
                audio_player_play_file(s_playlist[s_selected_idx].path);
                draw_track_list();
                draw_status_card();
                draw_controls_bar();
                return true;
            }
        } else {
            // Button 1: Test Speaker Chirp (82 - 128)
            if (ty >= 80 && ty <= 128) {
                ESP_LOGI(TAG, "Touch triggered Speaker Test (Chirp Melody)");
                audio_player_play_happy_sound();
                draw_status_card();
                draw_controls_bar();
                return true;
            }
            // Button 2: Test Speaker 1kHz Tone (130 - 180)
            if (ty >= 130 && ty <= 180) {
                ESP_LOGI(TAG, "Touch triggered Speaker Test (1000Hz Tone)");
                audio_player_play_test_tone();
                draw_status_card();
                draw_controls_bar();
                return true;
            }
        }
    }

    // Touch on Bottom Control Bar (Y: 188 to 236)
    if (ty >= 188 && ty <= 236) {
        // 1. Prev Track / UI click (8 to 50)
        if (tx >= 8 && tx <= 50) {
            if (s_track_count > 0) {
                s_selected_idx = (s_selected_idx - 1 + s_track_count) % s_track_count;
                if (s_selected_idx < s_scroll_offset) s_scroll_offset = s_selected_idx;
                audio_player_play_file(s_playlist[s_selected_idx].path);
                draw_track_list();
            } else {
                audio_player_play_ui_click();
            }
            draw_status_card();
            draw_controls_bar();
            return true;
        }

        // 2. Play / Pause (56 to 116)
        if (tx >= 56 && tx <= 116) {
            if (audio_player_get_state() == AUDIO_STATE_IDLE) {
                if (s_track_count > 0) {
                    audio_player_play_file(s_playlist[s_selected_idx].path);
                    draw_track_list();
                } else {
                    audio_player_play_happy_sound();
                }
            } else {
                audio_player_toggle_play_pause();
            }
            draw_status_card();
            draw_controls_bar();
            return true;
        }

        // 3. Stop (122 to 164)
        if (tx >= 122 && tx <= 164) {
            audio_player_stop();
            draw_status_card();
            draw_controls_bar();
            return true;
        }

        // 4. Next Track (170 to 212)
        if (tx >= 170 && tx <= 212) {
            if (s_track_count > 0) {
                s_selected_idx = (s_selected_idx + 1) % s_track_count;
                if (s_selected_idx >= s_scroll_offset + 4) s_scroll_offset = s_selected_idx - 3;
                audio_player_play_file(s_playlist[s_selected_idx].path);
                draw_track_list();
            } else {
                audio_player_play_test_tone();
            }
            draw_status_card();
            draw_controls_bar();
            return true;
        }

        // 5. Vol Down [-] (218 to 260)
        if (tx >= 218 && tx <= 260) {
            uint8_t cur_vol = audio_player_get_volume();
            if (cur_vol >= 10) audio_player_set_volume(cur_vol - 10);
            else audio_player_set_volume(0);
            audio_player_play_ui_click();
            draw_status_card();
            return true;
        }

        // 6. Vol Up [+] (266 to 312)
        if (tx >= 266 && tx <= 312) {
            uint8_t cur_vol = audio_player_get_volume();
            if (cur_vol <= 90) audio_player_set_volume(cur_vol + 10);
            else audio_player_set_volume(100);
            audio_player_play_happy_sound(); // Play sound effect feedback
            draw_status_card();
            return true;
        }
    }

    return false;
}
