// ============================================================================
// SeedSigner Stateless Secure Bootloader (Unified Multi-Target)
// Supports: ESP32-P4 (RISC-V) and ESP32-S3 (Xtensa LX7)
// ============================================================================
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

static const char *TAG = "SEEDSIGNER_LOADER";

// Progress callback fed to blsig_verify_multisig (secp256k1 is compute-intensive).
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
#if CONFIG_IDF_TARGET_ESP32S3
    extern void esp_cpu_intr_disable(uint32_t mask);
    esp_cpu_intr_disable(1 << 7); // ETS_CACHEERR_INUM on ESP32-S3
#endif

    ESP_LOGI(TAG, "SeedSigner Stateless Loader — Target: %s", EXPECTED_PLATFORM);

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
#if CONFIG_IDF_TARGET_ESP32P4
    extern int Cache_WriteBack_Addr(uint32_t map, uint32_t vaddr, uint32_t size);
    Cache_WriteBack_Addr(0x10, (uint32_t)psram_buf, fw_size);
    Cache_WriteBack_Addr(0x20, (uint32_t)psram_buf, fw_size);
#elif CONFIG_IDF_TARGET_ESP32S3
    extern int Cache_WriteBack_Addr(uint32_t vaddr, uint32_t size);
    Cache_WriteBack_Addr((uint32_t)psram_buf, fw_size);
#endif

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
            strcmp(platform_str, EXPECTED_PLATFORM) != 0) {
            ESP_LOGE(TAG, "Invalid platform attribute: '%s' (expected %s). Halting.",
                     platform_str, EXPECTED_PLATFORM);
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        ESP_LOGI(TAG, "Firmware version: %lu", (unsigned long)main_hdr->pl_ver);
        if (main_hdr->pl_ver < 1) {
            ESP_LOGE(TAG, "Firmware downgrade detected! Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        // Hash the whole main section (header + payload) from PSRAM
        bl_hash_t hash_obj;
        if (!blsect_hash_over_flash(main_hdr,
                                    (bl_addr_t)(psram_buf + sizeof(bl_section_t)),
                                    &hash_obj, 0)) {
            ESP_LOGE(TAG, "Hash calculation failed. Halting.");
            memset(psram_buf, 0, fw_size);
            while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        // Signature section immediately follows the main payload
        bl_section_t *sig_hdr = (bl_section_t *)(psram_buf + sizeof(bl_section_t) +
                                                 main_hdr->pl_size);
        if (sig_hdr->magic == BL_SECT_MAGIC && blsect_is_signature(sig_hdr)) {
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
                        ESP_LOGE(TAG, "Signature verification failed: %d valid signature(s), need %d",
                                 (int)sig_res, SIG_THRESHOLD);
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
    // Step 3: Anti-phishing tamper detection
    // ----------------------------------------------------------------
    provision_flash_fill();
    char words[BIP39_WORD_COUNT][12];
    esp_err_t ap_err = verify_anti_phishing_proof(words);
    if (ap_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "FLASH TAMPERED — halting boot");
        memset(psram_buf, 0, fw_size);
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // ----------------------------------------------------------------
    // Steps 4-6: Parse ESP32 image, map PSRAM, and prepare direct copies
    // ----------------------------------------------------------------
    image_plan_t plan = esp_image_plan(psram_buf, payload_offset);

    // ----------------------------------------------------------------
    // Step 7: Commit load plan into safe hand-off structures
    // ----------------------------------------------------------------
    safe_entry_addr = plan.entry_addr;

    for (int i = 0; i < plan.copy_count; i++) {
        safe_copies[i] = plan.direct_copies[i];
    }
    safe_copy_count = plan.copy_count;

    for (int i = 0; i < plan.mmu_mapping_count; i++) {
        safe_mappings[i] = plan.mmu_mappings[i];
    }
    safe_mapping_count = plan.mmu_mapping_count;

#if CONFIG_IDF_TARGET_ESP32S3
    fake_flash_ptr = plan.fake_flash;
    fake_flash_len = plan.max_offset;
#endif

    ESP_LOGI(TAG, "Ready to boot payload at entry 0x%08lX (%d direct copies, %d MMU mappings)",
             (unsigned long)safe_entry_addr, safe_copy_count, safe_mapping_count);

    vTaskDelay(10 / portTICK_PERIOD_MS);

    // ----------------------------------------------------------------
    // Step 8: Jump to payload (Point of No Return)
    // ----------------------------------------------------------------
#if CONFIG_IDF_TARGET_ESP32P4
    do_mmu_mapping_and_jump_trampoline();
#elif CONFIG_IDF_TARGET_ESP32S3
    do_mmu_mapping_and_jump();
#endif
}
