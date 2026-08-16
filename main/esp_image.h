#pragma once

#include <stdint.h>
#include "loader_config.h"
#include "jump.h"

#define ESP_IMAGE_MAGIC 0xE9

// ESP32 image format (see components/bootloader_support). Layout after the
// payload's Specter "main" header (if any):
//   esp_image_header_t hdr;
//   for each segment: esp_image_segment_header_t + raw data
typedef struct {
    uint8_t  magic;
    uint8_t  segment_count;
    uint8_t  spi_mode;
    uint8_t  spi_speed : 4;
    uint8_t  spi_size  : 4;
    uint32_t entry_addr;
    uint8_t  wp_pin;
    uint8_t  spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t  min_chip_rev;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint8_t  reserved[4];
    uint8_t  hash_appended;
} __attribute__((packed)) esp_image_header_t;

typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} esp_image_segment_header_t;

// Load plan produced by esp_image_plan(). Everything the JMP zone needs:
// entry point, the PSRAM fake-flash staging buffer backing MMU-mapped
// segments, and the deferred direct-to-SRAM copies.
typedef struct {
    uint32_t      entry_addr;       // payload entry point (hdr.entry_addr)
    uint8_t      *fake_flash;       // PSRAM staging buffer (never freed)
    uint32_t      flash_paddr;      // physical PSRAM addr backing fake_flash
    uint32_t      max_offset;       // fake_flash footprint (bytes), 0 if none
    mmu_mapping_t mmu_mappings[MAX_MMU_MAPPINGS];
    int           mmu_mapping_count;
    pending_copy_t direct_copies[MAX_DIRECT_COPIES];
    int           copy_count;
} image_plan_t;

// Validate the raw ESP32 image and build the load plan (app_main steps 3-6).
// Walks the segment table once to measure the PSRAM MMU footprint, stages
// MMU-mapped segments into a fresh aligned fake_flash buffer, and records
// direct-to-SRAM segments as deferred copies.
//
// Fatal errors halt inside (the bootloader fails closed by design). On success
// the caller must NOT free psram_buf — direct copies still point into it.
image_plan_t esp_image_plan(const uint8_t *psram_buf, uint32_t payload_offset);
