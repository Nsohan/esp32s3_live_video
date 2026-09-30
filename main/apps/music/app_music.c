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

#define MAX_TRACKS 32
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

// Cached render states for zero-flicker differential updates
static int s_rendered_selected_idx = -1;
static int s_rendered_scroll_offset = -1;
static audio_player_state_t s_rendered_state = (audio_player_state_t)-1;
static audio_player_state_t s_rendered_btn_state = (audio_player_state_t)-1;
static uint8_t s_rendered_volume = 255;
static char s_rendered_track_name[64] = "";

static void scan_directory(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_track_count < MAX_TRACKS) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0)) {
            // Check if already in playlist
            bool exists = false;
            for (int i = 0; i < s_track_count; i++) {
                if (strcmp(s_playlist[i].name, entry->d_name) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                size_t k = 0;
                while (k + 1 < TRACK_NAME_MAX && entry->d_name[k] != '\0') {
                    s_playlist[s_track_count].name[k] = entry->d_name[k];
                    k++;
                }
                s_playlist[s_track_count].name[k] = '\0';
                snprintf(s_playlist[s_track_count].path, sizeof(s_playlist[s_track_count].path), "%s/%s", dir_path, entry->d_name);
                s_track_count++;
            }
        }
    }
    closedir(dir);
}

void app_music_play_next(void)
{
    if (s_track_count <= 0) return;
    s_selected_idx = (s_selected_idx + 1) % s_track_count;
    if (s_selected_idx >= s_scroll_offset + 4) {
        s_scroll_offset = s_selected_idx - 3;
    } else if (s_selected_idx < s_scroll_offset) {
        s_scroll_offset = s_selected_idx;
    }
    audio_player_play_file(s_playlist[s_selected_idx].path);
}

void app_music_play_prev(void)
{
    if (s_track_count <= 0) return;
    s_selected_idx = (s_selected_idx - 1 + s_track_count) % s_track_count;
    if (s_selected_idx < s_scroll_offset) {
        s_scroll_offset = s_selected_idx;
    } else if (s_selected_idx >= s_scroll_offset + 4) {
        s_scroll_offset = s_selected_idx - 3;
    }
    audio_player_play_file(s_playlist[s_selected_idx].path);
}

static void on_track_completed(void)
{
    const char *current_track = audio_player_get_current_track_name();
    if (!current_track || s_track_count <= 0) return;

    // Only auto-advance if the audio that finished actually belongs to the music playlist
    if (strcmp(current_track, s_playlist[s_selected_idx].name) != 0) {
        ESP_LOGI(TAG, "Audio '%s' completed, but not active music playlist track -> ignoring auto-advance", current_track);
        return;
    }

    ESP_LOGI(TAG, "Track '%s' finished naturally -> Auto playing next track...", current_track);
    app_music_play_next();
}

void app_music_init(void)
{
    audio_player_set_finish_callback(on_track_completed);

    if (s_scanned && s_track_count > 0) {
        return;
    }

    s_track_count = 0;
    s_selected_idx = 0;
    s_scroll_offset = 0;
    s_scanned = false;

    if (!sdcard_is_mounted()) {
        sdcard_init();
    }

    if (sdcard_is_mounted()) {
        scan_directory("/sdcard/sound");
        scan_directory("/sdcard/sounds");
        scan_directory("/sdcard/music");
        scan_directory("/sdcard");
        s_scanned = true;
        ESP_LOGI(TAG, "Scanned %d tracks from SD card", s_track_count);
    }
}

// ─── Targeted Zero-Flicker Partial Render Helpers ──────────

static void update_status_card(bool full_redraw)
{
    const char *current_track = audio_player_get_current_track_name();
    audio_player_state_t state = audio_player_get_state();
    uint8_t vol = audio_player_get_volume();

    bool changed = (state != s_rendered_state) ||
                   (vol != s_rendered_volume) ||
                   (strncmp(current_track, s_rendered_track_name, sizeof(s_rendered_track_name)) != 0);

    if (!full_redraw && !changed) {
        return;
    }

    if (full_redraw) {
        gfx_fill_round_rect(8, 34, 304, 42, 6, COLOR_CARD_BG);
        gfx_draw_round_rect(8, 34, 304, 42, 6, COLOR_CARD_BORDER);
    }

    // Space-padded strings cleanly overwrite character cells with zero background flash
    char track_display[32];
    snprintf(track_display, sizeof(track_display), "%-26.26s", current_track);
    gfx_draw_string(16, 42, track_display, COLOR_CYAN_ACCENT, COLOR_CARD_BG, 1);

    char temp_state[48];
    snprintf(temp_state, sizeof(temp_state), "%s | Vol: %d%%",
             (state == AUDIO_STATE_PLAYING) ? "PLAYING" : (state == AUDIO_STATE_PAUSED) ? "PAUSED" : "IDLE",
             vol);
    char state_str[32];
    snprintf(state_str, sizeof(state_str), "%-26.26s", temp_state);
    gfx_draw_string(16, 58, state_str, COLOR_TEXT_DIM, COLOR_CARD_BG, 1);

    s_rendered_state = state;
    s_rendered_volume = vol;
    strncpy(s_rendered_track_name, current_track, sizeof(s_rendered_track_name) - 1);
}

