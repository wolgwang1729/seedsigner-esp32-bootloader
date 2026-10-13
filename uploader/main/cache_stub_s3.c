/** @file cache_stub_s3.c
 * @brief S3 pure-RAM only: satisfy SD/SPI cache-maintenance queries.
 *
 * Under APP_BUILD_TYPE_PURE_RAM_APP, IDF builds esp_mm/esp_cache_msync.c
 * only when SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE (P4), so two cache helpers
 * are undefined on S3 and the SD/SPI drivers fail to link:
 *
 *  - esp_cache_get_alignment(): S3 internal SRAM is not L1-cached, so the
 *    correct answer is 0 (callers fall back to 4-byte alignment).
 *  - esp_cache_msync(): cache clean/invalidate around DMA; a no-op is
 *    correct for uncached internal SRAM. External RAM never occurs here:
 *    the DMA wrap (ram_dma.c) rejects MALLOC_CAP_SPIRAM outright.
 */
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_private/esp_cache_private.h"

esp_err_t esp_cache_get_alignment(uint32_t heap_caps, size_t *out_alignment)
{
    (void)heap_caps;
    if (!out_alignment)
        return ESP_ERR_INVALID_ARG;
    *out_alignment = 0;
    return ESP_OK;
}

esp_err_t esp_cache_msync(void *addr, size_t size, int flags)
{
    (void)addr;
    (void)size;
    (void)flags;
    return ESP_OK;
}
