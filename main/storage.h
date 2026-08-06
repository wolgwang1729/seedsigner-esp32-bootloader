#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sdmmc_cmd.h"

#define MOUNT_POINT "/sdcard"
#define SD_FIRMWARE_PATH MOUNT_POINT "/seedsigner_esp32p4.bin"

// Mount the SD card via the native SDMMC peripheral (ESP32-P4).
//   Power:  on-chip LDO channel 4 (VDD_SDMMC)
//   Bus:    4-bit, explicit pins (CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42)
// The same wiring is used by the MicroPython payload's board init, so one card
// config covers both the loader read and the runtime firmware.
sdmmc_card_t *mount_storage_sdcard(void);

// Load the Specter-signed firmware bundle from the SD card into a PSRAM buffer:
//   /sdcard/seedsigner_esp32p4.bin = [bl_section_t "main" header]
//                                    [raw ESP32 image]
//                                    [bl_section_t "sign"]
// The card is mounted, read, and unmounted here (TOCTOU-safe: the caller then
// verifies against the PSRAM-resident copy only). On success returns the buffer
// and sets *out_size; the caller owns it (free with free()). On failure returns
// NULL after logging the reason — the caller is expected to halt.
uint8_t *load_firmware_from_sd(size_t *out_size);