static void draw_single_track_row(int visible_slot)
{
    int track_idx = s_scroll_offset + visible_slot;
    int item_y = 82 + visible_slot * 25;

    if (track_idx >= s_track_count) {
        gfx_fill_rect(8, item_y, 304, 22, COLOR_BG_DARK);
        return;
    }

    bool is_selected = (track_idx == s_selected_idx);
    uint16_t bg = is_selected ? 0x21A8 : COLOR_CARD_BG;
    uint16_t text_col = is_selected ? COLOR_WHITE : 0xCE59;

    gfx_fill_round_rect(8, item_y, 304, 22, 4, bg);
    if (is_selected) {
        gfx_draw_round_rect(8, item_y, 304, 22, 4, COLOR_CYAN_ACCENT);
    }

    char raw_txt[64];
    snprintf(raw_txt, sizeof(raw_txt), "%2d. %s", track_idx + 1, s_playlist[track_idx].name);
    char item_txt[36];
    snprintf(item_txt, sizeof(item_txt), "%-28.28s", raw_txt);
    gfx_draw_string(14, item_y + 6, item_txt, text_col, bg, 1);
}

static void update_track_list(bool full_redraw)
{
    if (s_track_count == 0) {
        if (full_redraw) {
            // Button 1: Test Speaker (Melody Chirp)
            gfx_fill_round_rect(8, 82, 304, 44, 8, COLOR_GREEN);
            gfx_draw_round_rect(8, 82, 304, 44, 8, COLOR_WHITE);
            gfx_draw_string_centered(8, 96, 304, ">> TEST SPEAKER (CHIRP) <<", COLOR_BLACK, COLOR_GREEN, 1);

            // Button 2: Test Speaker (1kHz Pure Tone)
            gfx_fill_round_rect(8, 132, 304, 44, 8, 0xFD20); // Orange / Amber
            gfx_draw_round_rect(8, 132, 304, 44, 8, COLOR_WHITE);
            gfx_draw_string_centered(8, 146, 304, ">> PLAY 1000Hz BEEP TONE <<", COLOR_BLACK, 0xFD20, 1);
        }
        return;
    }

    bool scroll_changed = (s_scroll_offset != s_rendered_scroll_offset);
    bool selection_changed = (s_selected_idx != s_rendered_selected_idx);

    if (!full_redraw && !scroll_changed && !selection_changed) {
        return;
    }

    if (full_redraw || scroll_changed) {
        // Redraw all 4 visible slots
        for (int slot = 0; slot < 4; slot++) {
            draw_single_track_row(slot);
        }
    } else if (selection_changed) {
        // Only repaint the previous selected row and the new selected row
        for (int slot = 0; slot < 4; slot++) {
            int track_idx = s_scroll_offset + slot;
            if (track_idx == s_selected_idx || track_idx == s_rendered_selected_idx) {
                draw_single_track_row(slot);
            }
        }
    }

    s_rendered_selected_idx = s_selected_idx;
    s_rendered_scroll_offset = s_scroll_offset;
}

static void update_play_pause_button(bool force)
{
    audio_player_state_t state = audio_player_get_state();
    if (!force && state == s_rendered_btn_state) {
        return; // Guard against redrawing every tick -> eliminates continuous blinking!
    }

    uint16_t btn_color = (state == AUDIO_STATE_PLAYING) ? COLOR_GREEN : COLOR_CYAN_ACCENT;
    const char *btn_icon = (state == AUDIO_STATE_PLAYING) ? "||" : ">";

    gfx_fill_round_rect(56, 190, 60, 40, 6, btn_color);
    gfx_draw_string_centered(56, 204, 60, btn_icon, COLOR_BLACK, btn_color, 2);

    s_rendered_btn_state = state;
}

