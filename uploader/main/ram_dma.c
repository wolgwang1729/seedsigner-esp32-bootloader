/** @file ram_dma.c
 * @brief Internal-only DMA alignment for the standalone pure-RAM SD driver.
 *
 * Ported from miketlk/specter-bootloader sd_uploader/main/ram_dma.c
 * (https://github.com/miketlk/specter-bootloader/tree/port_esp32-p4):
 * IDF 5.5 heap_align_hw.c rejects all P4 DMA allocations in pure-RAM mode
 * because its minimal closure omits cache APIs. The SDMMC closure includes
 * the pinned cache alignment and synchronization APIs. Keep those real APIs
 * and permit only internal RAM allocations; do not enable external memory.
 */
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"

void __wrap_esp_heap_adjust_alignment_to_hw(size_t* alignment, size_t* size,
                                            uint32_t* caps) {
  const uint32_t needs = MALLOC_CAP_DMA | MALLOC_CAP_DMA_DESC_AHB |
                         MALLOC_CAP_DMA_DESC_AXI | MALLOC_CAP_CACHE_ALIGNED |
                         MALLOC_CAP_SIMD;
  if (*caps & MALLOC_CAP_SPIRAM) {
    *caps |= MALLOC_CAP_INVALID;
    return;
  }
  if (!(*caps & needs)) return;
  size_t cache_alignment = 0;
  if (esp_cache_get_alignment(MALLOC_CAP_INTERNAL, &cache_alignment) !=
      ESP_OK) {
    *caps |= MALLOC_CAP_INVALID;
    return;
  }
  // AXI descriptors require 8-byte alignment and P4 SIMD requires 16.
  size_t required = (*caps & MALLOC_CAP_SIMD) ? 16 : 8;
  if (cache_alignment > required) required = cache_alignment;
  if (required > *alignment) *alignment = required;
  if (!*alignment || (*alignment & (*alignment - 1)) ||
      *size > SIZE_MAX - (*alignment - 1)) {
    *caps |= MALLOC_CAP_INVALID;
    return;
  }
  *size = (*size + *alignment - 1) & ~(*alignment - 1);
  if (*caps & (MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_DMA_DESC_AXI)) {
    *caps &= ~(MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_DMA_DESC_AXI);
    *caps |= MALLOC_CAP_DMA;
  }
  *caps = (*caps & ~MALLOC_CAP_CACHE_ALIGNED) | MALLOC_CAP_INTERNAL;
}
