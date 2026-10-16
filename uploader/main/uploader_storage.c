/** @file uploader_storage.c
 * @brief Target-aware SD mount + transactional publish.
 */
#include "uploader_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "mbedtls/sha256.h"

#if !CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_IDF_TARGET_ESP32S3
#error "seedsigner_sd_uploader supports esp32p4/esp32s3 only (re-run set-target)"
#endif

#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#elif CONFIG_IDF_TARGET_ESP32S3
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#endif

static const char *TAG = "SSDU";

const char *ssdu_state = "NO_MEDIA";
uint64_t ssdu_offset, ssdu_length, ssdu_capacity;
uint8_t ssdu_digest[32];
uint8_t ssdu_observed[32];
uint64_t ssdu_observed_len;
char ssdu_final[SSDU_MAX_NAME + 1];

static sdmmc_card_t *s_card;
static FILE *s_active;
static char s_temp[192], s_dest[192];
/* 64-byte aligned like the reference uploader: SDMMC DMA checks buffer
 * alignment against the data-cache line size (P4 L1). */
static uint8_t s_io_buf[SSDU_CHUNK_MAX] SDU_BUFFER __attribute__((aligned(64)));
// One file is open at a time. An explicit stdio buffer prevents the small
// libc default from fragmenting large reads into single-sector VFS requests.
static uint8_t s_file_buf[SSDU_CHUNK_MAX] SDU_BUFFER __attribute__((aligned(64)));
#if CONFIG_IDF_TARGET_ESP32S3
static bool s_s3_spi_inited;
#endif

#if CONFIG_IDF_TARGET_ESP32P4
static sd_pwr_ctrl_handle_t s_pwr;
#endif

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static ssdu_status_t io_fail(void)
{
    ssdu_state = "UNCERTAIN";
    return (errno == ENOSPC) ? SSDU_ERR_SPACE : SSDU_ERR_IO;
}

static bool is_idle(void)
{
    return strcmp(ssdu_state, "IDLE") == 0 ||
           strcmp(ssdu_state, "COMMITTED") == 0;
}

static void sha_hex(const uint8_t d[32], char out[65])
{
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = kHex[d[i] >> 4];
        out[2 * i + 1] = kHex[d[i] & 15];
    }
    out[64] = 0;
}

/** Independent readback hash of path; expects ssdu_length/ssdu_digest set. */
static ssdu_status_t verify_path(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return io_fail();
    setvbuf(f, (char *)s_file_buf, _IOFBF, sizeof(s_file_buf));
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        fclose(f);
        return io_fail();
    }
    uint64_t count = 0;
    bool fail = false;
    for (;;) {
        size_t n = fread(s_io_buf, 1, sizeof(s_io_buf), f);
        if (n) {
            count += n;
            if (count > ssdu_length ||
                mbedtls_sha256_update(&ctx, s_io_buf, n) != 0)
                fail = true;
        }
        if (n < sizeof(s_io_buf)) {
            if (ferror(f))
                fail = true;
            break;
        }
    }
    uint8_t got[32];
    if (mbedtls_sha256_finish(&ctx, got) != 0)
        fail = true;
    mbedtls_sha256_free(&ctx);
    if (fclose(f) != 0)
        fail = true;
    if (fail)
        return io_fail();
    ssdu_observed_len = count;
    memcpy(ssdu_observed, got, 32);
    if (count != ssdu_length || memcmp(got, ssdu_digest, 32) != 0)
        return SSDU_ERR_DIGEST;
    return SSDU_OK;
}

/* ------------------------------------------------------------------ */
/* mount                                                              */
/* ------------------------------------------------------------------ */

#if CONFIG_IDF_TARGET_ESP32S3
static void s3_powerup_clocks(void)
{
    /* Same 80-clock wake-up as the loader: bit-banged on MOSI/SCK with CS high. */
    gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_11, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_12, 1);
    gpio_set_level(GPIO_NUM_10, 1);
    for (int i = 0; i < 80; i++) {
        gpio_set_level(GPIO_NUM_11, 0);
        esp_rom_delay_us(5);
        gpio_set_level(GPIO_NUM_11, 1);
        esp_rom_delay_us(5);
    }
    gpio_set_level(GPIO_NUM_11, 0);
}
#endif

