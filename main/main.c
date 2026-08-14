// --- Standard library ---
#include <string.h>

// --- FreeRTOS ---
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// --- ESP-IDF: logging ---
#include "esp_log.h"

// --- Specter secure app loader (specter_crypto component) ---
#include "bl_section.h"
#include "bl_signature.h"
#include "anti_phish.h"

// --- Vendor payload signing keys (keys/<profile>/vendor_keys.c) ---
#include "vendor_keys.h"

// --- Loader modules ---
#include "loader_config.h"
#include "storage.h"
#include "esp_image.h"
#include "jump.h"

// ==================== ESP32-P4 Memory Map ====================
// The ESP32-P4 is a RISC-V dual-core SoC with a unified cache
// address space for both instructions and data (I/D share vaddr).
//
// Internal SRAM (HP L2MEM):  0x4FF00000 - 0x4FFBFFFF (768 KB)
// Flash cache (IROM/DROM):   0x40000000 - 0x44000000 (I/D shared)
// PSRAM cache:               0x48000000 - 0x4C000000 (via MMU)
//
// Key difference from ESP32-S3:
//   - S3: IROM 0x42000000-0x44000000, DROM 0x3C000000-0x3E000000
//   - P4: Both IROM and DROM share 0x40000000-0x44000000
//   - S3: IRAM 0x40370000-0x403D8000, DRAM 0x3FC88000-0x3FCE9000
//   - P4: HP L2MEM 0x4FF00000-0x4FFBFFFF (unified I/D RAM)
//   - S3: Xtensa architecture, P4: RISC-V architecture
// =============================================================

static const char *TAG = "SEEDSIGNER_LOADER";

