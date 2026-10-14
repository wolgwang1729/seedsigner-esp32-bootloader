#pragma once
/* Host stub for esp_cache.h */
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M (1U << 0)
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE (1U << 1)
esp_err_t esp_cache_msync(void *addr, size_t size, int flags);
