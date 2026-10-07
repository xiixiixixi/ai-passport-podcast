#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/podcast_config.c"

static uint8_t disk[PODCAST_CONFIG_WIRE_SIZE], staged[PODCAST_CONFIG_WIRE_SIZE];
static bool present, staged_present, fail_open, fail_set, fail_commit;
static bool setup_present; static uint8_t setup_value, staged_setup;
static unsigned handles, writes; static nvs_open_mode_t mode;
esp_err_t nvs_open(const char *name, nvs_open_mode_t m, nvs_handle_t *h)
{
    assert(!strcmp(name, "podcast_cfg") && !handles);
    if (fail_open) return ESP_FAIL;
    if (!present && !setup_present && m == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    mode = m; *h = 1; handles++; memcpy(staged, disk, sizeof(disk));
    staged_present = present; staged_setup = setup_value; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h == 1 && handles == 1); handles--; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n)
{
    assert(h == 1 && handles && !strcmp(key, "connection"));
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    assert(*n >= sizeof(disk)); memcpy(out, disk, sizeof(disk)); *n = sizeof(disk); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t n)
{
    assert(h == 1 && handles && mode == NVS_READWRITE && !strcmp(key, "connection") && n == sizeof(disk));
    if (fail_set) return ESP_FAIL;
    memcpy(staged, in, n); staged_present = true; writes++; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{ assert(h == 1 && handles && !strcmp(key, "setup")); if (!setup_present) return ESP_ERR_NVS_NOT_FOUND; *out = setup_value; return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{ assert(h == 1 && handles && mode == NVS_READWRITE && !strcmp(key, "setup")); if (fail_set) return ESP_FAIL; staged_setup = value; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == 1 && handles); if (fail_commit) return ESP_FAIL;
    memcpy(disk, staged, sizeof(disk)); present = staged_present;
    setup_value = staged_setup; setup_present = true; return ESP_OK;
}
static podcast_config_t fixture(void)
{
    podcast_config_t c = {.ssid="test-network", .password="test-only-password", .server="http://relay.test:8899"};
    assert(podcast_config_claim(&c, "device-1234567890abcdef", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    return c;
}
int main(void)
{
    assert(podcast_config_init() == PODCAST_CONFIG_MISSING && !podcast_config_get() && !podcast_config_authorization());
    const char *bad[] = {"", "relay.test", "ftp://relay.test", "http://localhost:8899", "http://127.0.0.1", "http://0.0.0.0", "http://user:pass@relay.test", "https://relay.test/path", "https://relay.test/?x=y", "https://relay.test/#secret", "http://relay.test:0", "http://relay.test:65536", "http://relay.test:abc", "http://relay.test\\evil", "http://relay.test\n", "http://[::1]"};
    char url[PODCAST_SERVER_MAX + 1];
    for (unsigned i=0; i<sizeof(bad)/sizeof(*bad); i++) assert(!podcast_config_server(url, sizeof(url), bad[i]));
    assert(podcast_config_server(url,sizeof(url),"https://relay.example.com:443///") && !strcmp(url,"https://relay.example.com:443"));
    assert(podcast_config_server(url,sizeof(url),"HTTPS://relay.example.com") && !strcmp(url,"https://relay.example.com"));
    assert(!podcast_config_server(url,sizeof(url),"http://LOCALHOST:8899"));
    assert(podcast_config_network("中文网络", "") && podcast_config_network("test", "12345678"));
    assert(!podcast_config_network("", "12345678") && !podcast_config_network("test", "short") && !podcast_config_network("test\n", "12345678"));
    assert(podcast_config_code("012345") && !podcast_config_code("12345") && !podcast_config_code("12a456"));
    podcast_config_t c = fixture(), decoded = {0}; uint8_t wire[PODCAST_CONFIG_WIRE_SIZE];
    podcast_config_encode(&c, wire); assert(podcast_config_decode(&decoded, wire, sizeof(wire)) && !memcmp(&c, &decoded, sizeof(c)));
    wire[10] ^= 1; assert(!podcast_config_decode(&decoded,wire,sizeof(wire))); wire[10] ^= 1;
    assert(!podcast_config_decode(&decoded,wire,sizeof(wire)-1));
    assert(!podcast_config_claim(&decoded,"device-1234567890ABCDEF",c.token));
    assert(!podcast_config_claim(&decoded,c.device_id,"too-short"));
    assert(podcast_config_save(&c) && writes == 1 && podcast_config_get());
    assert(!strcmp(podcast_config_authorization(),"Bearer aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    memset(&saved,0,sizeof(saved)); ready=false;
    assert(podcast_config_init()==PODCAST_CONFIG_OK && !strcmp(podcast_config_get()->device_id,c.device_id));
    podcast_config_t newer=c; strcpy(newer.server,"https://new-relay.example.com");
    fail_set=true; assert(!podcast_config_save(&newer)); fail_set=false;
    assert(!strcmp(podcast_config_get()->server,c.server));
    fail_commit=true; assert(!podcast_config_save(&newer)); fail_commit=false;
    assert(podcast_config_init()==PODCAST_CONFIG_OK && !strcmp(podcast_config_get()->server,c.server));
    assert(podcast_config_request_setup()); assert(podcast_config_take_setup_request()); assert(!podcast_config_take_setup_request());
    assert(podcast_config_init()==PODCAST_CONFIG_OK && !strcmp(podcast_config_get()->server,c.server));
    fail_open=true; assert(!podcast_config_save(&newer) && !podcast_config_request_setup()); fail_open=false;
    assert(!handles);
    puts("Configuration: strict network/origin/identity/token bounds, durable versioned reboot, corruption refusal, save failures retain old settings, setup request consumed once, listening namespaces untouched PASS");
}
