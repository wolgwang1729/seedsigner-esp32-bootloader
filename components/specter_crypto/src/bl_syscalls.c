#include <string.h>
#include "bl_syscalls.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_vfs_fat.h"

static const char* TAG = "SYSCALLS";

const char* blsys_platform_id(void) {
    return "seedsigner_esp32p4";
}

bool blsys_init(void) {
    return true;
}

void blsys_deinit(void) {
}

bool blsys_flash_map_get_items(int items, ...) {
    return false; // Not used
}

bool blsys_flash_erase(bl_addr_t addr, size_t size) {
    return false; // Flash writes disabled
}

bool blsys_flash_read(bl_addr_t addr, void* buf, size_t len) {
    // Treat addr as a direct pointer (PSRAM buffer)
    memcpy(buf, (const void*)addr, len);
    return true;
}

bool blsys_flash_write(bl_addr_t addr, const void* buf, size_t len) {
    return false; // Flash writes disabled
}

bool blsys_flash_crc32(uint32_t* p_crc, bl_addr_t addr, size_t len) {
    *p_crc = esp_rom_crc32_le(*p_crc, (const uint8_t*)addr, len);
    return true;
}

bool blsys_flash_write_protect(bl_addr_t addr, size_t size, bool enable) {
    return false;
}

bool blsys_flash_read_protect(int level) {
    return false;
}

int blsys_flash_get_read_protection_level(void) {
    return 0;
}

uint32_t blsys_media_devices(void) { return 0; }
const char* blsys_media_name(uint32_t device_idx) { return ""; }
bool blsys_media_check(uint32_t device_idx) { return false; }
bool blsys_media_mount(uint32_t device_idx) { return false; }
void blsys_media_umount(void) {}

const char* blsys_ffind_first(bl_ffind_ctx_t* ctx, const char* path, const char* pattern) { return NULL; }
const char* blsys_ffind_next(bl_ffind_ctx_t* ctx) { return NULL; }
void blsys_ffind_close(bl_ffind_ctx_t* ctx) {}

bl_file_t blsys_fopen(bl_file_obj_t* p_file_obj, const char* filename, const char* mode) { return NULL; }
size_t blsys_fread(void* ptr, size_t size, size_t count, bl_file_t file) { return 0; }
bl_foffset_t blsys_ftell(bl_file_t file) { return -1; }
int blsys_fseek(bl_file_t file, bl_foffset_t offset, int origin) { return -1; }
bl_fsize_t blsys_fsize(bl_file_t file) { return 0; }
int blsys_feof(bl_file_t file) { return 1; }
int blsys_fclose(bl_file_t file) { return EOF; }

void blsys_fatal_error(const char* text) {
    ESP_LOGE(TAG, "FATAL: %s", text);
    while(1) { }
}

bl_alert_status_t blsys_alert(blsys_alert_type_t type, const char* caption,
                              const char* text, uint32_t time_ms,
                              uint32_t flags) {
    ESP_LOGW(TAG, "ALERT: %s - %s", caption, text);
    return bl_alert_dismissed;
}

void blsys_progress(const char* caption, const char* operation,
                    uint32_t percent_x100) {
    ESP_LOGI(TAG, "PROGRESS: %s - %s (%lu%%)", caption, operation, percent_x100 / 100);
}

bool blsys_start_firmware(bl_addr_t start_addr, uint32_t argument) {
    return false;
}

void secp256k1_default_illegal_callback_fn(const char* str, void* data) {
    ESP_LOGE(TAG, "secp256k1 illegal callback: %s", str);
    while(1) { }
}

void secp256k1_default_error_callback_fn(const char* str, void* data) {
    ESP_LOGE(TAG, "secp256k1 error callback: %s", str);
    while(1) { }
}
