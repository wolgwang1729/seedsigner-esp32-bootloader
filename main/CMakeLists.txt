idf_component_register(
    SRCS "main.c" "anti_phish.c" "bip39_wordlist.c"
    PRIV_REQUIRES esp_mm spi_flash esp_partition fatfs sdmmc specter_crypto
)

# Relocate the loader's own segments off the payload's L2MEM region (0x4FF00000)
# up to 0x4FF40000+, so the payload entry line is never I-fetched by the loader
# and the D-cache drain buffer sits clear of the copied image. This script is a
# MEMORY redefinition that must land after esp_system's memory.ld on the link
# line — linked after esp_system, so it does.
target_linker_script(${COMPONENT_LIB} INTERFACE "${CMAKE_CURRENT_LIST_DIR}/ld/loader_high.ld")

# The payload is a Specter-signed firmware bundle read from the SD
# card (/sdcard/seedsigner_esp32p4.bin). The loader verifies the secp256k1
# multisig signature against the vendor keys in main.c before parsing/executing
# the image. The SD card is unmounted immediately after the read (TOCTOU-safe),
# then the loader executes the verified image entirely from PSRAM.
