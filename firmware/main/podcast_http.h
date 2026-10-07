#pragma once
#include "podcast_config.h"
#ifndef PODCAST_PLAYER_TEST
#include "esp_http_client.h"
#endif
#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#include <time.h>
#include <string.h>
#endif

/* Token remains a header, including audio HEAD/GET. Never redirect an
 * authenticated request to another origin. HTTPS always verifies normal CAs. */
static inline esp_http_client_handle_t podcast_http_client(esp_http_client_config_t config)
{
    const char *auth = podcast_config_authorization();
    if (!auth) return NULL;
#ifdef ESP_PLATFORM
    /* Certificate dates require a real clock, supplied by SNTP after Wi-Fi.
     * Do not weaken TLS verification while the clock is unavailable. */
    if (!strncmp(config.url, "https://", 8) && time(NULL) < 1735689600) return NULL;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
#endif
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client && esp_http_client_set_header(client, "Authorization", auth) != ESP_OK) {
        esp_http_client_cleanup(client); return NULL;
    }
    return client;
}
