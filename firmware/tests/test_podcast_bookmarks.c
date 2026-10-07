#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Exercise the production codec, slot scan and save/load logic with in-memory
 * NVS. No device dependencies or writes to the user's real settings. */
#include "../main/podcast_bookmarks.c"

static struct {
    uint8_t bytes[BOOKMARK_WIRE_SIZE];
    size_t size;
    bool present;
} slots[PODCAST_BOOKMARK_SLOTS];
static bool namespace_present, volume_present;
static uint8_t volume_value;
static unsigned open_handles, open_attempts, commits, blob_writes, volume_writes;
static bool fail_open, fail_read, fail_write, fail_commit;
static bool fail_payload_read, fail_blob_type;
static nvs_open_mode_t opened_mode;

static unsigned key_slot(const char *key)
{
    unsigned slot;
    char rest;
    assert(strlen(key) <= 15);
    assert(sscanf(key, "slot%u%c", &slot, &rest) == 1);
    assert(slot < PODCAST_BOOKMARK_SLOTS);
    return slot;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "podcast_v1") == 0 && open_handles == 0);
    ++open_attempts;
    if (fail_open) return ESP_FAIL;
    if (!namespace_present && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) namespace_present = true;
    opened_mode = mode;
    ++open_handles;
    *handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1 && open_handles == 1);
    --open_handles;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    assert(handle == 1 && open_handles == 1);
    if (fail_read) return ESP_FAIL;
    if (fail_blob_type) return ESP_ERR_NVS_TYPE_MISMATCH;
    unsigned slot = key_slot(key);
    if (!slots[slot].present) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *size = slots[slot].size; return ESP_OK; }
    if (fail_payload_read) return ESP_FAIL;
    if (*size < slots[slot].size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, slots[slot].bytes, slots[slot].size);
    *size = slots[slot].size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *bytes, size_t size)
{
    assert(handle == 1 && open_handles == 1 && opened_mode == NVS_READWRITE);
    if (fail_write) return ESP_FAIL;
    unsigned slot = key_slot(key);
    assert(size <= sizeof(slots[slot].bytes));
    memcpy(slots[slot].bytes, bytes, size);
    slots[slot].size = size;
    slots[slot].present = true;
    ++blob_writes;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    assert(handle == 1 && open_handles == 1 && strcmp(key, "volume") == 0);
    if (fail_read) return ESP_FAIL;
    if (!volume_present) return ESP_ERR_NVS_NOT_FOUND;
    *out = volume_value;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 1 && open_handles == 1 && opened_mode == NVS_READWRITE);
    assert(strcmp(key, "volume") == 0);
    if (fail_write) return ESP_FAIL;
    volume_present = true;
    volume_value = value;
    ++volume_writes;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && open_handles == 1 && opened_mode == NVS_READWRITE);
    if (fail_commit) return ESP_FAIL;
    ++commits;
    return ESP_OK;
}

static void reset_storage(void)
{
    assert(open_handles == 0);
    memset(slots, 0, sizeof(slots));
    namespace_present = volume_present = false;
    volume_value = 0;
    commits = blob_writes = volume_writes = 0;
    open_attempts = 0;
    fail_open = fail_read = fail_write = fail_commit = false;
    fail_payload_read = fail_blob_type = false;
}

typedef struct {
    uint8_t slot_bytes[sizeof(slots)];
    bool namespace_present, volume_present;
    uint8_t volume_value;
    unsigned commits, blob_writes, volume_writes;
} storage_snapshot_t;

static void snapshot_storage(storage_snapshot_t *snapshot)
{
    memcpy(snapshot->slot_bytes, slots, sizeof(slots));
    snapshot->namespace_present = namespace_present;
    snapshot->volume_present = volume_present;
    snapshot->volume_value = volume_value;
    snapshot->commits = commits;
    snapshot->blob_writes = blob_writes;
    snapshot->volume_writes = volume_writes;
}

static void assert_storage_unchanged(const storage_snapshot_t *snapshot)
{
    assert(memcmp(snapshot->slot_bytes, slots, sizeof(slots)) == 0);
    assert(snapshot->namespace_present == namespace_present);
    assert(snapshot->volume_present == volume_present);
    assert(snapshot->volume_value == volume_value);
    assert(snapshot->commits == commits);
    assert(snapshot->blob_writes == blob_writes);
    assert(snapshot->volume_writes == volume_writes);
    assert(open_handles == 0);
}

