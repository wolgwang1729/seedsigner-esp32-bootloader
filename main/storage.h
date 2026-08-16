#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sdmmc_cmd.h"

#define MOUNT_POINT "/sdcard"

#if CONFIG_IDF_TARGET_ESP32P4
#define SD_FIRMWARE_PATH  MOUNT_POINT "/seedsigner_esp32p4.bin"
#define EXPECTED_PLATFORM "seedsigner_esp32p4"
#elif CONFIG_IDF_TARGET_ESP32S3
#define SD_FIRMWARE_PATH  MOUNT_POINT "/seedsigner_esp32s3.bin"
#define EXPECTED_PLATFORM "seedsigner_esp32s3"
#else
#error "Unsupported IDF Target for SeedSigner Bootloader"
#endif

// Mount the SD card (target-specific: SDMMC on P4, SDSPI/SDMMC on S3)
sdmmc_card_t *mount_storage_sdcard(void);

// Load the Specter-signed firmware bundle from the SD card into a PSRAM buffer:
// Reads SD_FIRMWARE_PATH into 64KB-aligned PSRAM and unmounts immediately (anti-TOCTOU).
uint8_t *load_firmware_from_sd(size_t *out_size);
