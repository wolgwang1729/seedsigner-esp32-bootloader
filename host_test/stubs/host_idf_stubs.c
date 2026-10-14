/**
 * @file host_idf_stubs.c
 * @brief Host implementations of stubbed ESP-IDF heap/MMU/cache/partition APIs.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "esp_partition.h"
#include "esp_random.h"

const char *esp_err_to_name(esp_err_t err) {
  switch (err) {
  case ESP_OK:
    return "ESP_OK";
  case ESP_ERR_INVALID_STATE:
    return "ESP_ERR_INVALID_STATE";
  case ESP_ERR_NOT_FOUND:
    return "ESP_ERR_NOT_FOUND";
  case ESP_ERR_NO_MEM:
    return "ESP_ERR_NO_MEM";
  case ESP_ERR_INVALID_ARG:
    return "ESP_ERR_INVALID_ARG";
  default:
    return "ESP_ERR_UNKNOWN";
  }
}

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
  (void)caps;
  if (alignment < sizeof(void *)) {
    alignment = sizeof(void *);
  }
  void *ptr = NULL;
  if (posix_memalign(&ptr, alignment, size ? size : 1) != 0) {
    return NULL;
  }
  return ptr;
}

void heap_caps_free(void *ptr) { free(ptr); }

esp_err_t esp_mmu_vaddr_to_paddr(const void *vaddr, uint32_t *paddr,
                                 mmu_target_t *target) {
  if (!vaddr || !paddr) {
    return ESP_ERR_INVALID_ARG;
  }
  /* Fake deterministic paddr derived from vaddr for host tests. */
  *paddr = 0x40000000U + ((uintptr_t)vaddr & 0x00FFFFFFU);
  if (target) {
    *target = 0;
  }
  return ESP_OK;
}

esp_err_t esp_cache_msync(void *addr, size_t size, int flags) {
  (void)addr;
  (void)size;
  (void)flags;
  return ESP_OK;
}

/* ---- RAM-backed partitions for anti_phish ---- */

#ifndef HOST_RANDOM_FILL_SIZE
#define HOST_RANDOM_FILL_SIZE (4096U * 2U)
#endif

static uint8_t host_nvs_data[4096];
static uint8_t host_random_fill[HOST_RANDOM_FILL_SIZE];
static bool host_part_inited = false;

static esp_partition_t host_nvs_part = {
    .address = 0x100000, .size = sizeof(host_nvs_data), .label = "nvs"};
static esp_partition_t host_random_fill_part = {
    .address = 0x200000, .size = sizeof(host_random_fill), .label = "random_fill"};

static void host_part_init_once(void) {
  if (!host_part_inited) {
    memset(host_nvs_data, 0xFF, sizeof(host_nvs_data));
    memset(host_random_fill, 0xFF, sizeof(host_random_fill));
    host_part_inited = true;
  }
}

/** Reset stub partitions to erased state (called from tests). */
void host_part_reset(void) {
  memset(host_nvs_data, 0xFF, sizeof(host_nvs_data));
  memset(host_random_fill, 0xFF, sizeof(host_random_fill));
  host_part_inited = true;
}

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                 esp_partition_subtype_t subtype,
                                                 const char *label) {
  (void)type;
  (void)subtype;
  host_part_init_once();
  if (!label) {
    return NULL;
  }
  if (strcmp(label, "nvs") == 0) {
    return &host_nvs_part;
  }
  if (strcmp(label, "random_fill") == 0) {
    return &host_random_fill_part;
  }
  return NULL;
}

static uint8_t *part_backing(const esp_partition_t *p) {
  if (p == &host_nvs_part) {
    return host_nvs_data;
  }
  if (p == &host_random_fill_part) {
    return host_random_fill;
  }
  return NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *data, size_t size) {
  uint8_t *backing = part_backing(partition);
  if (!backing || !data || offset > partition->size ||
      size > partition->size - offset) {
    return ESP_ERR_INVALID_ARG;
  }
  memcpy(data, backing + offset, size);
  return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset,
                              const void *data, size_t size) {
  uint8_t *backing = part_backing(partition);
  if (!backing || !data || offset > partition->size ||
      size > partition->size - offset) {
    return ESP_ERR_INVALID_ARG;
  }
  memcpy(backing + offset, data, size);
  return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset, size_t size) {
  uint8_t *backing = part_backing(partition);
  if (!backing || offset > partition->size || size > partition->size - offset) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(backing + offset, 0xFF, size);
  return ESP_OK;
}

/* Deterministic xorshift PRNG so anti_phish tests are reproducible. */
static uint64_t host_rng_state = 0x9E3779B97F4A7C15ULL;

void host_rng_seed(uint64_t seed) { host_rng_state = seed ? seed : 1U; }

void esp_fill_random(void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  for (size_t i = 0; i < len; ++i) {
    host_rng_state ^= host_rng_state << 13;
    host_rng_state ^= host_rng_state >> 7;
    host_rng_state ^= host_rng_state << 17;
    p[i] = (uint8_t)(host_rng_state & 0xFFU);
  }
}

uint32_t esp_random(void) {
  uint32_t v = 0;
  esp_fill_random(&v, sizeof(v));
  return v;
}
