#pragma once
#include <stdbool.h>

/* Caller owns the LVGL lock. Setup screen has no timers or input handlers. */
bool podcast_setup_screen_create(const char *hotspot, const char *password, bool can_cancel);
void podcast_setup_screen_message(const char *message);
void podcast_setup_screen_delete(void);
