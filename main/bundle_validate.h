#pragma once

// Testable Specter bundle bounds validation, extracted from main/main.c
// (Review §3.1, fix 67c34ee). Both the loader and host_test link this file:
//  - target: compiled with ESP-IDF esp_err.h
//  - host: compiled with host_test/stubs/esp_err.h
//
// All functions are pure (no logging, no halt, no hardware) so Catch2 can
// exercise corrupt/malicious bundles without silicon.

#include <stddef.h>
#include <stdint.h>

#ifdef HOST_TEST
#include "esp_err.h"
#else
#include "esp_err.h"
#endif

#include "bl_section.h"

#ifdef __cplusplus
extern "C" {
#endif

// Validate that the main section's declared payload fits in the loaded bundle.
// Rejects sizeof(bl_section_t) + pl_size > fw_size (OOB read in
// blsect_hash_over_flash + OOB sig_hdr deref).
esp_err_t bundle_validate_main_size(uint32_t main_pl_size, size_t fw_size);

// Locate + validate the signature section that immediately follows the main
// payload. On success returns ESP_OK and sets *out_sig_hdr. Rejects a
// truncated sig header or a sig payload extending past bundle_end.
esp_err_t bundle_validate_sig_section(const uint8_t *psram_buf, size_t fw_size,
                                       uint32_t main_pl_size,
                                       const bl_section_t **out_sig_hdr);

#ifdef __cplusplus
}
#endif
