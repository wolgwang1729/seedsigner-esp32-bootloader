// ESP32 image parser + load-plan builder supporting both ESP32-P4 and ESP32-S3.
#include <string.h>
#include <stddef.h>
#include <stdint.h>

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

// Fail-closed on target (halt), returnable on host_test (plan.fatal = true)
// so parser regressions are unit-testable without silicon.
#ifdef HOST_TEST
#define IMAGE_FATAL_RETURN()                                                   \
    do {                                                                       \
        image_plan_t __f;                                                      \
        memset(&__f, 0, sizeof(__f));                                          \
        __f.fatal = true;                                                      \
        if (plan.fake_flash) {                                                 \
            heap_caps_free(plan.fake_flash);                                   \
        }                                                                      \
        plan = __f;                                                            \
        plan.fatal = true;                                                     \
        return plan;                                                           \
    } while (0)
#else
#define IMAGE_FATAL_RETURN()                                                   \
    do {                                                                       \
        while (1)                                                              \
            vTaskDelay(1000 / portTICK_PERIOD_MS);                              \
    } while (0)
#endif

image_plan_t esp_image_plan(const uint8_t *psram_buf, uint32_t payload_offset,
                            size_t fw_size)
{
    image_plan_t plan;
    memset(&plan, 0, sizeof(plan));

    // Validate the bundle cursor before touching it: every read below must
    // stay inside [psram_buf, psram_buf + fw_size).
    if (!psram_buf) {
        ESP_LOGE(TAG, "NULL psram buffer. Halting.");
        IMAGE_FATAL_RETURN();
    }
    if ((size_t)payload_offset > fw_size ||
        fw_size - (size_t)payload_offset < sizeof(esp_image_header_t)) {
        ESP_LOGE(TAG, "Payload offset %lu out of bounds (%lu loaded). Halting.",
                 (unsigned long)payload_offset, (unsigned long)fw_size);
        IMAGE_FATAL_RETURN();
    }

    // Step 3: validate the raw ESP32 image header.
    __attribute__((aligned(4))) esp_image_header_t hdr;
    memcpy(&hdr, psram_buf + payload_offset, sizeof(hdr));

    if (hdr.magic != ESP_IMAGE_MAGIC) {
        ESP_LOGE(TAG, "Bad image magic: 0x%02X (expected 0xE9). Halting.", hdr.magic);
        IMAGE_FATAL_RETURN();
    }
    if (hdr.segment_count == 0 || hdr.segment_count > MAX_SEGMENT_COUNT) {
        ESP_LOGE(TAG, "Bad segment count: %d. Halting.", hdr.segment_count);
        IMAGE_FATAL_RETURN();
    }
    ESP_LOGI(TAG, "Image OK: %d segments, entry=0x%08lX",
             hdr.segment_count, (unsigned long)hdr.entry_addr);
    plan.entry_addr = hdr.entry_addr;

    // Step 4: first pass — measure PSRAM MMU footprint.
    // offset is relative to the ESP image start (after payload_offset); all
    // sums are computed in size_t to avoid 32-bit wraparound.
    uint32_t max_offset = 0;
    size_t offset = sizeof(esp_image_header_t);

    for (int i = 0; i < hdr.segment_count; i++) {
        // Bounds-check segment header read.
        if (offset + sizeof(esp_image_segment_header_t) > fw_size - payload_offset) {
            ESP_LOGE(TAG, "Segment %d header out of bounds. Halting.", i);
            IMAGE_FATAL_RETURN();
        }
        esp_image_segment_header_t seg;
        memcpy(&seg, psram_buf + payload_offset + offset, sizeof(seg));
        offset += sizeof(seg);
        if (seg.data_len > MAX_FIRMWARE_SIZE) {
            ESP_LOGE(TAG, "Segment %d bad length %lu. Halting.", i, (unsigned long)seg.data_len);
            IMAGE_FATAL_RETURN();
        }
        // Bounds-check segment data extent.
        if ((size_t)seg.data_len > fw_size - payload_offset - offset) {
            ESP_LOGE(TAG, "Segment %d data out of bounds (len %lu). Halting.",
                     i, (unsigned long)seg.data_len);
            IMAGE_FATAL_RETURN();
        }

#if CONFIG_IDF_TARGET_ESP32P4
        if (seg.load_addr >= 0x48000000 && seg.load_addr < 0x4C000000) {
            uint64_t end64 = (uint64_t)(seg.load_addr - 0x48000000) + seg.data_len;
            if (end64 > UINT32_MAX) {
                ESP_LOGE(TAG, "Segment %d end overflows. Halting.", i);
                IMAGE_FATAL_RETURN();
            }
            uint32_t end = (uint32_t)end64;
            if (end > max_offset) max_offset = end;
        }
#elif CONFIG_IDF_TARGET_ESP32S3
        if ((seg.load_addr >= 0x42000000 && seg.load_addr < 0x44000000) ||
            (seg.load_addr >= 0x3C000000 && seg.load_addr < 0x3E000000)) {
            uint64_t end64 = (uint64_t)(seg.load_addr & SOC_MMU_LINEAR_ADDR_MASK) + seg.data_len;
            if (end64 > UINT32_MAX) {
                ESP_LOGE(TAG, "Segment %d end overflows. Halting.", i);
                IMAGE_FATAL_RETURN();
            }
            uint32_t end = (uint32_t)end64;
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
            IMAGE_FATAL_RETURN();
        }
        memset(plan.fake_flash, 0, max_offset);

        mmu_target_t target;
        if (esp_mmu_vaddr_to_paddr(plan.fake_flash, &plan.flash_paddr, &target) != ESP_OK) {
            ESP_LOGE(TAG, "paddr lookup for fake_flash failed. Halting.");
            IMAGE_FATAL_RETURN();
        }
        ESP_LOGI(TAG, "fake_flash: vaddr=%p paddr=0x%08lX",
                 plan.fake_flash, (unsigned long)plan.flash_paddr);
        plan.max_offset = max_offset;
    }

    // Step 6: second pass — place segments and build MMU map.
    offset = sizeof(esp_image_header_t);
    uint32_t last_page_start = 0xFFFFFFFF;

    for (int i = 0; i < hdr.segment_count; i++) {
        if (offset + sizeof(esp_image_segment_header_t) > fw_size - payload_offset) {
            ESP_LOGE(TAG, "Segment %d header out of bounds (2nd pass). Halting.", i);
            IMAGE_FATAL_RETURN();
        }
        esp_image_segment_header_t seg;
        memcpy(&seg, psram_buf + payload_offset + offset, sizeof(seg));
        offset += sizeof(seg);
        if (seg.data_len > MAX_FIRMWARE_SIZE) {
            ESP_LOGE(TAG, "Segment %d bad length %lu (2nd pass). Halting.",
                     i, (unsigned long)seg.data_len);
            IMAGE_FATAL_RETURN();
        }
        if ((size_t)seg.data_len > fw_size - payload_offset - offset) {
            ESP_LOGE(TAG, "Segment %d data out of bounds (2nd pass). Halting.", i);
            IMAGE_FATAL_RETURN();
        }

        ESP_LOGI(TAG, "Seg %d: addr=0x%08lX len=%lu",
                 i, (unsigned long)seg.load_addr, (unsigned long)seg.data_len);

#if CONFIG_IDF_TARGET_ESP32P4
        if (seg.load_addr >= 0x48000000 && seg.load_addr < 0x4C000000) {
            if (plan.mmu_mapping_count >= MAX_MMU_MAPPINGS) {
                ESP_LOGE(TAG, "Exceeded MAX_MMU_MAPPINGS (%d). Halting.", MAX_MMU_MAPPINGS);
                IMAGE_FATAL_RETURN();
            }
            // Mapped segment → stage into fake_flash
            uint64_t va_end64 = (uint64_t)seg.load_addr + seg.data_len + 0xFFFFu - 1u;
            uint32_t va_start = seg.load_addr & ~0xFFFF;
            uint32_t va_end   = (uint32_t)(va_end64 & ~(uint64_t)0xFFFF);
            if (seg.data_len == 0) va_end = va_start;

            mmu_mapping_t *m = &plan.mmu_mappings[plan.mmu_mapping_count];
            m->vaddr = va_start;
            m->len   = va_end - va_start;
            m->paddr = plan.flash_paddr + (va_start - 0x48000000);

            uint32_t write_off = seg.load_addr - 0x48000000;
            if (seg.data_len > plan.max_offset ||
                write_off > plan.max_offset - seg.data_len) {
                ESP_LOGE(TAG, "Segment %d write exceeds fake_flash buffer. Halting.", i);
                IMAGE_FATAL_RETURN();
            }
            memcpy(plan.fake_flash + write_off, psram_buf + payload_offset + offset, seg.data_len);

            // Stage the image header (32 B) at the mapped page start, as a real flash
            // boot would see it: system_early_init() reads esp_image_header_t from
            // &_rodata_reserved_start - 32, which lands in this mapped window.
            uint32_t page_start = write_off & ~0xFFFF;
            if (page_start != last_page_start) {
                memcpy(plan.fake_flash + page_start, psram_buf + payload_offset, 32);
                last_page_start = page_start;
            }
            plan.mmu_mapping_count++;
            ESP_LOGI(TAG, "  -> fake_flash+0x%lX", (unsigned long)write_off);
        } else if ((seg.load_addr >= 0x4FF00000 && seg.load_addr < 0x4FFC0000) ||
                   /* TCM .tcm.text (SOC_SPM 0x30100000-0x30102000, 8KB: rtc_clk_mpll_*) */
                   (seg.load_addr >= 0x30100000 && seg.load_addr < 0x30102000) ||
                   /* LP RAM (SOC_LP_RAM 0x50108000-0x50110000, 32KB: rtc_noinit/sleep) */
                   (seg.load_addr >= 0x50108000 && seg.load_addr < 0x50110000)) {
            if (plan.copy_count >= MAX_DIRECT_COPIES) {
                ESP_LOGE(TAG, "Exceeded MAX_DIRECT_COPIES (%d). Halting.", MAX_DIRECT_COPIES);
                IMAGE_FATAL_RETURN();
            }
            // Direct segment → deferred copy into internal SRAM/TCM/LP-RAM
            pending_copy_t *c = &plan.direct_copies[plan.copy_count];
            c->dest = (void *)(uintptr_t)seg.load_addr;
            c->src  = (void *)(psram_buf + payload_offset + offset);
            c->len  = seg.data_len;
            plan.copy_count++;
            ESP_LOGI(TAG, "  -> direct copy to 0x%08lX", (unsigned long)seg.load_addr);
        } else {
            ESP_LOGE(TAG, "Segment %d load addr 0x%08lX outside recognized P4 memory windows. Halting.",
                     i, (unsigned long)seg.load_addr);
            IMAGE_FATAL_RETURN();
        }

#elif CONFIG_IDF_TARGET_ESP32S3
        bool is_irom = (seg.load_addr >= 0x42000000 && seg.load_addr < 0x44000000);
        bool is_drom = (seg.load_addr >= 0x3C000000 && seg.load_addr < 0x3E000000);

        if (is_irom || is_drom) {
            if (plan.mmu_mapping_count >= MAX_MMU_MAPPINGS) {
                ESP_LOGE(TAG, "Exceeded MAX_MMU_MAPPINGS (%d). Halting.", MAX_MMU_MAPPINGS);
                IMAGE_FATAL_RETURN();
            }
            // Mapped segment → stage into fake_flash at its linear offset
            uint32_t linear   = seg.load_addr & SOC_MMU_LINEAR_ADDR_MASK;
            uint64_t va_end64 = (uint64_t)seg.load_addr + seg.data_len + 0xFFFFu - 1u;
            uint32_t va_start = seg.load_addr & ~0xFFFF;
            uint32_t va_end   = (uint32_t)(va_end64 & ~(uint64_t)0xFFFF);
            if (seg.data_len == 0) va_end = va_start;

            mmu_mapping_t *m = &plan.mmu_mappings[plan.mmu_mapping_count];
            m->vaddr = va_start;
            m->len   = va_end - va_start;
            m->paddr = plan.flash_paddr + (va_start & SOC_MMU_LINEAR_ADDR_MASK);

            uint32_t write_off = linear;
            if (seg.data_len > plan.max_offset ||
                write_off > plan.max_offset - seg.data_len) {
                ESP_LOGE(TAG, "Segment %d write exceeds fake_flash buffer. Halting.", i);
                IMAGE_FATAL_RETURN();
            }
            memcpy(plan.fake_flash + write_off, psram_buf + payload_offset + offset, seg.data_len);

            // Stage the image header (32 B) at the mapped page start, as a real flash
            // boot would see it: system_early_init() reads esp_image_header_t from
            // &_rodata_reserved_start - 32, which lands in this mapped window.
            uint32_t page_start = write_off & ~0xFFFF;
            if (page_start != last_page_start) {
                memcpy(plan.fake_flash + page_start, psram_buf + payload_offset, 32);
                last_page_start = page_start;
            }
            plan.mmu_mapping_count++;
            ESP_LOGI(TAG, "  -> fake_flash+0x%lX", (unsigned long)write_off);
        } else if ((seg.load_addr >= 0x40370000 && seg.load_addr < 0x403E0000) ||
                   (seg.load_addr >= 0x3FC88000 && seg.load_addr < 0x3FD00000) ||
                   (seg.load_addr >= 0x50000000 && seg.load_addr < 0x50020000) ||
                   (seg.load_addr >= 0x600FE000 && seg.load_addr < 0x60100000)) {
            if (plan.copy_count >= MAX_DIRECT_COPIES) {
                ESP_LOGE(TAG, "Exceeded MAX_DIRECT_COPIES (%d). Halting.", MAX_DIRECT_COPIES);
                IMAGE_FATAL_RETURN();
            }
            // Direct segment → check collision with loader JMP zone
            uint64_t seg_end64 = (uint64_t)seg.load_addr + seg.data_len;
            if (seg_end64 > UINT32_MAX) {
                ESP_LOGE(TAG, "Segment %d end overflows. Halting.", i);
                IMAGE_FATAL_RETURN();
            }
            uint32_t seg_end = (uint32_t)seg_end64;
            if (seg.load_addr < JMP_ZONE_BASE && seg_end > JMP_ZONE_BASE) {
                ESP_LOGE(TAG, "Segment %d (0x%08lX-0x%08lX) collides with loader's JMP zone (0x%08X-0x%08X). Halting.",
                         i, (unsigned long)seg.load_addr, (unsigned long)seg_end,
                         JMP_ZONE_BASE, JMP_ZONE_END);
                IMAGE_FATAL_RETURN();
            }
            if (seg.load_addr >= JMP_ZONE_BASE && seg.load_addr < JMP_ZONE_END) {
                ESP_LOGE(TAG, "Segment %d starts inside loader's JMP zone. Halting.", i);
                IMAGE_FATAL_RETURN();
            }

            pending_copy_t *c = &plan.direct_copies[plan.copy_count];
            c->dest = (void *)(uintptr_t)seg.load_addr;
            c->src  = (void *)(psram_buf + payload_offset + offset);
            c->len  = seg.data_len;
            plan.copy_count++;
            ESP_LOGI(TAG, "  -> direct copy to 0x%08lX", (unsigned long)seg.load_addr);
        } else {
            ESP_LOGE(TAG, "Segment %d load addr 0x%08lX outside recognized S3 memory windows. Halting.",
                     i, (unsigned long)seg.load_addr);
            IMAGE_FATAL_RETURN();
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