static void draw_controls_bar(bool full_redraw)
{
    if (full_redraw) {
        // 1. Prev Button / Chirp
        gfx_fill_round_rect(8, 190, 42, 40, 6, COLOR_CARD_BG);
        gfx_draw_round_rect(8, 190, 42, 40, 6, COLOR_CARD_BORDER);
        gfx_draw_string_centered(8, 204, 42, "|<", COLOR_WHITE, COLOR_CARD_BG, 2);

        // 2. Play / Pause Button
        update_play_pause_button(true);

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
    } else {
        update_play_pause_button(false);
    }
}

void app_music_draw(void)
{
    display_fill_screen(COLOR_BG_DARK);
    app_common_draw_header("SD MUSIC PLAYER");

    if (!s_scanned && sdcard_is_mounted()) {
        app_music_init();
    }

    // Invalidate cached state so full view renders cleanly once
    s_rendered_selected_idx = -1;
    s_rendered_scroll_offset = -1;
    s_rendered_state = (audio_player_state_t)-1;
    s_rendered_btn_state = (audio_player_state_t)-1;
    s_rendered_volume = 255;
    s_rendered_track_name[0] = '\0';

    update_status_card(true);
    update_track_list(true);
    draw_controls_bar(true);
}

void app_music_update(void)
{
    // Differential updates — only redraws dirty rectangles if value changed
    update_status_card(false);
    update_track_list(false);
    update_play_pause_button(false);
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
            if (clicked_row >= 0 && clicked_idx >= 0 && clicked_idx < s_track_count) {
                s_selected_idx = clicked_idx;
                audio_player_play_file(s_playlist[s_selected_idx].path);
                app_music_update();
                return true;
            }
        } else {
            // Button 1: Test Speaker Chirp (82 - 128)
            if (ty >= 80 && ty <= 128) {
                ESP_LOGI(TAG, "Touch triggered Speaker Test (Chirp Melody)");
                audio_player_play_happy_sound();
                app_music_update();
                return true;
            }
            // Button 2: Test Speaker 1kHz Tone (130 - 180)
            if (ty >= 130 && ty <= 180) {
                ESP_LOGI(TAG, "Touch triggered Speaker Test (1000Hz Tone)");
                audio_player_play_test_tone();
                app_music_update();
                return true;
            }
        }
    }

    // Touch on Bottom Control Bar (Y: 188 to 236)
    if (ty >= 188 && ty <= 236) {
        // 1. Prev Track / UI click (8 to 50)
        if (tx >= 8 && tx <= 50) {
            if (s_track_count > 0) {
                app_music_play_prev();
            } else {
                audio_player_play_ui_click();
            }
            app_music_update();
            return true;
        }

        // 2. Play / Pause (56 to 116)
        if (tx >= 56 && tx <= 116) {
            if (audio_player_get_state() == AUDIO_STATE_IDLE) {
                if (s_track_count > 0) {
                    audio_player_play_file(s_playlist[s_selected_idx].path);
                } else {
                    audio_player_play_happy_sound();
                }
            } else {
                audio_player_toggle_play_pause();
            }
            app_music_update();
            return true;
        }

        // 3. Stop (122 to 164)
        if (tx >= 122 && tx <= 164) {
            audio_player_stop();
            app_music_update();
            return true;
        }

        // 4. Next Track (170 to 212)
        if (tx >= 170 && tx <= 212) {
            if (s_track_count > 0) {
                app_music_play_next();
            } else {
                audio_player_play_test_tone();
            }
            app_music_update();
            return true;
        }

        // 5. Vol Down [-] (218 to 260)
        if (tx >= 218 && tx <= 260) {
            uint8_t cur_vol = audio_player_get_volume();
            if (cur_vol >= 10) audio_player_set_volume(cur_vol - 10);
            else audio_player_set_volume(0);
            audio_player_play_ui_click();
            app_music_update();
            return true;
        }

        // 6. Vol Up [+] (266 to 312)
        if (tx >= 266 && tx <= 312) {
            uint8_t cur_vol = audio_player_get_volume();
            if (cur_vol <= 90) audio_player_set_volume(cur_vol + 10);
            else audio_player_set_volume(100);
            audio_player_play_happy_sound();
            app_music_update();
            return true;
        }
    }

    return false;
}
