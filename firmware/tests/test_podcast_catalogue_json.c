/* Build with the same cJSON source used by ESP-IDF. This is a host memory
 * bound check, not a network request or evidence of device availability. */
#include "cJSON.h"
#include "podcast_catalogue_limits.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union { max_align_t alignment; size_t bytes; } allocation_header_t;
static size_t allocated, peak;
static void *tracked_malloc(size_t bytes)
{
    allocation_header_t *header = malloc(sizeof(*header) + bytes);
    assert(header);
    header->bytes = bytes; allocated += bytes;
    if (allocated > peak) peak = allocated;
    return header + 1;
}
static void tracked_free(void *p)
{
    if (!p) return;
    allocation_header_t *header = (allocation_header_t *)p - 1;
    assert(allocated >= header->bytes); allocated -= header->bytes;
    free(header);
}
int main(void)
{
    char payload[PODCAST_MAX_JSON_BYTES + 1];
    size_t length = (size_t)snprintf(payload, sizeof(payload), "{\"s\":[");
    assert(PODCAST_MAX_SHOWS >= 32 && PODCAST_SHOW_PAGE_SIZE==8);
    for (unsigned i = 0; i < PODCAST_SHOW_PAGE_SIZE; ++i) {
        length += (size_t)snprintf(payload + length, sizeof(payload) - length,
                                  "%s{\"i\":\"%023u\",\"n\":\"", i ? "," : "", i);
        /* The longest stored Chinese name is 21 UTF-8 characters. Even a
         * producer that escapes every character uses only 126 wire bytes. */
        for (unsigned j = 0; j < 21; ++j) {
            assert(length + 6 < sizeof(payload));
            memcpy(payload + length, "\\u4e2d", 6); length += 6;
        }
        length += (size_t)snprintf(payload+length,sizeof(payload)-length,"\",\"d\":\"2026-10-05\",\"p\":1791123456,\"e\":4096,\"l\":604800000000,\"c\":4096,\"u\":4096}");
    }
    memcpy(payload + length, "]}", 3); length += 2;
    assert(length < PODCAST_MAX_JSON_BYTES);
    cJSON_Hooks hooks = { tracked_malloc, tracked_free };
    cJSON_InitHooks(&hooks);
    cJSON *json = cJSON_Parse(payload); assert(json);
    cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "s");
    assert(cJSON_GetArraySize(array) == PODCAST_SHOW_PAGE_SIZE);
    cJSON *item;
    cJSON_ArrayForEach(item, array) {
        const char *name = cJSON_GetObjectItemCaseSensitive(item, "n")->valuestring;
        assert(strlen(name) == 63);
    }
    /* Host pointers are larger than ESP32-C3 pointers. cJSON requested bytes
     * plus the wire copy still fit this conservative 16 KiB transient bound. */
    assert(peak + length + 1 < 16 * 1024);
    printf("8-show page of 32 with dates/stats: escaped response=%zu bytes, cJSON peak=%zu, combined=%zu (<16KiB)\n",
           length, peak, peak + length + 1);
    cJSON_Delete(json); assert(allocated == 0); cJSON_InitHooks(NULL);
    return 0;
}