static podcast_bookmark_t sample(unsigned episode)
{
    podcast_bookmark_t record = {0};
    strcpy(record.show_id, "show_full_id");
    (void)snprintf(record.episode_id, sizeof(record.episode_id), "episode_%u", episode);
    assert(podcast_bookmark_copy_text(record.title, sizeof(record.title), "这一集的标题"));
    assert(podcast_bookmark_copy_text(record.name, sizeof(record.name), "节目名字"));
    record.segment = episode;
    record.byte_offset = episode * 32000U;
    return record;
}

static void refresh_crc(uint8_t *wire)
{
    put_le(wire + BOOKMARK_CRC_OFFSET, wire_crc(wire, BOOKMARK_CRC_OFFSET), 4);
}

static void test_validation_and_utf8(void)
{
    podcast_bookmark_t record = sample(1);
    assert(podcast_bookmark_valid(&record));
    record.byte_offset = 1;
    assert(!podcast_bookmark_valid(&record));
    record.byte_offset = (uint64_t)UINT32_MAX + 1;
    assert(!podcast_bookmark_valid(&record));
    record.byte_offset = 32000;
    record.segment = PODCAST_BOOKMARK_MAX_SEGMENT + 1;
    assert(!podcast_bookmark_valid(&record));
    record = sample(1);
    memset(record.show_id, 's', sizeof(record.show_id));
    assert(!podcast_bookmark_valid(&record));
    record = sample(1);
    record.episode_id[0] = 0;
    assert(!podcast_bookmark_valid(&record));
    char text[5];
    assert(podcast_bookmark_copy_text(text, sizeof(text), "中午"));
    assert(strcmp(text, "中") == 0 && text[4] == 0);
    assert(podcast_bookmark_copy_text(text, 3, "中午") && text[0] == 0);
    assert(podcast_bookmark_copy_text(text, sizeof(text), "A😀Z"));
    assert(strcmp(text, "A") == 0);
    assert(!podcast_bookmark_copy_text(text, sizeof(text), "\xc0\x80"));
    assert(!podcast_bookmark_copy_text(text, sizeof(text), "\xed\xa0\x80"));
    assert(!podcast_bookmark_copy_text(text, sizeof(text), "\xf4\x90\x80\x80"));
    assert(!podcast_bookmark_copy_text(text, sizeof(text), "\xe4\xb8"));
    record = sample(1);
    strcpy(record.title, "\xf0\x80\x80\x80");
    assert(!podcast_bookmark_valid(&record));
    assert(podcast_bookmarks_find("", "episode", &record) == PODCAST_BOOKMARK_INVALID);
}

static void test_codec_rejects_corruption(void)
{
    podcast_bookmark_t original = sample(2), decoded;
    uint8_t wire[BOOKMARK_WIRE_SIZE], changed[BOOKMARK_WIRE_SIZE];
    uint64_t sequence;
    assert(encode_record(&original, 7, wire));
    assert(decode_record(wire, sizeof(wire), &decoded, &sequence));
    assert(sequence == 7 && same_record(&original, &decoded));
    assert(!decode_record(wire, sizeof(wire) - 1, &decoded, &sequence));
    const size_t corruption_offsets[] = {0, 4, 6, 8, 16, 20, 28, 32, 56, 120, 361, 426};
    for (size_t i = 0; i < sizeof(corruption_offsets) / sizeof(corruption_offsets[0]); ++i) {
        memcpy(changed, wire, sizeof(wire));
        changed[corruption_offsets[i]] ^= 0x40;
        assert(!decode_record(changed, sizeof(changed), &decoded, &sequence));
    }
    /* Even a valid CRC cannot authorize incompatible or impossible records. */
    memcpy(changed, wire, sizeof(wire)); changed[4] = 2; refresh_crc(changed);
    assert(!decode_record(changed, sizeof(changed), &decoded, &sequence));
    memcpy(changed, wire, sizeof(wire)); memset(changed + 32, 'x', 24); refresh_crc(changed);
    assert(!decode_record(changed, sizeof(changed), &decoded, &sequence));
    memcpy(changed, wire, sizeof(wire)); changed[20] |= 1; refresh_crc(changed);
    assert(!decode_record(changed, sizeof(changed), &decoded, &sequence));
    memcpy(changed, wire, sizeof(wire)); put_le(changed + 16, PODCAST_BOOKMARK_MAX_SEGMENT + 1, 4); refresh_crc(changed);
    assert(!decode_record(changed, sizeof(changed), &decoded, &sequence));
    memcpy(changed, wire, sizeof(wire)); changed[28] = 2; refresh_crc(changed);
    assert(!decode_record(changed, sizeof(changed), &decoded, &sequence));
}

