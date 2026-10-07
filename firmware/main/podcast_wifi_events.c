#include "podcast_wifi_events.h"
#include "esp_wifi.h"
#include "esp_netif.h"

esp_err_t podcast_wifi_events_register(podcast_wifi_events_t *events,
    void (*callback)(void *, esp_event_base_t, int32_t, void *))
{
    esp_err_t result;
    if (!events->wifi) {
        result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                      callback, NULL, &events->wifi);
        if (result != ESP_OK) return result;
    }
    if (!events->ip) {
        result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                      callback, NULL, &events->ip);
        if (result != ESP_OK) {
            if (esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       events->wifi) == ESP_OK)
                events->wifi = NULL;
            return result;
        }
    }
    return ESP_OK;
}
