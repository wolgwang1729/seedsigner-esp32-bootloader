idf_component_register(
    SRCS
        "src/bl_section.c"
        "src/bl_signature.c"
        "src/bl_util.c"
        "src/bl_integrity_check.c"
        "src/bl_syscalls.c"
        "secp256k1/src/secp256k1.c"
        "crypto/sha2.c"
        "crypto/memzero.c"
        "crc32/crc32.c"
        "bech32/segwit_addr.c"
    INCLUDE_DIRS
        "include"
        "src"
        "crc32"
        "bech32"
        "secp256k1"
        "secp256k1/src"
        "secp256k1/include"
        "crypto"
        "secp256k1_add"
    REQUIRES
        fatfs
    PRIV_REQUIRES
        "bootloader_support"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SECP256K1_BUILD
    HAVE_CONFIG_H
    USE_NUM_NONE
    USE_FIELD_10X26
    USE_FIELD_INV_BUILTIN
    USE_SCALAR_8X32
    USE_SCALAR_INV_BUILTIN
    "__BYTE_ORDER=1234"
    "CRC32_USE_LOOKUP_TABLE_SLICING_BY_8"
)

target_compile_options(${COMPONENT_LIB} PRIVATE 
    "-Wno-error" 
    "-Wno-array-parameter" 
    "-Wno-unused-function"
    "-Wno-implicit-fallthrough"
)
