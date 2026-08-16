/*
 * Stateless loader relocation for ESP32-P4.
 *
 * The stock REV_LESS_V3 memory.ld.in layout places `sram_low` at 0x4FF00000,
 * so the loader's .text/.data/.bss occupy the same L2MEM lines the payload is
 * copied onto. That leaves stale L1 I-cache lines (and dirty D-cache lines) at
 * the payload entry point after the jump, and the ROM's L1 cache ops hang for
 * internal-SRAM ranges (L2 sync engine only covers external memory).
 *
 * Relocate the loader's whole footprint up to 0x4FF40000+, clear of:
 *   - payload copy region           0x4FF00000 - 0x4FF20000
 *   - stock IDF bootloader window   0x4FF29ED0 - 0x4FF352C0
 *   - ROM bss/data/stack            0x4FF3FEDC - 0x4FF3FFF0
 * so the payload's entry line is never fetched by the loader (cold miss), and
 * `evict_buf` ends up above the payload region so it can safely drain the
 * D-cache after the copies.
 *
 * GNU ld processes MEMORY redefinitions in script order: a later declaration of
 * the same region name overrides the earlier one (only a "redeclaration of
 * memory region" warning is emitted). This script must therefore be linked
 * after esp_system's memory.ld — it is, because this component is linked after
 * esp_system on the final link line.
 */
MEMORY
{
  sram_low (RWX) : org = 0x4FF40000, len = 0x60000
  sram_high (RW) : org = 0x4FFA0000, len = 0x0
}