ssdu_status_t ssdu_mount(void)
{
    if (s_card)
        return SSDU_OK;
    if (strcmp(ssdu_state, "NO_MEDIA") != 0)
        return SSDU_ERR_BUSY;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false, /* never format a user card */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t ret;

#if CONFIG_IDF_TARGET_ESP32P4
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = 4 };
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr) != ESP_OK) {
        ESP_LOGE(TAG, "LDO4 power control init failed");
        return SSDU_ERR_MOUNT;
    }
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = 43;
    slot.cmd = 44;
    slot.d0 = 39;
    slot.d1 = 40;
    slot.d2 = 41;
    slot.d3 = 42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ret = esp_vfs_fat_sdmmc_mount(SSDU_MOUNT_POINT, &host, &slot,
                                  &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P4 SDMMC mount failed: %s", esp_err_to_name(ret));
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
        s_pwr = NULL;
        return SSDU_ERR_MOUNT;
    }
#else
    /* ESP32-S3: SDSPI on SPI2 (MOSI=10 MISO=9 SCK=11 CS=12), then 1-bit SDMMC. */
    gpio_set_pull_mode(GPIO_NUM_9, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_NUM_10, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_NUM_11, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_NUM_12, GPIO_PULLUP_ONLY);
    s3_powerup_clocks();

    sdmmc_host_t host_spi = SDSPI_HOST_DEFAULT();
    host_spi.slot = SPI2_HOST;
    host_spi.max_freq_khz = 4000;
    spi_bus_config_t bus = {
        .mosi_io_num = 10,
        .miso_io_num = 9,
        .sclk_io_num = 11,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,
    };
    ret = spi_bus_initialize(host_spi.slot, &bus, SDSPI_DEFAULT_DMA);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_s3_spi_inited = true;
        sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
        dev.gpio_cs = GPIO_NUM_12;
        dev.host_id = host_spi.slot;
        ret = esp_vfs_fat_sdspi_mount(SSDU_MOUNT_POINT, &host_spi, &dev,
                                      &mount_config, &s_card);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "S3 SD mounted via SDSPI");
            goto mounted;
        }
        ESP_LOGW(TAG, "SDSPI 4MHz failed (%s), trying probing speed",
                 esp_err_to_name(ret));
        host_spi.max_freq_khz = SDMMC_FREQ_PROBING;
        ret = esp_vfs_fat_sdspi_mount(SSDU_MOUNT_POINT, &host_spi, &dev,
                                      &mount_config, &s_card);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "S3 SD mounted via SDSPI (probing)");
            goto mounted;
        }
    }
    {
        sdmmc_host_t host_mm = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
        slot.clk = 11;
        slot.cmd = 10;
        slot.d0 = 9;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        ret = esp_vfs_fat_sdmmc_mount(SSDU_MOUNT_POINT, &host_mm, &slot,
                                      &mount_config, &s_card);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "S3 SD mount failed: %s", esp_err_to_name(ret));
            if (s_s3_spi_inited) {
                spi_bus_free(host_spi.slot);
                s_s3_spi_inited = false;
            }
            return SSDU_ERR_MOUNT;
        }
    }
mounted:;
#endif

    ssdu_capacity =
        (uint64_t)s_card->csd.capacity * (uint64_t)s_card->csd.sector_size;
    ssdu_state = "IDLE";
    /* NOTE: no %llu anywhere (see uploader_main.c note). Capacity in bytes
     * is reported to the host in HELLO via ssdu_print_u64. */
    ESP_LOGI(TAG, "SD mounted at %s", SSDU_MOUNT_POINT);
    return SSDU_OK;
}

/* ------------------------------------------------------------------ */
/* transactions                                                       */
/* ------------------------------------------------------------------ */

