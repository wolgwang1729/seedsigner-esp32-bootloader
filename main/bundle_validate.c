// Specter bundle bounds validation (see bundle_validate.h).
#include "bundle_validate.h"

esp_err_t bundle_validate_main_size(uint32_t main_pl_size, size_t fw_size) {
  if (sizeof(bl_section_t) + (size_t)main_pl_size > fw_size) {
    return ESP_ERR_INVALID_SIZE;
  }
  return ESP_OK;
}

esp_err_t bundle_validate_sig_section(const uint8_t *psram_buf, size_t fw_size,
                                       uint32_t main_pl_size,
                                       const bl_section_t **out_sig_hdr) {
  if (!psram_buf || !out_sig_hdr) {
    return ESP_ERR_INVALID_ARG;
  }
  if (sizeof(bl_section_t) + (size_t)main_pl_size > fw_size) {
    return ESP_ERR_INVALID_SIZE;
  }
  const uint8_t *bundle_end = psram_buf + fw_size;
  const bl_section_t *sig_hdr = (const bl_section_t *)(psram_buf +
                                                       sizeof(bl_section_t) +
                                                       main_pl_size);
  if ((const uint8_t *)(sig_hdr + 1) > bundle_end) {
    return ESP_ERR_INVALID_SIZE;
  }
  if ((size_t)sig_hdr->pl_size > fw_size ||
      (const uint8_t *)(sig_hdr + 1) + (size_t)sig_hdr->pl_size > bundle_end) {
    return ESP_ERR_INVALID_SIZE;
  }
  *out_sig_hdr = sig_hdr;
  return ESP_OK;
}
