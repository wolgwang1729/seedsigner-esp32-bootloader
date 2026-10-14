// SD-card storage for ESP32-P4: mount native 4-bit SDMMC interface with on-chip LDO.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "storage.h"
#include "loader_config.h"

static const char *TAG = "SEEDSIGNER_LOADER";

// LDO4 powers the SDMMC IO domain. Retained statically so the unmount path
// can power the domain off (electrically TOCTOU-safe) instead of leaking it.
// Mirrors specter-bootloader-mike esp32p4_media.c + uploader_storage.c.
static sd_pwr_ctrl_handle_t s_pwr_ctrl = NULL;

// Unmount + power off LDO4. Every exit from load_firmware_from_sd() must go
// through here so the SD card is electrically dead before verification/JMP.
static void unmount_and_power_off(sdmmc_card_t *card)
{
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    if (s_pwr_ctrl) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
        s_pwr_ctrl = NULL;
    }
}

sdmmc_card_t *mount_storage_sdcard(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    if (s_pwr_ctrl != NULL) {
        // Stale handle from a previous failed attempt — don't stack handles.
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
        s_pwr_ctrl = NULL;
    }
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create on-chip LDO power control driver (%s)",
                 esp_err_to_name(ret));
        s_pwr_ctrl = NULL;
        return NULL; // fail-closed: don't mount without IO power
    }
    host.pwr_ctrl_handle = s_pwr_ctrl;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk   = 43;
    slot_config.cmd   = 44;
    slot_config.d0    = 39;
    slot_config.d1    = 40;
    slot_config.d2    = 41;
    slot_config.d3    = 42;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = NULL;
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (%s).", esp_err_to_name(ret));
        if (s_pwr_ctrl) {
            sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
            s_pwr_ctrl = NULL;
        }
        return NULL;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return card;
}

uint8_t *load_firmware_from_sd(size_t *out_size)
{
    sdmmc_card_t *card = mount_storage_sdcard();
    if (card == NULL) {
        ESP_LOGE(TAG, "No SD card. Halting.");
        return NULL;
    }

    struct stat st;
    if (stat(SD_FIRMWARE_PATH, &st) != 0) {
        ESP_LOGE(TAG, "Firmware file %s not found. Halting.", SD_FIRMWARE_PATH);
        unmount_and_power_off(card);
        return NULL;
    }
    if (st.st_size == 0 || st.st_size > MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Invalid firmware size: %ld. Halting.", (long)st.st_size);
        unmount_and_power_off(card);
        return NULL;
    }

    uint8_t *psram_buf = heap_caps_aligned_alloc(65536, st.st_size, MALLOC_CAP_SPIRAM);
    if (!psram_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%lu bytes). Halting.", (unsigned long)st.st_size);
        unmount_and_power_off(card);
        return NULL;
    }

    FILE *f = fopen(SD_FIRMWARE_PATH, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s. Halting.", SD_FIRMWARE_PATH);
        free(psram_buf);
        unmount_and_power_off(card);
        return NULL;
    }
    if (fread(psram_buf, 1, st.st_size, f) != st.st_size) {
        ESP_LOGE(TAG, "Failed to read firmware file entirely. Halting.");
        fclose(f);
        free(psram_buf);
        unmount_and_power_off(card);
        return NULL;
    }
    fclose(f);

    ESP_LOGI(TAG, "Unmounting SD card before verification (TOCTOU-safe)...");
    unmount_and_power_off(card);

    *out_size = st.st_size;
    ESP_LOGI(TAG, "Loaded %lu bytes from %s", (unsigned long)*out_size, SD_FIRMWARE_PATH);
    return psram_buf;
}
