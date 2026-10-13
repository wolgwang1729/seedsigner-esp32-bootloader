# Device firmware for the SeedSigner RAM-only SD uploader.
# ram_reservations.c + the custom high-SRAM layout are ESP32-P4 specific
# (ported from the reference sd_uploader, qualified on P4 rev 1.x only);
# ESP32-S3 uses the stock pure-RAM layout.
idf_build_get_property(target IDF_TARGET)
set(RAM_EXTRA_SRCS "")
if(target STREQUAL "esp32p4")
    list(APPEND RAM_EXTRA_SRCS "ram_reservations.c")
elseif(target STREQUAL "esp32s3")
    list(APPEND RAM_EXTRA_SRCS "cache_stub_s3.c")
endif()
idf_component_register(
    SRCS "uploader_main.c" "ssdu_proto.c" "uploader_storage.c"
         "ram_dma.c" ${RAM_EXTRA_SRCS}
    REQUIRES esp_driver_uart esp_driver_gpio fatfs sdmmc mbedtls esp_timer
    PRIV_REQUIRES esp_driver_sdspi esp_driver_spi esp_mm
)

# Build identity: any source change invalidates stale prebuilt images.
file(GLOB SDU_SOURCES "${CMAKE_CURRENT_LIST_DIR}/*.c" "${CMAKE_CURRENT_LIST_DIR}/*.h"
     "${CMAKE_CURRENT_LIST_DIR}/../ld/*.cmake" "${SDKCONFIG}")
set(IDENTITY "")
foreach(SOURCE IN LISTS SDU_SOURCES)
  file(SHA256 "${SOURCE}" DIGEST)
  string(APPEND IDENTITY "${DIGEST}")
endforeach()
string(SHA256 SDU_BUILD_ID "${IDENTITY}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE SDU_BUILD_ID="${SDU_BUILD_ID}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SDU_SOURCES})

# Internal-only DMA alignment for the pure-RAM SDMMC path (see ram_dma.c).
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_heap_adjust_alignment_to_hw")
