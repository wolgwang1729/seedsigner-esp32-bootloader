// ESP32 image parser + load-plan builder.
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"

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
        if (seg.load_addr >= 0x48000000 && seg.load_addr < 0x4C000000) {
            uint32_t end = (seg.load_addr - 0x48000000) + seg.data_len;
            if (end > max_offset) max_offset = end;
        }
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
    offset          = sizeof(esp_image_header_t);
    uint32_t last_page_start = 0xFFFFFFFF;

    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        memcpy(&seg, psram_buf + payload_offset + offset, sizeof(seg));
        offset += sizeof(seg);

        ESP_LOGI(TAG, "Seg %d: addr=0x%08lX len=%lu",
                 i, (unsigned long)seg.load_addr, (unsigned long)seg.data_len);

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

            uint32_t page_start = write_off & ~0xFFFF;
            if (page_start != last_page_start) {
                memcpy(plan.fake_flash + page_start, psram_buf + payload_offset, 32); // image header at page start
                last_page_start = page_start;
            }
            plan.mmu_mapping_count++;
            ESP_LOGI(TAG, "  -> fake_flash+0x%lX", (unsigned long)write_off);
        } else {
            // Direct segment → deferred copy into internal SRAM
            plan.direct_copies[plan.copy_count].dest = (void *)seg.load_addr;
            plan.direct_copies[plan.copy_count].src  = (void *)(psram_buf + payload_offset + offset);
            plan.direct_copies[plan.copy_count].len  = seg.data_len;
            plan.copy_count++;
            ESP_LOGI(TAG, "  -> direct copy to 0x%08lX", (unsigned long)seg.load_addr);
        }
        offset += seg.data_len;
    }

    plan.entry_addr = hdr.entry_addr;

    // Flush the staged fake_flash out of the D-cache to physical PSRAM so the
    // MMU maps coherent data (the JMP zone programs the MMU with flash_paddr).
    extern int Cache_WriteBack_Addr(uint32_t map, uint32_t vaddr, uint32_t size);
    Cache_WriteBack_Addr(0x10, (uint32_t)plan.fake_flash, plan.max_offset);
    Cache_WriteBack_Addr(0x20, (uint32_t)plan.fake_flash, plan.max_offset);

    return plan;
}
