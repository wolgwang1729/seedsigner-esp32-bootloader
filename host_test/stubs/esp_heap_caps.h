#pragma once
/* Host stub for esp_heap_caps.h */
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM (1U << 0)
#define MALLOC_CAP_8BIT (1U << 1)
#define MALLOC_CAP_DEFAULT 0U
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
