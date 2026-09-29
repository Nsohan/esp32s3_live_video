#pragma once

#include <stdbool.h>
#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_music_init(void);
void app_music_draw(void);
void app_music_update(void);
void app_music_play_next(void);
void app_music_play_prev(void);
bool app_music_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw);

#ifdef __cplusplus
}
#endif
