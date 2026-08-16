/*
 * Phase 15/17 — stateless loader JMP-zone relocation for the ESP32-S3.
 *
 * The stock esp32s3 memory.ld.in places the loader's IRAM (iram0_0_seg) at
 * 0x40374000 with ICACHE=16KB, which is exactly the region the payload's IRAM
 * segments are copied onto at JMP time. The loader must therefore move its
 * "point of no return" trampoline (do_mmu_mapping_and_jump and friends) out of
 * that region and into a dedicated high-IRAM zone:
 *
 *   iram0_0_seg  org 0x40374000, len 0x20000    (expanded for SDSPI/crypto drivers)
 *   jmp_zone     org 0x403A0000, len 0x18000    (ends 0x403B8000)
 *
 * JMP-zone statics live in DRAM (`.jmp_zone.bss` -> jmp_bss, 0x3FCB8000):
 * - Physical memory model: I/D are aliases (I = D + 0x6F0000). The zone text at
 *   0x403A0000 aliases 0x3FCB0000-0x3FCB8000, so the zone's .bss must stay below
 *   D 0x3FCB0000 or above 0x3FCB8000 so its writes never clobber the zone code.
 * - The internal heap is pinned away by CONFIG_ESP32S3_USE_FIXED_STATIC_RAM_SIZE
 *   =0x40000 (_heap_start = 0x3FCC8000 > 0x3FCB8000), so the heap can never
 *   reclaim the zone's physical alias.
 *
 * GNU ld processes MEMORY redefinitions in script order: a later declaration of
 * the same region name overrides the earlier one. This script is linked after
 * esp_system on the final link line via target_linker_script.
 */
MEMORY
{
  iram0_0_seg (RX) : org = 0x40374000, len = 0x20000
  jmp_zone    (RX) : org = 0x403A0000, len = 0x18000
  /* JMP-zone .bss DRAM home: placed at 0x3FCB8000 (directly above the
   * JMP zone text's D-alias 0x3FCB0000-0x3FCB8000 and well below _heap_start
   * 0x3FCC8000). This guarantees that payload DRAM copies (0x3FC90000-0x3FCA0050)
   * and loader text/data never collide with jump_stack or JMP-zone state. */
  jmp_bss     (RW) : org = 0x3FCB8000, len = 0x8000
}

SECTIONS
{
  .jmp_zone.text :
  {
    /* Xtensa l32r: the literal pool must sit BEFORE the code that uses it
     * (within 256KB below the PC). The compiler emits .jmp_zone.literal for
     * .jmp_zone.text functions — capture it first. */
    *(.jmp_zone.literal .jmp_zone.literal.*)
    KEEP(*(.jmp_zone.text .jmp_zone.text.*))
    . = ALIGN(4);
  } > jmp_zone

  .jmp_zone.bss (NOLOAD) :
  {
    . = ALIGN(16);
    _jmp_zone_bss_start = ABSOLUTE(.);
    KEEP(*(.jmp_zone.stack .jmp_zone.stack.*))
    . = ALIGN(16);
    . += 0x400;   /* window-overflow spill margin above the stack top */
    KEEP(*(.jmp_zone.bss .jmp_zone.bss.*))
    . = ALIGN(8);
    _jmp_zone_bss_end = ABSOLUTE(.);
  } > jmp_bss
}

ASSERT(_jmp_zone_bss_end <= 0x3FCC8000, "JMP-zone .bss reaches heap start. Shrink jump_stack or reduce loader DRAM usage.")
