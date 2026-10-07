#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_EVENT_ANY_ID (-1)
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    void (*callback)(void *, esp_event_base_t, int32_t, void *), void *arg,
    esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
    esp_event_handler_instance_t instance);
