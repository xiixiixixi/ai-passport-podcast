#include "podcast_bookmarks.h"

#include "nvs.h"
#include <string.h>

#define BOOKMARK_NAMESPACE "podcast_v1"
#define BOOKMARK_WIRE_SIZE 430U
#define BOOKMARK_VERSION 1U
#define BOOKMARK_CRC_OFFSET 426U

/* Explicit little-endian layout: no compiler padding or pointers are stored.
 * magic[4], version[2], length[2], sequence[8], segment[4], offset[8],
 * finished[1], reserved[3], show[24], episode[64], title[241], name[65], crc[4]. */
static size_t utf8_scalar_length(const unsigned char *p)
{
    unsigned char a = p[0];
    if (a < 0x80) return a ? 1 : 0;
    if (a < 0xc2 || a > 0xf4 || !p[1] || (p[1] & 0xc0) != 0x80) return 0;
    if (a < 0xe0) return 2;
    if ((a == 0xe0 && p[1] < 0xa0) || (a == 0xed && p[1] >= 0xa0) ||
        !p[2] || (p[2] & 0xc0) != 0x80) return 0;
    if (a < 0xf0) return 3;
    if ((a == 0xf0 && p[1] < 0x90) || (a == 0xf4 && p[1] >= 0x90) ||
        !p[3] || (p[3] & 0xc0) != 0x80) return 0;
    return 4;
}

static bool bounded_text_valid(const char *text, size_t capacity, bool required)
{
    size_t length = 0;
    while (length < capacity && text[length]) ++length;
    if (length == capacity || (required && !length)) return false;
    const char *end = text + length;
    const unsigned char *p = (const unsigned char *)text;
    while ((const char *)p < end) {
        size_t size = utf8_scalar_length(p);
        if (!size || size > (size_t)(end - (const char *)p)) return false;
        p += size;
    }
    return true;
}

bool podcast_bookmark_copy_text(char *destination, size_t capacity, const char *source)
{
    if (!destination || !capacity || !source) return false;
    size_t written = 0;
    const unsigned char *p = (const unsigned char *)source;
    while (*p) {
        size_t size = utf8_scalar_length(p);
        if (!size) {
            memset(destination, 0, capacity);
            return false;
        }
        if (written + size >= capacity) break;
        memcpy(destination + written, p, size);
        written += size;
        p += size;
    }
    memset(destination + written, 0, capacity - written);
    return true;
}

bool podcast_bookmark_valid(const podcast_bookmark_t *record)
{
    return record &&
        bounded_text_valid(record->show_id, sizeof(record->show_id), true) &&
        bounded_text_valid(record->episode_id, sizeof(record->episode_id), true) &&
        bounded_text_valid(record->title, sizeof(record->title), false) &&
        bounded_text_valid(record->name, sizeof(record->name), false) &&
        record->segment <= PODCAST_BOOKMARK_MAX_SEGMENT &&
        record->byte_offset <= PODCAST_BOOKMARK_MAX_BYTE_OFFSET &&
        (record->byte_offset & 1U) == 0;
}