static void test_recent_lru_and_wear(void)
{
    reset_storage();
    podcast_bookmark_t record = sample(0), loaded = sample(99), unchanged = loaded;
    assert(podcast_bookmarks_init() == PODCAST_BOOKMARK_OK && !namespace_present);
    assert(podcast_bookmarks_load_recent(&loaded) == PODCAST_BOOKMARK_NOT_FOUND);
    assert(same_record(&loaded, &unchanged));
    for (unsigned i = 0; i < PODCAST_BOOKMARK_SLOTS; ++i) {
        record = sample(i);
        assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK);
    }
    assert(blob_writes == PODCAST_BOOKMARK_SLOTS && commits == PODCAST_BOOKMARK_SLOTS);
    assert(podcast_bookmarks_load_recent(&loaded) == PODCAST_BOOKMARK_OK);
    podcast_bookmark_t newest = sample(PODCAST_BOOKMARK_SLOTS - 1);
    assert(strcmp(loaded.episode_id, newest.episode_id) == 0);
    for (unsigned repeat = 0; repeat < 20; ++repeat)
        assert(podcast_bookmarks_save(&loaded) == PODCAST_BOOKMARK_OK);
    assert(blob_writes == PODCAST_BOOKMARK_SLOTS && commits == PODCAST_BOOKMARK_SLOTS);
    record = sample(0);
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK); // Makes old entry recent.
    record = sample(PODCAST_BOOKMARK_SLOTS);
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK); // Evicts episode 1.
    assert(podcast_bookmarks_find("show_full_id", "episode_0", &loaded) == PODCAST_BOOKMARK_OK);
    assert(podcast_bookmarks_find("show_full_id", "episode_1", &loaded) == PODCAST_BOOKMARK_NOT_FOUND);
    assert(podcast_bookmarks_find("show_full_id", "episode_2", &loaded) == PODCAST_BOOKMARK_OK);
    record = sample(PODCAST_BOOKMARK_SLOTS); record.finished = true;
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK);
    assert(podcast_bookmarks_load_recent(&loaded) == PODCAST_BOOKMARK_OK && loaded.finished);
    assert(podcast_bookmarks_find("show_full_id_extra", record.episode_id, &loaded) == PODCAST_BOOKMARK_NOT_FOUND);
    record = sample(PODCAST_BOOKMARK_SLOTS); strcpy(record.show_id, "another_full_id");
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK);
    assert(podcast_bookmarks_find("show_full_id", record.episode_id, &loaded) == PODCAST_BOOKMARK_OK && loaded.finished);
    assert(podcast_bookmarks_find("another_full_id", record.episode_id, &loaded) == PODCAST_BOOKMARK_OK && !loaded.finished);
    assert(open_handles == 0);
}

static void test_corrupt_storage_and_errors(void)
{
    reset_storage();
    podcast_bookmark_t record = sample(1), loaded = sample(99), unchanged = loaded;
    fail_open = true;
    assert(podcast_bookmarks_init() == PODCAST_BOOKMARK_STORAGE_ERROR);
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_STORAGE_ERROR);
    fail_open = false;
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK);
    fail_read = true;
    assert(podcast_bookmarks_load_recent(&loaded) == PODCAST_BOOKMARK_STORAGE_ERROR);
    assert(same_record(&loaded, &unchanged));
    unsigned writes_before = blob_writes;
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_STORAGE_ERROR);
    assert(blob_writes == writes_before);
    fail_read = false;
    slots[0].bytes[56] ^= 1;
    assert(podcast_bookmarks_load_recent(&loaded) == PODCAST_BOOKMARK_INVALID);
    assert(same_record(&loaded, &unchanged));
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK); // Reuses corrupt slot.
    slots[0].size = 65536; // Size check occurs before reading into fixed buffer.
    assert(podcast_bookmarks_load_recent(&loaded) == PODCAST_BOOKMARK_INVALID);
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK);
    record.byte_offset += 2;
    fail_write = true;
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_STORAGE_ERROR);
    fail_write = false;
    fail_commit = true;
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_STORAGE_ERROR);
    fail_commit = false;
    assert(open_handles == 0);
}

