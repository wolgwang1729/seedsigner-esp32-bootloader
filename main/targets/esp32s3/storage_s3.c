// SD-card storage for ESP32-S3: dual-host SDSPI (SPI2_HOST) with SDMMC 1-bit fallback.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#include "storage.h"
#include "loader_config.h"

static const char *TAG = "SEEDSIGNER_LOADER";

static void diag_sd_pins(void)
{
    ESP_LOGI(TAG, "=== SD CARD PIN DIAGNOSTIC ===");
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << 9),
        .pull_down_en = 0,
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);

    gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_11, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT);

    // Test 1: CS HIGH
    gpio_set_level(GPIO_NUM_12, 1);
    gpio_set_level(GPIO_NUM_10, 1);
    gpio_set_level(GPIO_NUM_11, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    int miso_cs_high = gpio_get_level(GPIO_NUM_9);

    // Test 2: CS LOW
    gpio_set_level(GPIO_NUM_12, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    int miso_cs_low = gpio_get_level(GPIO_NUM_9);

    ESP_LOGI(TAG, "Pin States: MISO(CS=1)=%d, MISO(CS=0)=%d", miso_cs_high, miso_cs_low);

    // Send 80 clock pulses with CS=1, MOSI=1 to wake up SD card into SPI mode
    gpio_set_level(GPIO_NUM_12, 1);
    gpio_set_level(GPIO_NUM_10, 1);
    for (int i = 0; i < 80; i++) {
        gpio_set_level(GPIO_NUM_11, 0);
        esp_rom_delay_us(5);
        gpio_set_level(GPIO_NUM_11, 1);
        esp_rom_delay_us(5);
    }
    gpio_set_level(GPIO_NUM_11, 0);
    ESP_LOGI(TAG, "Sent 80 power-up sync clocks to SD card.");
}

sdmmc_card_t *mount_storage_sdcard(void)
{
    diag_sd_pins();
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card = NULL;

    // Attempt 1: SDSPI Host (for buffered Arduino SPI modules like HW-125)
    ESP_LOGI(TAG, "Initializing SDSPI host on SPI2 (MOSI=10, MISO=9, SCK=11, CS=12)...");

    // Enable internal pull-ups on SPI pins for signal integrity
    gpio_set_pull_mode(GPIO_NUM_9,  GPIO_PULLUP_ONLY); // MISO
    gpio_set_pull_mode(GPIO_NUM_10, GPIO_PULLUP_ONLY); // MOSI
    gpio_set_pull_mode(GPIO_NUM_11, GPIO_PULLUP_ONLY); // SCK
    gpio_set_pull_mode(GPIO_NUM_12, GPIO_PULLUP_ONLY); // CS

    sdmmc_host_t host_spi = SDSPI_HOST_DEFAULT();
    host_spi.slot = SPI2_HOST;
    host_spi.max_freq_khz = 4000; // 4MHz for breadboard stability

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = 10,
        .miso_io_num = 9,
        .sclk_io_num = 11,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,
    };

    esp_err_t ret = spi_bus_initialize(host_spi.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        sdspi_device_config_t slot_config_spi = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config_spi.gpio_cs = GPIO_NUM_12;
        slot_config_spi.host_id = host_spi.slot;

        ESP_LOGI(TAG, "Trying SDSPI mount at 4MHz (CS=GPIO 12)...");
        ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host_spi, &slot_config_spi, &mount_config, &card);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD card mounted via SDSPI at %s", MOUNT_POINT);
            return card;
        }
        ESP_LOGW(TAG, "SDSPI mount (4MHz) failed (%s), trying 400kHz...", esp_err_to_name(ret));

        // Try probing speed (400kHz)
        host_spi.max_freq_khz = SDMMC_FREQ_PROBING;
        ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host_spi, &slot_config_spi, &mount_config, &card);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD card mounted via SDSPI (400kHz) at %s", MOUNT_POINT);
            return card;
        }
        ESP_LOGW(TAG, "SDSPI mount (400kHz) failed (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGE(TAG, "Failed to initialize SPI bus (%s)", esp_err_to_name(ret));
    }

    // Attempt 2: SDMMC Slot 1 (1-bit mode, native SD bus: CLK=11, CMD=10, D0=9)
    ESP_LOGI(TAG, "Trying SDMMC 1-bit mount (CLK=11, CMD=10, D0=9)...");
    sdmmc_host_t host_sdmmc = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config_sdmmc = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config_sdmmc.width = 1;
    slot_config_sdmmc.clk   = 11;
    slot_config_sdmmc.cmd   = 10;
    slot_config_sdmmc.d0    = 9;
    slot_config_sdmmc.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host_sdmmc, &slot_config_sdmmc, &mount_config, &card);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted via SDMMC at %s", MOUNT_POINT);
        return card;
    }
    ESP_LOGE(TAG, "SDMMC mount failed (%s)", esp_err_to_name(ret));

    return NULL;
}

uint8_t *load_firmware_from_sd(size_t *out_size)
{
    sdmmc_card_t *card = mount_storage_sdcard();
    if (card == NULL) {
        ESP_LOGE(TAG, "Failed to mount SD card. Halting.");
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

    *out_size = st.st_size;
    ESP_LOGI(TAG, "Loaded %lu bytes from %s", (unsigned long)*out_size, SD_FIRMWARE_PATH);
    return psram_buf;
}