// Progress callback fed to blsig_verify_multisig (secp256k1 is slow). We yield
// instead of esp_task_wdt_reset() — the main task isn't auto-subscribed to the
// TWDT in IDF v5.
static void crypto_progress_cb(void *ctx, bl_cbarg_t arg, uint32_t total, uint32_t complete)
{
    (void)ctx;
    (void)arg;
    (void)total;
    (void)complete;
    vTaskDelay(1);
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "SeedSigner Loader — ESP32-P4 PSRAM payload");

    size_t fw_size = 0;

    // ----------------------------------------------------------------
    // Step 1: Load the Specter-signed firmware bundle from the SD card into
    // PSRAM. The card is unmounted before verification (TOCTOU-safe: the
    // signature check below runs against the PSRAM-resident copy only).
    // ----------------------------------------------------------------
    uint8_t *psram_buf = load_firmware_from_sd(&fw_size);
    if (!psram_buf) {
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // ----------------------------------------------------------------
    // Step 2: Flush loaded buffer to physical PSRAM (cache coherence)
    // ----------------------------------------------------------------
    extern int Cache_WriteBack_Addr(uint32_t map, uint32_t vaddr, uint32_t size);
    Cache_WriteBack_Addr(0x10, (uint32_t)psram_buf, fw_size);
    Cache_WriteBack_Addr(0x20, (uint32_t)psram_buf, fw_size);

    // ----------------------------------------------------------------
    // Step 2.5: Specter secure app loader — verify the payload signature
    // ----------------------------------------------------------------
    uint32_t payload_offset = 0;
    bl_section_t *main_hdr = (bl_section_t *)psram_buf;
    if (main_hdr->magic == BL_SECT_MAGIC) {
        ESP_LOGI(TAG, "Specter bootloader section detected");

        if (!blsect_validate_header(main_hdr)) {
            ESP_LOGE(TAG, "Invalid Specter section header. Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        char platform_str[BL_ATTR_STR_MAX] = {0};
        if (!blsect_get_attr_str(main_hdr, bl_attr_platform, platform_str,
                                 sizeof(platform_str)) ||
            strcmp(platform_str, "seedsigner_esp32p4") != 0) {
            ESP_LOGE(TAG, "Invalid platform attribute: '%s' (expected seedsigner_esp32p4). Halting.",
                     platform_str);
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "Firmware version: %lu", (unsigned long)main_hdr->pl_ver);
        if (main_hdr->pl_ver < 1) {
            ESP_LOGE(TAG, "Firmware downgrade detected! Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        // Bounds-check the declared payload size against the bundle we actually
        // loaded. blsect_validate_header only enforces the Specter 16 MB cap,
        // not the loaded file size — a forged pl_size would hash and then
        // dereference out of bounds.
        if (sizeof(bl_section_t) + main_hdr->pl_size > fw_size) {
            ESP_LOGE(TAG, "Payload size out of bounds (%lu + %lu > %lu loaded). Halting.",
                     (unsigned long)sizeof(bl_section_t), (unsigned long)main_hdr->pl_size,
                     (unsigned long)fw_size);
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        // Hash the whole main section (header + payload) from PSRAM.
        // blsys_flash_read maps bl_addr_t to a direct pointer.
        bl_hash_t hash_obj;
        if (!blsect_hash_over_flash(main_hdr,
                                    (bl_addr_t)(psram_buf + sizeof(bl_section_t)),
                                    &hash_obj, 0)) {
            ESP_LOGE(TAG, "Hash calculation failed. Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        // Signature section immediately follows the main payload.
        bl_section_t *sig_hdr = (bl_section_t *)(psram_buf + sizeof(bl_section_t) +
                                                 main_hdr->pl_size);
        if (sig_hdr->magic == BL_SECT_MAGIC && blsect_is_signature(sig_hdr)) {
            // Bounds-check the signature section (header + sig payload) too.
            const uint8_t *bundle_end = psram_buf + fw_size;
            if ((uint8_t *)(sig_hdr + 1) + sig_hdr->pl_size > bundle_end) {
                ESP_LOGE(TAG, "Signature section out of bounds. Halting.");
                memset(psram_buf, 0, fw_size);
                while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            uint8_t sig_msg[BL_SIG_MSG_MAX];
            size_t sig_msg_size = sizeof(sig_msg);
            if (blsect_make_signature_message(sig_msg, &sig_msg_size, &hash_obj, 1)) {
                ESP_LOGI(TAG, "Performing secp256k1 multisig verification...");
                bl_set_progress_callback(crypto_progress_cb, NULL);

                int32_t sig_res = blsig_verify_multisig(
                    "secp256k1-sha256",
                    (uint8_t *)sig_hdr + sizeof(bl_section_t), sig_hdr->pl_size,
                    pubkeys_boot, sig_msg, sig_msg_size, 0);

                if (blsig_is_error(sig_res) || sig_res < SIG_THRESHOLD) {
                    if (blsig_is_error(sig_res)) {
                        ESP_LOGE(TAG, "Signature verification failed: %s", blsig_error_text(sig_res));
                    } else {
                        ESP_LOGE(TAG, "Signature verification failed: %d valid signature(s), need %d", sig_res, SIG_THRESHOLD);
                    }
                    ESP_LOGE(TAG, "HALTING execution.");
                    memset(psram_buf, 0, fw_size);
                    while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
                }
                ESP_LOGI(TAG, "Signature verification PASSED!");
            } else {
                ESP_LOGE(TAG, "Failed to build signature message. Halting.");
                memset(psram_buf, 0, fw_size);
                while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        } else {
            ESP_LOGE(TAG, "Signature section missing or invalid. Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        payload_offset = sizeof(bl_section_t);
    } else {
        ESP_LOGE(TAG, "No Specter section header (magic 0x%08lX) — raw images are not "
                      "accepted from the SD card. Halting.",
                 (unsigned long)main_hdr->magic);
        memset(psram_buf, 0, fw_size);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // ----------------------------------------------------------------
    // Anti-phishing proof
    // ----------------------------------------------------------------
    provision_flash_fill();       // no-op if already provisioned
    char words[4][12];
    esp_err_t ap_err = verify_anti_phishing_proof(words);
    if (ap_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "FLASH TAMPERED — halting boot");
        memset(psram_buf, 0, fw_size);
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // ----------------------------------------------------------------
    // Steps 3-6: validate the raw ESP32 image and build the load plan
    //   (header check, MMU footprint measure, fake-flash staging for
    //   PSRAM-mapped segments, direct-to-SRAM copies).
    // NOTE: psram_buf must NOT be freed — deferred copies still point into it.
    // ----------------------------------------------------------------
    image_plan_t plan = esp_image_plan(psram_buf, payload_offset);

    // ----------------------------------------------------------------
    // Step 7: Commit and jump
    // ----------------------------------------------------------------
    safe_entry_addr = plan.entry_addr;
    for (int i = 0; i < plan.mmu_mapping_count; i++) safe_mappings[i] = plan.mmu_mappings[i];
    safe_mapping_count = plan.mmu_mapping_count;
    for (int i = 0; i < plan.copy_count; i++) safe_copies[i] = plan.direct_copies[i];
    safe_copy_count = plan.copy_count;

    ESP_LOGI(TAG, "Jumping to 0x%08lX ...", (unsigned long)safe_entry_addr);
    for (int i = 0; i < safe_copy_count; i++)
        ESP_LOGI(TAG, "  copy[%d]: %p <- %p (%lu B)",
                 i, safe_copies[i].dest, safe_copies[i].src, (unsigned long)safe_copies[i].len);

    vTaskDelay(100 / portTICK_PERIOD_MS);
    do_mmu_mapping_and_jump_trampoline();
}
