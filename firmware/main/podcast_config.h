#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PODCAST_SERVER_MAX 160
#define PODCAST_CONFIG_WIRE_SIZE 384
typedef struct {
    char ssid[33], password[65], server[PODCAST_SERVER_MAX + 1];
    char device_id[24], token[65];
} podcast_config_t;
typedef enum { PODCAST_CONFIG_OK, PODCAST_CONFIG_MISSING,
               PODCAST_CONFIG_INVALID, PODCAST_CONFIG_STORAGE_ERROR } podcast_config_result_t;

/* Explicit configuration only: no compiled network, address, or credentials.
 * All storage uses podcast_cfg; listening namespaces are never erased. */
podcast_config_result_t podcast_config_init(void);
const podcast_config_t *podcast_config_get(void);
const char *podcast_config_authorization(void);
bool podcast_config_save(const podcast_config_t *candidate);
bool podcast_config_request_setup(void);
bool podcast_config_take_setup_request(void);
bool podcast_config_valid(const podcast_config_t *candidate);
bool podcast_config_server(char *out, size_t cap, const char *input);
bool podcast_config_network(const char *ssid, const char *password);
bool podcast_config_claim(podcast_config_t *candidate, const char *device_id, const char *token);
bool podcast_config_code(const char *code);
void podcast_config_encode(const podcast_config_t *cfg, uint8_t out[PODCAST_CONFIG_WIRE_SIZE]);
bool podcast_config_decode(podcast_config_t *cfg, const uint8_t *wire, size_t size);
