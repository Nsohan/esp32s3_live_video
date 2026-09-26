#include "app_touch_test.h"
#include <stdio.h>
#include "esp_log.h"
#include "display.h"
#include "display_gfx.h"

static const char *TAG = "app_touch_test";

#define CAL_GRID_COLS 6
#define CAL_GRID_ROWS 4
#define TOTAL_CAL_BLOCKS (CAL_GRID_COLS * CAL_GRID_ROWS)

static int s_active_block = -1;
static int s_last_hit_x = -1;
static int s_last_hit_y = -1;
static uint16_t s_last_raw_rx = 0;
static uint16_t s_last_raw_ry = 0;

static void get_cal_block_rect(int col, int row, int *x, int *y, int *w, int *h)
{
    *x = 6 + col * 52;
    *y = 32 + row * 52;
    *w = 48;
    *h = 48;
}

static void draw_cal_block(int num, int col, int row, bool is_hit)
{
    int bx, by, bw, bh;
    get_cal_block_rect(col, row, &bx, &by, &bw, &bh);

    uint16_t bg = is_hit ? COLOR_YELLOW : 0x18E3;
    uint16_t fg = is_hit ? COLOR_BLACK  : COLOR_WHITE;
    uint16_t border = is_hit ? COLOR_WHITE : 0x39E7;

    gfx_fill_round_rect(bx, by, bw, bh, 5, bg);
    gfx_draw_round_rect(bx, by, bw, bh, 5, border);

    char num_str[16];
    snprintf(num_str, sizeof(num_str), "[%d]", num);
    gfx_draw_string_centered(bx, by + (is_hit ? 8 : 16), bw, num_str, fg, bg, 1);

    if (is_hit) {
        gfx_draw_string_centered(bx, by + 28, bw, "HIT", COLOR_RED, bg, 1);
    }
}

void app_touch_test_reset(void)
{
    s_active_block = -1;
    s_last_hit_x = -1;
    s_last_hit_y = -1;
    s_last_raw_rx = 0;
    s_last_raw_ry = 0;
}

void app_touch_test_draw(void)
{
    display_fill_screen(0x0000);

    // Header bar
    gfx_fill_rect(0, 0, 320, 26, 0x10A2);
    gfx_fill_round_rect(4, 3, 58, 20, 4, 0x2124);
    gfx_draw_icon(14, 13, ICON_BACK, COLOR_WHITE);
    gfx_draw_string(24, 8, "BACK", COLOR_WHITE, 0x2124, 1);

    char header_msg[64];
    if (s_active_block > 0) {
        snprintf(header_msg, sizeof(header_msg), "HIT #%d  X:%d Y:%d", s_active_block, s_last_hit_x, s_last_hit_y);
    } else {
        snprintf(header_msg, sizeof(header_msg), "TAP ANY NUMBERED BLOCK");
    }
    gfx_draw_string_centered(70, 8, 240, header_msg, COLOR_CYAN, 0x10A2, 1);
    gfx_fill_rect(0, 26, 320, 1, 0x31A6);

    // Draw all 24 blocks (6 columns x 4 rows)
    for (int row = 0; row < CAL_GRID_ROWS; row++) {
        for (int col = 0; col < CAL_GRID_COLS; col++) {
            int num = row * CAL_GRID_COLS + col + 1;
            draw_cal_block(num, col, row, (num == s_active_block));
        }
    }
}

static void update_calibrate_hit(int tx, int ty, uint16_t rx, uint16_t ry)
{
    s_last_hit_x = tx;
    s_last_hit_y = ty;
    s_last_raw_rx = rx;
    s_last_raw_ry = ry;

    int col = (tx - 6) / 52;
    int row = (ty - 32) / 52;

    if (col < 0) col = 0;
    if (col >= CAL_GRID_COLS) col = CAL_GRID_COLS - 1;
    if (row < 0) row = 0;
    if (row >= CAL_GRID_ROWS) row = CAL_GRID_ROWS - 1;

    int new_block = row * CAL_GRID_COLS + col + 1;

    ESP_LOGI(TAG, ">>> TOUCH CALIBRATE: Hit Block #[%d] | Screen(X=%d, Y=%d) | Raw(rx=%u, ry=%u)",
             new_block, tx, ty, rx, ry);

    // Unhighlight previous block
    if (s_active_block > 0 && s_active_block != new_block) {
        int prev_idx = s_active_block - 1;
        draw_cal_block(s_active_block, prev_idx % CAL_GRID_COLS, prev_idx / CAL_GRID_COLS, false);
    }

    // Highlight new block
    s_active_block = new_block;
    draw_cal_block(s_active_block, col, row, true);

    // Update Header Bar info
    gfx_fill_rect(70, 0, 248, 25, 0x10A2);
    char header_msg[64];
    snprintf(header_msg, sizeof(header_msg), "HIT #%d X:%d Y:%d (R:%u,%u)", s_active_block, tx, ty, rx, ry);
    gfx_draw_string_centered(65, 8, 250, header_msg, COLOR_YELLOW, 0x10A2, 1);
}

bool app_touch_test_handle_touch(int tx, int ty, uint16_t rx, uint16_t ry, AppState *next_state)
{
    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        return true;
    }

    update_calibrate_hit(tx, ty, rx, ry);
    return true;
}