ssdu_status_t ssdu_begin(const char *name, uint64_t size,
                         const uint8_t sha256[32])
{
    if (!is_idle())
        return (strcmp(ssdu_state, "UNCERTAIN") == 0) ? SSDU_ERR_UNCERTAIN
               : (strcmp(ssdu_state, "NO_MEDIA") == 0) ? SSDU_ERR_NO_MEDIA
                                                       : SSDU_ERR_BUSY;
    if (!s_card)
        return SSDU_ERR_NO_MEDIA;
    if (!ssdu_valid_name(name))
        return SSDU_ERR_PATH;
    if (size > SSDU_MAX_FILE)
        return SSDU_ERR_RANGE;

    snprintf(s_dest, sizeof(s_dest), SSDU_MOUNT_POINT "/%s", name);
    struct stat st;
    if (stat(s_dest, &st) == 0)
        return SSDU_ERR_EXISTS;
    if (errno != ENOENT)
        return io_fail();

    uint32_t rnd = esp_random();
    snprintf(s_temp, sizeof(s_temp), SSDU_MOUNT_POINT "/_ssdu_%08lx.part",
             (unsigned long)rnd);
    s_active = fopen(s_temp, "wbx"); /* exclusive: no clobber, FAT-safe */
    if (!s_active)
        return (errno == EEXIST) ? SSDU_ERR_EXISTS : io_fail();
    setvbuf(s_active, (char *)s_file_buf, _IOFBF, sizeof(s_file_buf));

    strncpy(ssdu_final, name, SSDU_MAX_NAME);
    ssdu_final[SSDU_MAX_NAME] = 0;
    memcpy(ssdu_digest, sha256, 32);
    ssdu_length = size;
    ssdu_offset = 0;
    ssdu_state = "RECEIVING";
    return SSDU_OK;
}

ssdu_status_t ssdu_write(uint64_t offset, const uint8_t *data, size_t len)
{
    if (strcmp(ssdu_state, "RECEIVING") != 0 || !s_active)
        return SSDU_ERR_BUSY;
    if (offset != ssdu_offset)
        return SSDU_ERR_OFFSET;
    if (len == 0 || len > SSDU_CHUNK_MAX || len > ssdu_length - ssdu_offset)
        return SSDU_ERR_RANGE;
    if (fwrite(data, 1, len, s_active) != len)
        return io_fail();
    ssdu_offset += len;
    return SSDU_OK;
}

ssdu_status_t ssdu_commit(void)
{
    if (strcmp(ssdu_state, "COMMITTED") == 0)
        return SSDU_OK;
    if (strcmp(ssdu_state, "RECEIVING") != 0 || !s_active)
        return SSDU_ERR_BUSY;
    if (ssdu_offset != ssdu_length)
        return SSDU_ERR_OFFSET;

    ssdu_state = "VERIFYING";
    if (fflush(s_active) != 0 || fsync(fileno(s_active)) != 0 ||
        fclose(s_active) != 0) {
        s_active = NULL;
        return io_fail();
    }
    s_active = NULL;

    /* 1st readback: temp file must hash exactly. */
    ssdu_status_t rc = verify_path(s_temp);
    if (rc != SSDU_OK) {
        if (rc == SSDU_ERR_DIGEST) {
            /* Corrupt transport: drop temp, back to IDLE for a clean retry. */
            unlink(s_temp);
            s_temp[0] = 0;
            ssdu_state = "IDLE";
        }
        return rc;
    }
    /* Refuse to overwrite a file that appeared mid-upload (TOCTOU). */
    struct stat st;
    if (stat(s_dest, &st) == 0) {
        ssdu_state = "UNCERTAIN";
        return SSDU_ERR_EXISTS;
    }
    if (rename(s_temp, s_dest) != 0)
        return io_fail();
    s_temp[0] = 0;

    /* 2nd readback: final file must hash exactly. */
    rc = verify_path(s_dest);
    if (rc == SSDU_OK)
        ssdu_state = "COMMITTED";
    return rc;
}

ssdu_status_t ssdu_abort(void)
{
    if (strcmp(ssdu_state, "UNCERTAIN") == 0)
        return SSDU_ERR_UNCERTAIN;
    if (s_active) {
        fclose(s_active);
        s_active = NULL;
    }
    if (s_temp[0]) {
        if (unlink(s_temp) != 0 && errno != ENOENT)
            return io_fail();
        s_temp[0] = 0;
    }
    if (s_card)
        ssdu_state = "IDLE";
    return SSDU_OK;
}