static void test_slot_export_is_read_only(void)
{
    reset_storage();
    for (unsigned slot = 0; slot < PODCAST_BOOKMARK_SLOTS; ++slot) {
        podcast_bookmark_t record = sample(slot);
        assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK);
    }
    /* Physical order differs from recency after an early slot is updated. */
    podcast_bookmark_t newest = sample(0), loaded;
    newest.finished = true;
    newest.byte_offset = 64000;
    assert(podcast_bookmarks_save(&newest) == PODCAST_BOOKMARK_OK);
    assert(podcast_bookmarks_save_volume(37) == PODCAST_BOOKMARK_OK);
    assert(podcast_bookmarks_load_recent(&loaded) == PODCAST_BOOKMARK_OK);
    assert(same_record(&loaded, &newest));
    storage_snapshot_t snapshot;
    snapshot_storage(&snapshot);

    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        for (unsigned slot = 0; slot < PODCAST_BOOKMARK_SLOTS; ++slot) {
            podcast_bookmark_t expected = slot == 0 ? newest : sample(slot);
            assert(podcast_bookmarks_load_slot(slot, &loaded) == PODCAST_BOOKMARK_OK);
            assert(same_record(&loaded, &expected));
            assert(opened_mode == NVS_READONLY);
            assert_storage_unchanged(&snapshot);
        }
        assert(podcast_bookmarks_load_recent(&loaded) == PODCAST_BOOKMARK_OK);
        assert(same_record(&loaded, &newest));
        assert_storage_unchanged(&snapshot);
    }
    /* The preserved sequences still select the original oldest victim. */
    podcast_bookmark_t added = sample(PODCAST_BOOKMARK_SLOTS);
    assert(podcast_bookmarks_save(&added) == PODCAST_BOOKMARK_OK);
    assert(podcast_bookmarks_find("show_full_id", "episode_1", &loaded) == PODCAST_BOOKMARK_NOT_FOUND);
    assert(podcast_bookmarks_find("show_full_id", "episode_0", &loaded) == PODCAST_BOOKMARK_OK);
    assert(same_record(&loaded, &newest));
    assert(open_handles == 0);
}

static void test_slot_export_boundaries(void)
{
    reset_storage();
    podcast_bookmark_t loaded = sample(99);
    uint8_t output_before[sizeof(loaded)];
    memcpy(output_before, &loaded, sizeof(loaded));
    storage_snapshot_t snapshot;
    snapshot_storage(&snapshot);
    assert(podcast_bookmarks_load_slot(0, &loaded) == PODCAST_BOOKMARK_NOT_FOUND);
    assert(memcmp(output_before, &loaded, sizeof(loaded)) == 0);
    assert_storage_unchanged(&snapshot);

    /* Invalid arguments must be rejected even when storage cannot open. */
    fail_open = true;
    unsigned opens_before = open_attempts;
    assert(podcast_bookmarks_load_slot(PODCAST_BOOKMARK_SLOTS, &loaded) == PODCAST_BOOKMARK_INVALID);
    assert(podcast_bookmarks_load_slot(UINT_MAX, &loaded) == PODCAST_BOOKMARK_INVALID);
    assert(podcast_bookmarks_load_slot(0, NULL) == PODCAST_BOOKMARK_INVALID);
    assert(open_attempts == opens_before);
    assert(memcmp(output_before, &loaded, sizeof(loaded)) == 0);
    assert_storage_unchanged(&snapshot);
    fail_open = false;

    podcast_bookmark_t record = sample(1);
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK);
    snapshot_storage(&snapshot);
    assert(podcast_bookmarks_load_slot(PODCAST_BOOKMARK_SLOTS - 1, &loaded) == PODCAST_BOOKMARK_NOT_FOUND);
    assert(opened_mode == NVS_READONLY);
    assert(memcmp(output_before, &loaded, sizeof(loaded)) == 0);
    assert_storage_unchanged(&snapshot);
}

