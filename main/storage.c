// SD-card storage: mount the SDMMC interface and read the firmware bundle.
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

sdmmc_card_t *mount_storage_sdcard(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create on-chip LDO power control driver (%s)",
                 esp_err_to_name(ret));
    } else {
        host.pwr_ctrl_handle = pwr_ctrl_handle;
    }

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
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        return NULL;
    }
    if (st.st_size == 0 || st.st_size > MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Invalid firmware size: %ld. Halting.", (long)st.st_size);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        return NULL;
    }

    uint8_t *psram_buf = heap_caps_aligned_alloc(65536, st.st_size, MALLOC_CAP_SPIRAM);
    if (!psram_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%lu bytes). Halting.", (unsigned long)st.st_size);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        return NULL;
    }

    FILE *f = fopen(SD_FIRMWARE_PATH, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s. Halting.", SD_FIRMWARE_PATH);
        free(psram_buf);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        return NULL;
    }
    if (fread(psram_buf, 1, st.st_size, f) != st.st_size) {
        ESP_LOGE(TAG, "Failed to read firmware file entirely. Halting.");
        fclose(f);
        free(psram_buf);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        return NULL;
    }
    fclose(f);

    ESP_LOGI(TAG, "Unmounting SD card before verification (TOCTOU-safe)...");
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    ESP_LOGI(TAG, "[SD CARD] Loaded %lu bytes from %s", (unsigned long)st.st_size,
             SD_FIRMWARE_PATH);

    if (out_size) *out_size = st.st_size;
    return psram_buf;
}
