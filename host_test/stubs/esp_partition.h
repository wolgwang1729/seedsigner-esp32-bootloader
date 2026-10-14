#pragma once
/* Host stub for esp_partition.h — RAM-backed partitions for anti_phish tests.
 * Ported from specter-bootloader-mike/test/esp32p4_stubs/esp_partition.h,
 * extended with ESP_ERR_* codes used by main/anti_phish.c.
 */
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ESP_PARTITION_TYPE_DATA
#define ESP_PARTITION_TYPE_DATA 1
#endif

typedef int esp_partition_type_t;
typedef int esp_partition_subtype_t;

typedef struct esp_partition_t {
  uint32_t address;
  uint32_t size;
  char label[17];
  bool encrypted;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                 esp_partition_subtype_t subtype,
                                                 const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *data, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset,
                              const void *data, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset, size_t size);