ssdu_status_t ssdu_verify_existing(const char *name, uint64_t size,
                                   const uint8_t sha256[32])
{
    if (!is_idle())
        return SSDU_ERR_BUSY;
    if (!s_card)
        return SSDU_ERR_NO_MEDIA;
    if (!ssdu_valid_name(name))
        return SSDU_ERR_PATH;
    if (size > SSDU_MAX_FILE)
        return SSDU_ERR_RANGE;
    char path[192];
    snprintf(path, sizeof(path), SSDU_MOUNT_POINT "/%s", name);
    /* Fast path: sizes differ, so contents cannot match. Report mismatch
     * without reading: verify_path() below treats count > ssdu_length as a
     * failure and would misreport IO_ERROR (wedging the session UNCERTAIN)
     * instead of DIGEST_MISMATCH. (The length guard stays correct for the
     * COMMIT readbacks, where the file must be exactly the announced size.)
     * The DIGEST report shape is unchanged: observed_length carries the real
     * SD size, observed digest is zeroed (nothing was hashed). */
    struct stat st;
    if (stat(path, &st) != 0)
        return io_fail();
    if (!S_ISREG(st.st_mode))
        return SSDU_ERR_PATH;
    if ((uint64_t)st.st_size != size) {
        ssdu_observed_len = (uint64_t)st.st_size;
        memset(ssdu_observed, 0, 32);
        return SSDU_ERR_DIGEST;
    }
    uint64_t keep_len = ssdu_length;
    uint8_t keep_d[32];
    memcpy(keep_d, ssdu_digest, 32);
    ssdu_length = size;
    memcpy(ssdu_digest, sha256, 32);
    ssdu_status_t rc = verify_path(path);
    ssdu_length = keep_len;
    memcpy(ssdu_digest, keep_d, 32);
    if (rc == SSDU_ERR_DIGEST) {
        /* keep observed/observed_len for the error report */
    }
    return rc;
}

ssdu_status_t ssdu_remove(const char *name)
{
    if (!is_idle())
        return SSDU_ERR_BUSY;
    if (!s_card)
        return SSDU_ERR_NO_MEDIA;
    if (!ssdu_valid_name(name))
        return SSDU_ERR_PATH;
    char path[192];
    snprintf(path, sizeof(path), SSDU_MOUNT_POINT "/%s", name);
    struct stat st;
    if (stat(path, &st) != 0)
        return (errno == ENOENT) ? SSDU_OK : io_fail();
    if (!S_ISREG(st.st_mode))
        return SSDU_ERR_PATH;
    return (unlink(path) == 0) ? SSDU_OK : io_fail();
}

ssdu_status_t ssdu_release(void)
{
    if (strcmp(ssdu_state, "RELEASED") == 0)
        return SSDU_OK;
    if (!is_idle())
        return SSDU_ERR_BUSY;
    if (!s_card)
        return SSDU_ERR_NO_MEDIA;
    if (esp_vfs_fat_sdcard_unmount(SSDU_MOUNT_POINT, s_card) != ESP_OK)
        return io_fail();
    s_card = NULL;
#if CONFIG_IDF_TARGET_ESP32P4
    if (s_pwr) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
        s_pwr = NULL;
    }
#else
    if (s_s3_spi_inited) {
        spi_bus_free(SPI2_HOST);
        s_s3_spi_inited = false;
    }
#endif
    ssdu_state = "RELEASED";
    return SSDU_OK;
}

void ssdu_list_json(char *buf, size_t cap)
{
    size_t pos = 0;
    pos += snprintf(buf + pos, cap - pos, "[");
    DIR *d = opendir(SSDU_MOUNT_POINT);
    if (d) {
        struct dirent *e;
        bool first = true;
        while ((e = readdir(d)) != NULL && pos + 64 < cap) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            /* Bound the copy for -Wformat-truncation: dirent names can be
             * up to 255 chars, we only list sane ones. */
            if (strlen(e->d_name) > SSDU_MAX_NAME)
                continue;
            char path[192];
            snprintf(path, sizeof(path), SSDU_MOUNT_POINT "/%.128s", e->d_name);
            struct stat st;
            if (stat(path, &st) != 0)
                continue;
            char size_s[21];
            ssdu_print_u64(size_s, sizeof(size_s), (uint64_t)st.st_size);
            pos += snprintf(buf + pos, cap - pos,
                            "%s{\"name\":\"%.128s\",\"size\":%s,\"dir\":%s}",
                            first ? "" : ",", e->d_name, size_s,
                            S_ISDIR(st.st_mode) ? "true" : "false");
            first = false;
        }
        closedir(d);
    }
    snprintf(buf + pos, cap - pos, "]");
}

/* Expose digest hex for the RESP layer without leaking mbedtls internals. */
void ssdu_digest_hex(char out[65], const uint8_t d[32]) { sha_hex(d, out); }
