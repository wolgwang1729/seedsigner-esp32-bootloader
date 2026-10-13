# Derived from miketlk/specter-bootloader
# (https://github.com/miketlk/specter-bootloader/tree/port_esp32-p4)
# platforms/esp32-p4-wifi6-touch-lcd/sd_uploader/ld/place-buffers.cmake.
#
# Rewrites the IDF-generated sections.ld for the pure-RAM uploader:
# ordinary code/constants move to high SRAM (unified I/D memory on P4),
# low SRAM keeps IDF interrupt/startup/data sections, and a `.sdu_buffers`
# section collects the uploader's static buffers before expandable BSS.
# Fails closed if the pinned IDF layout ever changes (see FATAL_ERRORs).
file(READ "${INPUT}" SCRIPT)
set(MARKER "_bss_start_high = ABSOLUTE(.);")
string(FIND "${SCRIPT}" "${MARKER}" POSITION)
if(POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned high SRAM BSS layout changed")
endif()
string(REPLACE "${MARKER}" "${MARKER}\n    *(.sdu_buffers)" SCRIPT "${SCRIPT}")
# High SRAM is unified instruction/data memory. Put ordinary code/constants
# there, retaining low SRAM for the IDF interrupt/startup/data sections.
string(REPLACE "> flash_text_seg" "> dram_high_seg" SCRIPT "${SCRIPT}")
string(REPLACE "> flash_rodata_seg" "> dram_high_seg" SCRIPT "${SCRIPT}")
# Reserve all relocated code/constants before exposing high SRAM to the heap.
string(REGEX MATCH "  \\.dram1\\.heap_start[^}]+} > dram_high_seg" HEAP "${SCRIPT}")
if(NOT HEAP)
  message(FATAL_ERROR "Pinned high SRAM heap marker changed")
endif()
string(REPLACE "${HEAP}" "" SCRIPT "${SCRIPT}")
# Keep initialized startup tables before the high BSS. The ROM's initial
# cache split must not constrain where buffers/heap can grow after cache init.
string(REGEX MATCH "  \\.dram1\\.bss[^}]+} > dram_high_seg" HIGH_BSS "${SCRIPT}")
if(NOT HIGH_BSS)
  message(FATAL_ERROR "Pinned high BSS section changed")
endif()
string(REPLACE "${HIGH_BSS}" "" SCRIPT "${SCRIPT}")
string(REPLACE "  .dram0.heap_start" "${HIGH_BSS}\n  .dram0.heap_start" SCRIPT "${SCRIPT}")
string(REPLACE "  .dram0.heap_start" "${HEAP}\n  .dram0.heap_start" SCRIPT "${SCRIPT}")
string(REPLACE ". = ALIGN(ALIGNOF(.flash.text)) + SIZEOF(.flash.text);" "" SCRIPT "${SCRIPT}")
string(REPLACE ". = ALIGN(0) + 0x20;" "" SCRIPT "${SCRIPT}")
string(REPLACE "> iram_text_seg" "> dram_high_seg" SCRIPT "${SCRIPT}")
foreach(REGION rtc_text_seg rtc_data_seg rtc_slow_seg rtc_force_fast_seg rtc_force_slow_seg rtc_reserved_seg spm_idram_seg)
  string(REPLACE "> ${REGION}" "> dram_seg" SCRIPT "${SCRIPT}")
endforeach()
string(REPLACE "ORIGIN(iram_text_seg)" "ORIGIN(dram_high_seg)" SCRIPT "${SCRIPT}")
string(REPLACE "LENGTH(iram_text_seg)" "LENGTH(dram_high_seg)" SCRIPT "${SCRIPT}")
string(APPEND SCRIPT "\n_sdu_prefix_start = ORIGIN(dram_seg);\n_sdu_prefix_end = _spm_text_start;\n_sdu_reserved_start = ORIGIN(dram_seg) + LENGTH(dram_seg);\n_sdu_reserved_end = ORIGIN(dram_high_seg);\n_sdu_spm_start = ORIGIN(spm_idram_seg);\n_sdu_spm_end = ORIGIN(spm_idram_seg) + LENGTH(spm_idram_seg);\n")
file(WRITE "${OUTPUT}" "${SCRIPT}")