static void test_slot_export_failures_preserve_storage(void)
{
    reset_storage();
    podcast_bookmark_t record = sample(1), loaded = sample(99);
    uint8_t output_before[sizeof(loaded)];
    memcpy(output_before, &loaded, sizeof(loaded));
    assert(podcast_bookmarks_save(&record) == PODCAST_BOOKMARK_OK);
    assert(podcast_bookmarks_save_volume(63) == PODCAST_BOOKMARK_OK);
    storage_snapshot_t snapshot;
    snapshot_storage(&snapshot);

    bool *failures[] = {&fail_open, &fail_read, &fail_payload_read};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        *failures[i] = true;
        assert(podcast_bookmarks_load_slot(0, &loaded) == PODCAST_BOOKMARK_STORAGE_ERROR);
        assert(memcmp(output_before, &loaded, sizeof(loaded)) == 0);
        assert_storage_unchanged(&snapshot);
        *failures[i] = false;
    }
    fail_blob_type = true;
    assert(podcast_bookmarks_load_slot(0, &loaded) == PODCAST_BOOKMARK_INVALID);
    assert(memcmp(output_before, &loaded, sizeof(loaded)) == 0);
    assert_storage_unchanged(&snapshot);
    fail_blob_type = false;
    assert(podcast_bookmarks_load_slot(0, &loaded) == PODCAST_BOOKMARK_OK);
    assert(same_record(&loaded, &record));
    assert_storage_unchanged(&snapshot);
    memcpy(output_before, &loaded, sizeof(loaded));

    slots[0].size = BOOKMARK_WIRE_SIZE - 1;
    snapshot_storage(&snapshot);
    assert(podcast_bookmarks_load_slot(0, &loaded) == PODCAST_BOOKMARK_INVALID);
    assert(memcmp(output_before, &loaded, sizeof(loaded)) == 0);
    assert_storage_unchanged(&snapshot);

    slots[0].size = BOOKMARK_WIRE_SIZE;
    slots[0].bytes[56] ^= 1;
    snapshot_storage(&snapshot);
    assert(podcast_bookmarks_load_slot(0, &loaded) == PODCAST_BOOKMARK_INVALID);
    assert(memcmp(output_before, &loaded, sizeof(loaded)) == 0);
    assert_storage_unchanged(&snapshot);
}

static void test_volume(void)
{
    reset_storage();
    uint8_t volume = 55;
    assert(podcast_bookmarks_load_volume(&volume) == PODCAST_BOOKMARK_NOT_FOUND && volume == 55);
    assert(podcast_bookmarks_save_volume(101) == PODCAST_BOOKMARK_INVALID && !namespace_present);
    assert(podcast_bookmarks_save_volume(0) == PODCAST_BOOKMARK_OK);
    assert(podcast_bookmarks_load_volume(&volume) == PODCAST_BOOKMARK_OK && volume == 0);
    assert(podcast_bookmarks_save_volume(0) == PODCAST_BOOKMARK_OK && volume_writes == 1);
    assert(podcast_bookmarks_save_volume(100) == PODCAST_BOOKMARK_OK && volume_writes == 2);
    volume_value = 255;
    assert(podcast_bookmarks_load_volume(&volume) == PODCAST_BOOKMARK_INVALID && volume == 0);
    fail_read = true;
    assert(podcast_bookmarks_save_volume(55) == PODCAST_BOOKMARK_STORAGE_ERROR);
    assert(podcast_bookmarks_load_volume(&volume) == PODCAST_BOOKMARK_STORAGE_ERROR && volume == 0);
    fail_read = false; fail_write = true;
    assert(podcast_bookmarks_save_volume(55) == PODCAST_BOOKMARK_STORAGE_ERROR);
    fail_write = false; fail_commit = true;
    assert(podcast_bookmarks_save_volume(55) == PODCAST_BOOKMARK_STORAGE_ERROR);
    assert(open_handles == 0);
}

int main(void)
{
    test_validation_and_utf8();
    test_codec_rejects_corruption();
    test_recent_lru_and_wear();
    test_corrupt_storage_and_errors();
    test_slot_export_is_read_only();
    test_slot_export_boundaries();
    test_slot_export_failures_preserve_storage();
    test_volume();
    puts("podcast bookmarks: UTF-8, corruption, full IDs, 24-slot LRU, recency, read-only slot export, wear, errors and volume PASS");
    return 0;
}
