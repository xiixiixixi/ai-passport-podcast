#include "podcast_config.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static podcast_config_t saved;
static bool ready;
static char authorization[72];
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t c = UINT32_MAX;
    while (n--) { c ^= *p++; for (unsigned j = 0; j < 8; ++j) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U))); }
    return ~c;
}
static bool bounded(const char *s, size_t cap)
{ if (!s) return false; for (size_t i = 0; i < cap; ++i) if (!s[i]) return true; return false; }
static bool device_identity(const char *s)
{
    if (!bounded(s, 24) || strlen(s) != 23 || strncmp(s, "device-", 7)) return false;
    for (unsigned i = 7; i < 23; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
static bool token_valid(const char *s)
{
    if (!bounded(s, 65) || strlen(s) != 64) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
bool podcast_config_network(const char *ssid, const char *password)
{
    if (!bounded(ssid, 33) || !*ssid || !bounded(password, 65)) return false;
    size_t n = strlen(password);
    if (n && (n < 8 || n > 63)) return false;
    for (const unsigned char *p = (const unsigned char *)ssid; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    for (const unsigned char *p = (const unsigned char *)password; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}
bool podcast_config_server(char *out, size_t cap, const char *input)
{
    if (!input || !*input) return false;
    size_t n = strlen(input);
    if (n > PODCAST_SERVER_MAX || n + 1 > cap) return false;
    size_t prefix = !strncasecmp(input, "https://", 8) ? 8 : !strncasecmp(input, "http://", 7) ? 7 : 0;
    if (!prefix) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)input[i] <= 32 || (unsigned char)input[i] >= 127 || strchr("?#@\\", input[i])) return false;
    while (n > prefix && input[n - 1] == '/') --n;
    if (n <= prefix) return false;
    const char *host = input + prefix;
    size_t host_n = n - prefix;
    /* Root origin only; a reverse proxy may expose the app at its root. */
    for (size_t i = 0; i < host_n; ++i) if (host[i] == '/') return false;
    const char *colon = memchr(host, ':', host_n);
    size_t name_n = colon ? (size_t)(colon - host) : host_n;
    if (!name_n || name_n > 253) return false;
    if ((name_n == 9 && !strncasecmp(host, "localhost", 9)) ||
        (name_n > 10 && !strncasecmp(host + name_n - 10, ".localhost", 10)) ||
        (name_n >= 4 && !memcmp(host, "127.", 4)) ||
        (name_n == 7 && !memcmp(host, "0.0.0.0", 7))) return false;
    for (size_t i = 0; i < name_n; ++i)
        if (!((host[i] >= 'a' && host[i] <= 'z') || (host[i] >= 'A' && host[i] <= 'Z') ||
              (host[i] >= '0' && host[i] <= '9') || host[i] == '-' || host[i] == '.')) return false;
    if (host[0] == '.' || host[0] == '-' || host[name_n - 1] == '.' || host[name_n - 1] == '-') return false;
    if (colon) {
        unsigned port = 0; size_t start = (size_t)(colon - host) + 1;
        if (start == host_n || host_n - start > 5) return false;
        for (size_t i = start; i < host_n; ++i) {
            if (host[i] < '0' || host[i] > '9') return false;
            port = port * 10 + (unsigned)(host[i] - '0');
        }
        if (!port || port > 65535) return false;
    }
    memcpy(out, prefix == 8 ? "https://" : "http://", prefix);
    memcpy(out + prefix, input + prefix, n - prefix); out[n] = 0; return true;
}
bool podcast_config_code(const char *code)
{
    if (!code || strlen(code) != 6) return false;
    for (unsigned i = 0; i < 6; ++i) if (code[i] < '0' || code[i] > '9') return false;
    return true;
}
bool podcast_config_claim(podcast_config_t *cfg, const char *device_id, const char *token)
{
    if (!cfg || !device_identity(device_id) || !token_valid(token)) return false;
    memcpy(cfg->device_id, device_id, 24); memcpy(cfg->token, token, 65); return true;
}
bool podcast_config_valid(const podcast_config_t *cfg)
{
    char origin[PODCAST_SERVER_MAX + 1];
    return cfg && podcast_config_network(cfg->ssid, cfg->password) &&
        bounded(cfg->server, sizeof(cfg->server)) &&
        podcast_config_server(origin, sizeof(origin), cfg->server) && !strcmp(origin, cfg->server) &&
        device_identity(cfg->device_id) && token_valid(cfg->token);
}
void podcast_config_encode(const podcast_config_t *cfg, uint8_t out[PODCAST_CONFIG_WIRE_SIZE])
{
    memset(out, 0, PODCAST_CONFIG_WIRE_SIZE); memcpy(out, "PCF1", 4);
    memcpy(out + 4, cfg->ssid, 33); memcpy(out + 37, cfg->password, 65);
    memcpy(out + 102, cfg->server, 161); memcpy(out + 263, cfg->device_id, 24);
    memcpy(out + 287, cfg->token, 65);
    uint32_t sum = crc(out, PODCAST_CONFIG_WIRE_SIZE - 4);
    for (unsigned i = 0; i < 4; ++i) out[PODCAST_CONFIG_WIRE_SIZE - 4 + i] = (uint8_t)(sum >> (8 * i));
}
bool podcast_config_decode(podcast_config_t *cfg, const uint8_t *wire, size_t size)
{
    if (!cfg || !wire || size != PODCAST_CONFIG_WIRE_SIZE || memcmp(wire, "PCF1", 4)) return false;
    uint32_t sum = 0; for (unsigned i = 0; i < 4; ++i) sum |= (uint32_t)wire[size - 4 + i] << (8 * i);
    if (sum != crc(wire, size - 4)) return false;
    podcast_config_t decoded = {0};
    memcpy(decoded.ssid, wire + 4, 33); memcpy(decoded.password, wire + 37, 65);
    memcpy(decoded.server, wire + 102, 161); memcpy(decoded.device_id, wire + 263, 24);
    memcpy(decoded.token, wire + 287, 65);
    if (!podcast_config_valid(&decoded)) return false;
    *cfg = decoded; return true;
}
podcast_config_result_t podcast_config_init(void)
{
    ready = false; memset(&saved, 0, sizeof(saved)); memset(authorization, 0, sizeof(authorization));
    nvs_handle_t h; esp_err_t e = nvs_open("podcast_cfg", NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return PODCAST_CONFIG_MISSING;
    if (e != ESP_OK) return PODCAST_CONFIG_STORAGE_ERROR;
    uint8_t wire[PODCAST_CONFIG_WIRE_SIZE]; size_t n = sizeof(wire);
    e = nvs_get_blob(h, "connection", wire, &n); nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return PODCAST_CONFIG_MISSING;
    if (e != ESP_OK) return PODCAST_CONFIG_STORAGE_ERROR;
    if (!podcast_config_decode(&saved, wire, n)) return PODCAST_CONFIG_INVALID;
    ready = true; snprintf(authorization, sizeof(authorization), "Bearer %s", saved.token);
    memset(wire, 0, sizeof(wire)); return PODCAST_CONFIG_OK;
}
const podcast_config_t *podcast_config_get(void) { return ready ? &saved : NULL; }
const char *podcast_config_authorization(void) { return ready ? authorization : NULL; }
bool podcast_config_save(const podcast_config_t *candidate)
{
    if (!podcast_config_valid(candidate)) return false;
    nvs_handle_t h; if (nvs_open("podcast_cfg", NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t wire[PODCAST_CONFIG_WIRE_SIZE]; podcast_config_encode(candidate, wire);
    esp_err_t e = nvs_set_blob(h, "connection", wire, sizeof(wire));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); memset(wire, 0, sizeof(wire));
    if (e != ESP_OK) return false;
    saved = *candidate; ready = true; snprintf(authorization, sizeof(authorization), "Bearer %s", saved.token); return true;
}
bool podcast_config_request_setup(void)
{
    nvs_handle_t h; if (nvs_open("podcast_cfg", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, "setup", 1); if (e == ESP_OK) e = nvs_commit(h); nvs_close(h); return e == ESP_OK;
}
bool podcast_config_take_setup_request(void)
{
    nvs_handle_t h; if (nvs_open("podcast_cfg", NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t requested = 0; esp_err_t e = nvs_get_u8(h, "setup", &requested);
    if (e == ESP_OK && requested) { e = nvs_set_u8(h, "setup", 0); if (e == ESP_OK) e = nvs_commit(h); }
    nvs_close(h); return e == ESP_OK && requested;
}
