/** @file ram_reservations.c
 * @brief Restrict runtime heaps to the same SRAM allow-list as the image.
 *
 * Ported from miketlk/specter-bootloader sd_uploader/main/ram_reservations.c
 * (https://github.com/miketlk/specter-bootloader/tree/port_esp32-p4).
 * Pure-RAM startup skips normal cache initialization; this applies the pinned
 * internal SRAM/cache split before using the configured high SRAM heap, then
 * clears high BSS. Required for reliable SD DMA on P4: omitting it can
 * corrupt readback despite apparently valid images.
 */
#include <stdint.h>
#include <string.h>

#include "esp_private/startup_internal.h"
#include "hal/cache_hal.h"
#include "heap_memory_layout.h"
#include "rom/rom_layout.h"
extern int _sdu_reserved_start, _sdu_reserved_end, _sdu_spm_start, _sdu_spm_end;
extern int _sdu_prefix_start, _sdu_prefix_end;
extern uint8_t _bss_start_high[], _bss_end_high[];
SOC_RESERVE_MEMORY_REGION((intptr_t)&_sdu_reserved_start,
                          (intptr_t)&_sdu_reserved_end, sdu_rom_gap);
SOC_RESERVE_MEMORY_REGION((intptr_t)&_sdu_spm_start, (intptr_t)&_sdu_spm_end,
                          sdu_no_spm_heap);
SOC_RESERVE_MEMORY_REGION((intptr_t)&_sdu_prefix_start,
                          (intptr_t)&_sdu_prefix_end, sdu_prefix);
/// Explicit anchor retains the reservation object in the link.
void sdu_reservations_link(void) {}
ESP_SYSTEM_INIT_FN(sdu_reserve, CORE, BIT(0), 090) {
  // Pure-RAM startup skips normal cache initialization. Apply the pinned
  // internal SRAM/cache split before using the configured high SRAM heap.
  cache_hal_init();
  // Reinitialize high BSS now that the configured SRAM is physically usable.
  memset(_bss_start_high, 0,
         (uintptr_t)_bss_end_high - (uintptr_t)_bss_start_high);
  // IDF reserves the ROM table's tail separately; reservations cannot overlap.
  intptr_t rom_start = (intptr_t)ets_rom_layout_p->dram0_rtos_reserved_start;
  if (rom_start <= (intptr_t)&_sdu_reserved_start ||
      rom_start > (intptr_t)&_sdu_reserved_end)
    return ESP_FAIL;
  reserved_region_sdu_rom_gap.end = rom_start;
  return ESP_OK;
}
