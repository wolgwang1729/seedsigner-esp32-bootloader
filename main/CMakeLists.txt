# Vendor keys profile: test (default) or production. Mirrors
# cryptoadvance/specter-bootloader's keys/<profile> layout.
#
# The profile is a CMake CACHE variable, set with -D so that switching it
# always forces a reconfiguration (CMakeCache.txt is a configure dependency):
#   idf.py -DVENDOR_KEYS_PROFILE=production build
if(DEFINED ENV{VENDOR_KEYS_PROFILE} AND NOT DEFINED VENDOR_KEYS_PROFILE)
    set(VENDOR_KEYS_PROFILE "$ENV{VENDOR_KEYS_PROFILE}")
endif()
if(NOT DEFINED VENDOR_KEYS_PROFILE)
    set(VENDOR_KEYS_PROFILE "test")
endif()
set(VENDOR_KEYS_PROFILE "${VENDOR_KEYS_PROFILE}" CACHE STRING "Vendor keys profile (test|production)" FORCE)

if(DEFINED ENV{VENDOR_KEYS_PROFILE} AND NOT "$ENV{VENDOR_KEYS_PROFILE}" STREQUAL "${VENDOR_KEYS_PROFILE}")
    message(WARNING "VENDOR_KEYS_PROFILE env var is '$ENV{VENDOR_KEYS_PROFILE}' but the configured "
                    "profile is '${VENDOR_KEYS_PROFILE}'. The env var only seeds a fresh configure; "
                    "switch profiles with: idf.py -DVENDOR_KEYS_PROFILE=<profile> build")
endif()
message(STATUS "Seedsigner loader: vendor keys profile = ${VENDOR_KEYS_PROFILE}")

set(KEYS_DIR "${CMAKE_CURRENT_LIST_DIR}/../keys")

# Detect current build target
idf_build_get_property(target IDF_TARGET)

set(TARGET_SRCS "")
set(EXTRA_REQUIRES "")

if(target STREQUAL "esp32p4")
    list(APPEND TARGET_SRCS "targets/esp32p4/storage_p4.c" "targets/esp32p4/jump_p4.c")
    set(TARGET_LD "${CMAKE_CURRENT_LIST_DIR}/targets/esp32p4/loader_high.ld")
elseif(target STREQUAL "esp32s3")
    list(APPEND TARGET_SRCS "targets/esp32s3/storage_s3.c" "targets/esp32s3/jump_s3.c")
    list(APPEND EXTRA_REQUIRES "esp_driver_sdspi" "esp_driver_spi" "bootloader_support")
    set(TARGET_LD "${CMAKE_CURRENT_LIST_DIR}/targets/esp32s3/loader_high.ld")
else()
    message(FATAL_ERROR "Unsupported IDF_TARGET '${target}'. Supported targets: esp32p4, esp32s3")
endif()

idf_component_register(
    SRCS "main.c" "esp_image.c" "anti_phish.c" "bip39_wordlist.c"
         ${TARGET_SRCS}
         "${KEYS_DIR}/${VENDOR_KEYS_PROFILE}/vendor_keys.c"
    PRIV_INCLUDE_DIRS "${KEYS_DIR}" "."
    PRIV_REQUIRES esp_mm spi_flash esp_partition fatfs sdmmc specter_crypto ${EXTRA_REQUIRES}
)

# Relocate the loader's own segments / JMP trampoline via target linker script
target_linker_script(${COMPONENT_LIB} INTERFACE "${TARGET_LD}")
