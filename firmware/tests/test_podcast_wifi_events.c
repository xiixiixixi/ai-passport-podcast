#include "podcast_wifi_events.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned registrations, unregistrations, fail_at, live;
static bool fail_unregister;
static struct { bool active; const char *base; int32_t id; } instances[2];

esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    void (*callback)(void *, esp_event_base_t, int32_t, void *), void *arg,
    esp_event_handler_instance_t *instance)
{
    assert(callback && !arg && instance && !*instance);
    if (++registrations == fail_at) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < 2; ++i) {
        if (instances[i].active) continue;
        instances[i].active = true;
        instances[i].base = base;
        instances[i].id = id;
        *instance = &instances[i];
        ++live;
        return ESP_OK;
    }
    assert(false); // A reconnect must never allocate a third live callback.
    return ESP_ERR_NO_MEM;
}

esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
    esp_event_handler_instance_t instance)
{
    ++unregistrations;
    for (size_t i = 0; i < 2; ++i) {
        if (instance != &instances[i]) continue;
        assert(instances[i].active && strcmp(base, instances[i].base) == 0 && id == instances[i].id);
        if (fail_unregister) return ESP_ERR_NO_MEM;
        instances[i].active = false;
        --live;
        return ESP_OK;
    }
    assert(false);
    return ESP_ERR_NO_MEM;
}

static void callback(void *arg, esp_event_base_t base, int32_t id, void *data)
{ (void)arg; (void)base; (void)id; (void)data; }

static void cleanup(podcast_wifi_events_t *events)
{
    if (events->wifi) assert(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, events->wifi) == ESP_OK);
    if (events->ip) assert(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, events->ip) == ESP_OK);
    *events = (podcast_wifi_events_t){0};
    assert(live == 0);
}

int main(void)
{
    podcast_wifi_events_t events = {0};
    assert(podcast_wifi_events_register(&events, callback) == ESP_OK);
    esp_event_handler_instance_t wifi = events.wifi, ip = events.ip;
    for (unsigned reconnect = 0; reconnect < 1000; ++reconnect)
        assert(podcast_wifi_events_register(&events, callback) == ESP_OK);
    assert(registrations == 2 && unregistrations == 0 && live == 2);
    assert(events.wifi == wifi && events.ip == ip);
    cleanup(&events);

    registrations = unregistrations = 0; fail_at = 1;
    assert(podcast_wifi_events_register(&events, callback) == ESP_ERR_NO_MEM);
    assert(live == 0 && !events.wifi && !events.ip && unregistrations == 0);
    fail_at = 0;
    assert(podcast_wifi_events_register(&events, callback) == ESP_OK);
    cleanup(&events);

    registrations = unregistrations = 0; fail_at = 2;
    assert(podcast_wifi_events_register(&events, callback) == ESP_ERR_NO_MEM);
    assert(live == 0 && !events.wifi && !events.ip && unregistrations == 1);
    fail_at = 0;
    assert(podcast_wifi_events_register(&events, callback) == ESP_OK);
    cleanup(&events);

    registrations = unregistrations = 0; fail_at = 2; fail_unregister = true;
    assert(podcast_wifi_events_register(&events, callback) == ESP_ERR_NO_MEM);
    assert(live == 1 && events.wifi && !events.ip);
    wifi = events.wifi;
    fail_at = 0; fail_unregister = false;
    assert(podcast_wifi_events_register(&events, callback) == ESP_OK);
    assert(live == 2 && events.wifi == wifi && registrations == 3);
    cleanup(&events);
    puts("Podcast Wi-Fi: repeated reconnects and registration/rollback failures PASS");
    return 0;
}
