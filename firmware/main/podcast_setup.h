#pragma once
#include "bsp_button.h"
#include <stdbool.h>

/* Boot-only temporary WPA2 AP setup. Called before audio/player startup.
 * On save or cancellation with an existing config, cleans up then restarts.
 * Button callback merely sets cancellation intent. No Flash erase is used. */
bool podcast_setup_active(void);
void podcast_setup_key(bsp_btn_t button, bsp_btn_ev_t event);
bool podcast_setup_run(void);