static void put_le(uint8_t *out, uint64_t value, size_t size)
{
    for (size_t i = 0; i < size; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

static uint64_t get_le(const uint8_t *in, size_t size)
{
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= (uint64_t)in[i] << (8 * i);
    return value;
}

static uint32_t wire_crc(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static bool encode_record(const podcast_bookmark_t *record, uint64_t sequence,
                          uint8_t wire[BOOKMARK_WIRE_SIZE])
{
    if (!podcast_bookmark_valid(record) || !sequence) return false;
    memset(wire, 0, BOOKMARK_WIRE_SIZE);
    memcpy(wire, "PBK1", 4);
    put_le(wire + 4, BOOKMARK_VERSION, 2);
    put_le(wire + 6, BOOKMARK_WIRE_SIZE, 2);
    put_le(wire + 8, sequence, 8);
    put_le(wire + 16, record->segment, 4);
    put_le(wire + 20, record->byte_offset, 8);
    wire[28] = record->finished ? 1 : 0;
    memcpy(wire + 32, record->show_id, strlen(record->show_id));
    memcpy(wire + 56, record->episode_id, strlen(record->episode_id));
    memcpy(wire + 120, record->title, strlen(record->title));
    memcpy(wire + 361, record->name, strlen(record->name));
    put_le(wire + BOOKMARK_CRC_OFFSET, wire_crc(wire, BOOKMARK_CRC_OFFSET), 4);
    return true;
}

static bool decode_record(const uint8_t *wire, size_t size,
                          podcast_bookmark_t *record, uint64_t *sequence)
{
    if (size != BOOKMARK_WIRE_SIZE || memcmp(wire, "PBK1", 4) != 0 ||
        get_le(wire + 4, 2) != BOOKMARK_VERSION ||
        get_le(wire + 6, 2) != BOOKMARK_WIRE_SIZE || !get_le(wire + 8, 8) ||
        wire[28] > 1 || wire[29] || wire[30] || wire[31] ||
        get_le(wire + BOOKMARK_CRC_OFFSET, 4) != wire_crc(wire, BOOKMARK_CRC_OFFSET))
        return false;
    memset(record, 0, sizeof(*record));
    record->segment = (uint32_t)get_le(wire + 16, 4);
    record->byte_offset = get_le(wire + 20, 8);
    record->finished = wire[28] != 0;
    memcpy(record->show_id, wire + 32, sizeof(record->show_id));
    memcpy(record->episode_id, wire + 56, sizeof(record->episode_id));
    memcpy(record->title, wire + 120, sizeof(record->title));
    memcpy(record->name, wire + 361, sizeof(record->name));
    if (!podcast_bookmark_valid(record)) return false;
    *sequence = get_le(wire + 8, 8);
    return true;
}

static bool same_record(const podcast_bookmark_t *a, const podcast_bookmark_t *b)
{
    return a->segment == b->segment && a->byte_offset == b->byte_offset &&
        a->finished == b->finished && strcmp(a->show_id, b->show_id) == 0 &&
        strcmp(a->episode_id, b->episode_id) == 0 &&
        strcmp(a->title, b->title) == 0 && strcmp(a->name, b->name) == 0;
}

typedef struct {
    int free_slot, oldest_slot, matching_slot, newest_slot;
    uint64_t oldest_sequence, matching_sequence, newest_sequence;
    bool matching_equal, corrupted;
} bookmark_scan_t;

static void slot_key(unsigned slot, char key[8])
{
    memcpy(key, "slot00", 7);
    key[4] = (char)('0' + slot / 10);
    key[5] = (char)('0' + slot % 10);
}

static podcast_bookmark_result_t read_slot(nvs_handle_t handle, unsigned slot,
                                          podcast_bookmark_t *record, uint64_t *sequence)
{
    char key[8];
    slot_key(slot, key);
    size_t size = 0;
    esp_err_t error = nvs_get_blob(handle, key, NULL, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) return PODCAST_BOOKMARK_NOT_FOUND;
    if (error == ESP_ERR_NVS_TYPE_MISMATCH) return PODCAST_BOOKMARK_INVALID;
    if (error != ESP_OK) return PODCAST_BOOKMARK_STORAGE_ERROR;
    if (size != BOOKMARK_WIRE_SIZE) return PODCAST_BOOKMARK_INVALID;
    uint8_t wire[BOOKMARK_WIRE_SIZE];
    error = nvs_get_blob(handle, key, wire, &size);
    if (error != ESP_OK) return PODCAST_BOOKMARK_STORAGE_ERROR;
    return decode_record(wire, size, record, sequence) ? PODCAST_BOOKMARK_OK
                                                       : PODCAST_BOOKMARK_INVALID;
}

/* Reads only one slot at a time. At most 24 records, bounded key lengths and
 * no index of all titles/IDs in scarce device RAM. Full IDs select a record. */
static podcast_bookmark_result_t scan_slots(nvs_handle_t handle,
                                           const char *show_id, const char *episode_id,
                                           const podcast_bookmark_t *saving,
                                           bookmark_scan_t *scan,
                                           podcast_bookmark_t *selected)
{
    *scan = (bookmark_scan_t){.free_slot = -1, .oldest_slot = -1,
        .matching_slot = -1, .newest_slot = -1, .oldest_sequence = UINT64_MAX};
    for (unsigned slot = 0; slot < PODCAST_BOOKMARK_SLOTS; ++slot) {
        podcast_bookmark_t record;
        uint64_t sequence;
        podcast_bookmark_result_t result = read_slot(handle, slot, &record, &sequence);
        if (result == PODCAST_BOOKMARK_STORAGE_ERROR) return result;
        if (result != PODCAST_BOOKMARK_OK) {
            if (scan->free_slot < 0) scan->free_slot = (int)slot;
            if (result == PODCAST_BOOKMARK_INVALID) scan->corrupted = true;
            continue;
        }
        if (scan->newest_slot < 0 || sequence > scan->newest_sequence) {
            scan->newest_slot = (int)slot;
            scan->newest_sequence = sequence;
            if (!show_id && selected) *selected = record;
        }
        if (scan->oldest_slot < 0 || sequence < scan->oldest_sequence) {
            scan->oldest_slot = (int)slot;
            scan->oldest_sequence = sequence;
        }
        if (show_id && strcmp(show_id, record.show_id) == 0 &&
            strcmp(episode_id, record.episode_id) == 0 &&
            (scan->matching_slot < 0 || sequence > scan->matching_sequence)) {
            scan->matching_slot = (int)slot;
            scan->matching_sequence = sequence;
            scan->matching_equal = saving && same_record(saving, &record);
            if (selected) *selected = record;
        }
    }
    return PODCAST_BOOKMARK_OK;
}

static podcast_bookmark_result_t open_storage(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    esp_err_t error = nvs_open(BOOKMARK_NAMESPACE, mode, handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return PODCAST_BOOKMARK_NOT_FOUND;
    return error == ESP_OK ? PODCAST_BOOKMARK_OK : PODCAST_BOOKMARK_STORAGE_ERROR;
}

podcast_bookmark_result_t podcast_bookmarks_init(void)
{
    nvs_handle_t handle;
    podcast_bookmark_result_t result = open_storage(NVS_READONLY, &handle);
    if (result == PODCAST_BOOKMARK_OK) nvs_close(handle);
    /* An empty first boot is valid and creates no namespace until a save. */
    return result == PODCAST_BOOKMARK_NOT_FOUND ? PODCAST_BOOKMARK_OK : result;
}

static podcast_bookmark_result_t load_selected(const char *show_id, const char *episode_id,
                                              podcast_bookmark_t *record)
{
    if (!record) return PODCAST_BOOKMARK_INVALID;
    nvs_handle_t handle;
    podcast_bookmark_result_t result = open_storage(NVS_READONLY, &handle);
    if (result != PODCAST_BOOKMARK_OK) return result;
    bookmark_scan_t scan;
    podcast_bookmark_t selected;
    result = scan_slots(handle, show_id, episode_id, NULL, &scan, &selected);
    nvs_close(handle);
    if (result != PODCAST_BOOKMARK_OK) return result;
    if ((show_id ? scan.matching_slot : scan.newest_slot) < 0)
        return !show_id && scan.corrupted ? PODCAST_BOOKMARK_INVALID
                                          : PODCAST_BOOKMARK_NOT_FOUND;
    *record = selected;
    return PODCAST_BOOKMARK_OK;
}

podcast_bookmark_result_t podcast_bookmarks_load_recent(podcast_bookmark_t *record)
{
    return load_selected(NULL, NULL, record);
}

podcast_bookmark_result_t podcast_bookmarks_find(const char *show_id,
                                               const char *episode_id,
                                               podcast_bookmark_t *record)
{
    if (!show_id || !episode_id ||
        !bounded_text_valid(show_id, PODCAST_BOOKMARK_SHOW_CAP, true) ||
        !bounded_text_valid(episode_id, PODCAST_BOOKMARK_EPISODE_CAP, true))
        return PODCAST_BOOKMARK_INVALID;
    return load_selected(show_id, episode_id, record);
}

podcast_bookmark_result_t podcast_bookmarks_save(const podcast_bookmark_t *record)
{
    if (!podcast_bookmark_valid(record)) return PODCAST_BOOKMARK_INVALID;
    nvs_handle_t handle;
    podcast_bookmark_result_t result = open_storage(NVS_READWRITE, &handle);
    if (result != PODCAST_BOOKMARK_OK) return result;
    bookmark_scan_t scan;
    result = scan_slots(handle, record->show_id, record->episode_id, record, &scan, NULL);
    if (result != PODCAST_BOOKMARK_OK) { nvs_close(handle); return result; }
    if (scan.matching_equal && scan.matching_sequence == scan.newest_sequence) {
        nvs_close(handle);
        return PODCAST_BOOKMARK_OK;
    }
    if (scan.newest_sequence == UINT64_MAX) {
        nvs_close(handle);
        return PODCAST_BOOKMARK_STORAGE_ERROR;
    }
    int slot = scan.matching_slot >= 0 ? scan.matching_slot :
        scan.free_slot >= 0 ? scan.free_slot : scan.oldest_slot;
    if (slot < 0) { nvs_close(handle); return PODCAST_BOOKMARK_STORAGE_ERROR; }
    uint8_t wire[BOOKMARK_WIRE_SIZE];
    if (!encode_record(record, scan.newest_sequence + 1, wire)) {
        nvs_close(handle);
        return PODCAST_BOOKMARK_INVALID;
    }
    char key[8];
    slot_key((unsigned)slot, key);
    esp_err_t error = nvs_set_blob(handle, key, wire, sizeof(wire));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK ? PODCAST_BOOKMARK_OK : PODCAST_BOOKMARK_STORAGE_ERROR;
}

podcast_bookmark_result_t podcast_bookmarks_load_volume(uint8_t *volume)
{
    if (!volume) return PODCAST_BOOKMARK_INVALID;
    nvs_handle_t handle;
    podcast_bookmark_result_t result = open_storage(NVS_READONLY, &handle);
    if (result != PODCAST_BOOKMARK_OK) return result;
    uint8_t saved;
    esp_err_t error = nvs_get_u8(handle, "volume", &saved);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return PODCAST_BOOKMARK_NOT_FOUND;
    if (error == ESP_ERR_NVS_TYPE_MISMATCH) return PODCAST_BOOKMARK_INVALID;
    if (error != ESP_OK) return PODCAST_BOOKMARK_STORAGE_ERROR;
    if (saved > 100) return PODCAST_BOOKMARK_INVALID;
    *volume = saved;
    return PODCAST_BOOKMARK_OK;
}

podcast_bookmark_result_t podcast_bookmarks_save_volume(uint8_t volume)
{
    if (volume > 100) return PODCAST_BOOKMARK_INVALID;
    nvs_handle_t handle;
    podcast_bookmark_result_t result = open_storage(NVS_READWRITE, &handle);
    if (result != PODCAST_BOOKMARK_OK) return result;
    uint8_t saved;
    esp_err_t error = nvs_get_u8(handle, "volume", &saved);
    if (error == ESP_OK && saved == volume) { nvs_close(handle); return PODCAST_BOOKMARK_OK; }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND && error != ESP_ERR_NVS_TYPE_MISMATCH) {
        nvs_close(handle);
        return PODCAST_BOOKMARK_STORAGE_ERROR;
    }
    error = nvs_set_u8(handle, "volume", volume);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK ? PODCAST_BOOKMARK_OK : PODCAST_BOOKMARK_STORAGE_ERROR;
}

podcast_bookmark_result_t podcast_bookmarks_load_slot(unsigned slot,podcast_bookmark_t *record)
{
    if(slot>=PODCAST_BOOKMARK_SLOTS||!record)return PODCAST_BOOKMARK_INVALID;
    nvs_handle_t h;esp_err_t e=nvs_open(BOOKMARK_NAMESPACE,NVS_READONLY,&h);
    if(e==ESP_ERR_NVS_NOT_FOUND)return PODCAST_BOOKMARK_NOT_FOUND;
    if(e!=ESP_OK)return PODCAST_BOOKMARK_STORAGE_ERROR;
    podcast_bookmark_t fresh;uint64_t sequence;
    podcast_bookmark_result_t result=read_slot(h,slot,&fresh,&sequence);nvs_close(h);
    if(result==PODCAST_BOOKMARK_OK)*record=fresh;
    return result;
}
