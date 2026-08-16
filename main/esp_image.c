// ESP32 image parser + load-plan builder supporting both ESP32-P4 and ESP32-S3.
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "esp_cache.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/ext_mem_defs.h"
#define JMP_ZONE_BASE 0x403A0000
#define JMP_ZONE_END  0x403B8000
#endif

#include "loader_config.h"
#include "esp_image.h"

static const char *TAG = "SEEDSIGNER_LOADER";

image_plan_t esp_image_plan(const uint8_t *psram_buf, uint32_t payload_offset)
{
    image_plan_t plan;
    memset(&plan, 0, sizeof(plan));

    // Step 3: validate the raw ESP32 image header.
    __attribute__((aligned(4))) esp_image_header_t hdr;
    hdr = *(esp_image_header_t *)(psram_buf + payload_offset);

    if (hdr.magic != ESP_IMAGE_MAGIC) {
        ESP_LOGE(TAG, "Bad image magic: 0x%02X (expected 0xE9). Halting.", hdr.magic);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    if (hdr.segment_count == 0 || hdr.segment_count > MAX_SEGMENT_COUNT) {
        ESP_LOGE(TAG, "Bad segment count: %d. Halting.", hdr.segment_count);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "Image OK: %d segments, entry=0x%08lX",
             hdr.segment_count, (unsigned long)hdr.entry_addr);
    plan.entry_addr = hdr.entry_addr;

    // Step 4: first pass — measure PSRAM MMU footprint.
    uint32_t max_offset = 0;
    uint32_t offset     = sizeof(esp_image_header_t);

    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        memcpy(&seg, psram_buf + payload_offset + offset, sizeof(seg));
        offset += sizeof(seg);
        if (seg.data_len > MAX_FIRMWARE_SIZE) {
            ESP_LOGE(TAG, "Segment %d bad length %lu. Halting.", i, (unsigned long)seg.data_len);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

#if CONFIG_IDF_TARGET_ESP32P4
        if (seg.load_addr >= 0x48000000 && seg.load_addr < 0x4C000000) {
            uint32_t end = (seg.load_addr - 0x48000000) + seg.data_len;
            if (end > max_offset) max_offset = end;
        }
#elif CONFIG_IDF_TARGET_ESP32S3
        if ((seg.load_addr >= 0x42000000 && seg.load_addr < 0x44000000) ||
            (seg.load_addr >= 0x3C000000 && seg.load_addr < 0x3E000000)) {
            uint32_t end = (seg.load_addr & SOC_MMU_LINEAR_ADDR_MASK) + seg.data_len;
            if (end > max_offset) max_offset = end;
        }
#endif
        offset += seg.data_len;
    }
    max_offset = (max_offset + 0xFFFF) & ~0xFFFF;
    ESP_LOGI(TAG, "PSRAM MMU footprint: %lu bytes", (unsigned long)max_offset);

    // Step 5: allocate fake-flash staging buffer for mapped segments.
    if (max_offset > 0) {
        plan.fake_flash = heap_caps_aligned_alloc(65536, max_offset, MALLOC_CAP_SPIRAM);
        if (!plan.fake_flash) {
            ESP_LOGE(TAG, "fake_flash alloc failed. Halting.");
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        memset(plan.fake_flash, 0, max_offset);

        mmu_target_t target;
        if (esp_mmu_vaddr_to_paddr(plan.fake_flash, &plan.flash_paddr, &target) != ESP_OK) {
            ESP_LOGE(TAG, "paddr lookup for fake_flash failed. Halting.");
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "fake_flash: vaddr=%p paddr=0x%08lX",
                 plan.fake_flash, (unsigned long)plan.flash_paddr);
        plan.max_offset = max_offset;
    }

    // Step 6: second pass — place segments and build MMU map.
    offset = sizeof(esp_image_header_t);

    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        memcpy(&seg, psram_buf + payload_offset + offset, sizeof(seg));
        offset += sizeof(seg);

        ESP_LOGI(TAG, "Seg %d: addr=0x%08lX len=%lu",
                 i, (unsigned long)seg.load_addr, (unsigned long)seg.data_len);

#if CONFIG_IDF_TARGET_ESP32P4
        if (seg.load_addr >= 0x48000000 && seg.load_addr < 0x4C000000) {
            // Mapped segment → stage into fake_flash
            uint32_t va_start = seg.load_addr & ~0xFFFF;
            uint32_t va_end   = (seg.load_addr + seg.data_len + 0xFFFF - 1) & ~0xFFFF;
            if (seg.data_len == 0) va_end = va_start;

            mmu_mapping_t *m = &plan.mmu_mappings[plan.mmu_mapping_count];
            m->vaddr = va_start;
            m->len   = va_end - va_start;
            m->paddr = plan.flash_paddr + (va_start - 0x48000000);

            uint32_t write_off = seg.load_addr - 0x48000000;
            memcpy(plan.fake_flash + write_off, psram_buf + payload_offset + offset, seg.data_len);
            plan.mmu_mapping_count++;
            ESP_LOGI(TAG, "  -> fake_flash+0x%lX", (unsigned long)write_off);
        } else if (seg.load_addr >= 0x4FF00000 && seg.load_addr < 0x4FFC0000) {
            // Direct segment → deferred copy into internal SRAM
            pending_copy_t *c = &plan.direct_copies[plan.copy_count];
            c->dest = (void *)seg.load_addr;
            c->src  = (void *)(psram_buf + payload_offset + offset);
            c->len  = seg.data_len;
            plan.copy_count++;
            ESP_LOGI(TAG, "  -> direct copy to 0x%08lX", (unsigned long)seg.load_addr);
        } else {
            ESP_LOGE(TAG, "Segment %d load addr 0x%08lX outside recognized P4 memory windows. Halting.",
                     i, (unsigned long)seg.load_addr);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

#elif CONFIG_IDF_TARGET_ESP32S3
        bool is_irom = (seg.load_addr >= 0x42000000 && seg.load_addr < 0x44000000);
        bool is_drom = (seg.load_addr >= 0x3C000000 && seg.load_addr < 0x3E000000);

        if (is_irom || is_drom) {
            // Mapped segment → stage into fake_flash at its linear offset
            uint32_t linear   = seg.load_addr & SOC_MMU_LINEAR_ADDR_MASK;
            uint32_t va_start = seg.load_addr & ~0xFFFF;
            uint32_t va_end   = (seg.load_addr + seg.data_len + 0xFFFF - 1) & ~0xFFFF;
            if (seg.data_len == 0) va_end = va_start;

            mmu_mapping_t *m = &plan.mmu_mappings[plan.mmu_mapping_count];
            m->vaddr = va_start;
            m->len   = va_end - va_start;
            m->paddr = plan.flash_paddr + (va_start & SOC_MMU_LINEAR_ADDR_MASK);

            uint32_t write_off = linear;
            memcpy(plan.fake_flash + write_off, psram_buf + payload_offset + offset, seg.data_len);
            plan.mmu_mapping_count++;
            ESP_LOGI(TAG, "  -> fake_flash+0x%lX", (unsigned long)write_off);
        } else if ((seg.load_addr >= 0x40370000 && seg.load_addr < 0x403E0000) ||
                   (seg.load_addr >= 0x3FC88000 && seg.load_addr < 0x3FD00000) ||
                   (seg.load_addr >= 0x50000000 && seg.load_addr < 0x50020000) ||
                   (seg.load_addr >= 0x600FE000 && seg.load_addr < 0x60100000)) {
            // Direct segment → check collision with loader JMP zone
            uint32_t seg_end = seg.load_addr + seg.data_len;
            if (seg.load_addr < JMP_ZONE_BASE && seg_end > JMP_ZONE_BASE) {
                ESP_LOGE(TAG, "Segment %d (0x%08lX-0x%08lX) collides with loader's JMP zone (0x%08X-0x%08X). Halting.",
                         i, (unsigned long)seg.load_addr, (unsigned long)seg_end,
                         JMP_ZONE_BASE, JMP_ZONE_END);
                while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            if (seg.load_addr >= JMP_ZONE_BASE && seg.load_addr < JMP_ZONE_END) {
                ESP_LOGE(TAG, "Segment %d starts inside loader's JMP zone. Halting.", i);
                while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
            }

            pending_copy_t *c = &plan.direct_copies[plan.copy_count];
            c->dest = (void *)seg.load_addr;
            c->src  = (void *)(psram_buf + payload_offset + offset);
            c->len  = seg.data_len;
            plan.copy_count++;
            ESP_LOGI(TAG, "  -> direct copy to 0x%08lX", (unsigned long)seg.load_addr);
        } else {
            ESP_LOGE(TAG, "Segment %d load addr 0x%08lX outside recognized S3 memory windows. Halting.",
                     i, (unsigned long)seg.load_addr);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
#endif
        offset += seg.data_len;
    }

    if (plan.max_offset > 0) {
        esp_cache_msync(plan.fake_flash, plan.max_offset,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }

    return plan;
}
