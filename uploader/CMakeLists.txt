# SeedSigner SD Uploader (RAM-only dev app)
#
# Standalone ESP-IDF project that runs entirely from internal SRAM (loaded over
# UART with `esptool load_ram`) and writes only the removable microSD card.
# It never touches flash, eFuses, or the Secure Boot chain.
#
# Architecture ported from miketlk/specter-bootloader `sd_uploader`
# (https://github.com/miketlk/specter-bootloader/tree/port_esp32-p4): same
# developer experience
# (no physical SD-card swapping, high-baud UART transport, atomic .part +
# SHA-256 double-readback publish) and the same RAM-residency machinery
# (PURE_RAM_APP + custom high-SRAM linker layout + heap reservations +
# internal-only DMA wrap). The wire protocol is a leaner binary+JSON framing
# (SSDU v1, see main/ssdu_proto.h) instead of CBOR/SPSD, so the host needs
# nothing beyond what ESP-IDF ships (pyserial + esptool) and the image stays
# small enough for internal SRAM on both ESP32-P4 and ESP32-S3.
#
# Safety properties kept from the reference design:
#   * mount never formats the card
#   * transactional publish: reserved `.part` file, flush+fsync+close checks,
#     independent SHA-256 readback of the temp file, atomic rename (no
#     overwrite), independent SHA-256 readback of the final file
#   * per-chunk CRC-32, filename policy, capacity checks
cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(seedsigner_sd_uploader)

# Guardrail: this project must stay a flash-touching-free pure-RAM app.
# load_ram images must not contain a 2nd-stage bootloader or flash sections,
# and must never enable Secure Boot / flash encryption.
# NOTE: during `idf.py set-target` the first configure pass still sees the
# previous/default IDF_TARGET ("esp32"); target checks only apply afterwards.
if(NOT IDF_TARGET OR IDF_TARGET STREQUAL "esp32")
  set(SDU_TARGET_TRANSITIONAL TRUE)
else()
  set(SDU_TARGET_TRANSITIONAL FALSE)
  if(NOT IDF_TARGET STREQUAL "esp32p4" AND NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "seedsigner_sd_uploader supports esp32p4 and esp32s3 only")
  endif()
endif()
if(NOT CONFIG_APP_BUILD_TYPE_PURE_RAM_APP OR NOT CONFIG_APP_BUILD_TYPE_RAM
   OR NOT CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT OR CONFIG_APP_BUILD_USE_FLASH_SECTIONS
   OR CONFIG_APP_BUILD_BOOTLOADER OR CONFIG_SPIRAM OR CONFIG_SECURE_BOOT
   OR CONFIG_SECURE_FLASH_ENC_ENABLED OR CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS)
  message(FATAL_ERROR "Uploader requires reversible plaintext pure internal RAM configuration")
endif()
if(NOT SDU_TARGET_TRANSITIONAL AND IDF_TARGET STREQUAL "esp32p4" AND NOT CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
  message(FATAL_ERROR "P4 uploader currently supports silicon revision 1.x only")
endif()

if(NOT SDU_TARGET_TRANSITIONAL AND IDF_TARGET STREQUAL "esp32p4")
  # Derive the layout from IDF scripts: high SRAM holds code and buffers,
  # low SRAM holds data. See ld/place-buffers.cmake (ported from the reference
  # sd_uploader; fails closed if the pinned IDF layout changes).
  set(SDU_LD "${CMAKE_BINARY_DIR}/sdu-sections.ld")
  set(IDF_LD "${CMAKE_BINARY_DIR}/esp-idf/esp_system/ld/sections.ld")
  add_custom_command(OUTPUT "${SDU_LD}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${IDF_LD} -DOUTPUT=${SDU_LD}
      -P "${CMAKE_CURRENT_LIST_DIR}/ld/place-buffers.cmake"
    DEPENDS "${IDF_LD}" "${CMAKE_CURRENT_LIST_DIR}/ld/place-buffers.cmake")
  add_custom_target(sdu_linker DEPENDS "${SDU_LD}")
  add_dependencies(sdu_linker __ldgen_output_sections.ld)
  add_dependencies(seedsigner_sd_uploader.elf sdu_linker)
  get_target_property(SYSTEM_OPTIONS __idf_esp_system INTERFACE_LINK_OPTIONS)
  list(TRANSFORM SYSTEM_OPTIONS REPLACE "SHELL:-T sections.ld" "SHELL:-T ${SDU_LD}")
  list(REMOVE_ITEM SYSTEM_OPTIONS "-Wl,--enable-non-contiguous-regions")
  set_property(TARGET __idf_esp_system PROPERTY INTERFACE_LINK_OPTIONS "${SYSTEM_OPTIONS}")
  set_property(TARGET seedsigner_sd_uploader.elf APPEND PROPERTY LINK_DEPENDS "${SDU_LD}")
endif()

# Flash, erase and merge/deployment targets must fail intentionally;
# ignore ESP-IDF's generic post-build flashing suggestions.
add_custom_target(sdu_forbid_deployment
  COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/ld/no-deployment.cmake")
foreach(DEPLOY flash app-flash erase_flash bootloader-flash partition-table-flash encrypted-flash encrypted-app-flash merge-bin)
  if(TARGET ${DEPLOY})
    add_dependencies(${DEPLOY} sdu_forbid_deployment)
  endif()
endforeach()
foreach(UNSAFE ESP_SYSTEM_PMP_IDRAM_SPLIT ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
    ESP_COREDUMP_ENABLE_TO_FLASH SECURE_BOOT_ENABLE_AGGRESSIVE_KEY_REVOKE
    BOOTLOADER_APP_ANTI_ROLLBACK BOOTLOADER_EFUSE_SECURE_VERSION_EMULATE)
  if(CONFIG_${UNSAFE})
    message(FATAL_ERROR "Unsupported RAM uploader configuration: ${UNSAFE}")
  endif()
endforeach()
