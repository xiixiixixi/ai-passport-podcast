#pragma once

#include "esp_event.h"

typedef struct {
    esp_event_handler_instance_t wifi;
    esp_event_handler_instance_t ip;
} podcast_wifi_events_t;

/* Persist across reconnects. A failed second registration rolls back the first;
 * if unregister also fails, retain its live handle and reuse it on retry. */
esp_err_t podcast_wifi_events_register(podcast_wifi_events_t *events,
    void (*callback)(void *, esp_event_base_t, int32_t, void *));
