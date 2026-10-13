/** @file ssdu_proto.h
 * @brief SeedSigner SD Uploader (SSDU v1) framing.
 *
 * Simplified port of the specter-bootloader `sd_uploader` SPSD/CBOR
 * transport (https://github.com/miketlk/specter-bootloader/tree/port_esp32-p4):
 * same envelope idea
 * (magic + version + length + CRC-32) but plain binary payloads and
 * JSON responses, so the device needs no CBOR decoder and the host
 * needs nothing beyond pyserial + esptool.
 *
 * Wire format (both directions), all multi-byte integers big-endian:
 *
 *   magic  4B  "SSDU"
 *   ver    1B  0x01
 *   type   1B  opcode (host->device 0x01..0x09, device->host 0x81)
 *   len    4B  payload length (0 .. SSDU_MAX_PAYLOAD)
 *   payload len B
 *   crc32  4B  IEEE CRC-32 over (ver | type | len | payload)
 *
 * Host->device opcodes and payload encodings:
 *
 *   0x01 HELLO    empty
 *   0x02 BEGIN    u8 name_len | name[name_len] | u64 BE size | 32B sha256
 *                 (create reserved temp file, refuse if final exists)
 *   0x03 WRITE    u64 BE offset | u32 BE chunk_crc | data...
 *                 (chunk_crc = CRC-32 of data only)
 *   0x04 COMMIT   empty (flush+fsync, SHA-256 temp readback, atomic rename,
 *                 SHA-256 final readback)
 *   0x05 ABORT    empty (close + delete temp, back to IDLE)
 *   0x06 LIST     empty (root directory listing as JSON)
 *   0x07 VERIFY   same encoding as BEGIN (SHA-256 readback of existing file)
 *   0x08 REMOVE   u8 name_len | name[name_len]
 *   0x09 RELEASE  empty (unmount SD, deinit; stays until reset)
 *
 * Device->host:
 *
 *   0x81 RESP     u32 BE status | JSON bytes (no NUL)
 *                 status 0 = OK; see ssdu_status_t for errors.
 *
 * Design notes:
 *   * Bounded and allocation-free on the device: single static frame buffer,
 *     2 s reassembly timeout, resync on bad magic/version/length/CRC.
 *   * Filename policy mirrors the reference: printable ASCII, max 128 chars,
 *     no path separators or FAT-reserved chars, no trailing space/dot,
 *     normal files may not use the reserved `_ssdu_` prefix.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSDU_MAGIC_0 'S'
#define SSDU_MAGIC_1 'S'
#define SSDU_MAGIC_2 'D'
#define SSDU_MAGIC_3 'U'
#define SSDU_VERSION 0x01

/* Host -> device opcodes */
#define SSDU_OP_HELLO   0x01
#define SSDU_OP_BEGIN   0x02
#define SSDU_OP_WRITE   0x03
#define SSDU_OP_COMMIT  0x04
#define SSDU_OP_ABORT   0x05
#define SSDU_OP_LIST    0x06
#define SSDU_OP_VERIFY  0x07
#define SSDU_OP_REMOVE  0x08
#define SSDU_OP_RELEASE 0x09

/* Device -> host */
#define SSDU_OP_RESP 0x81

#define SSDU_CHUNK_MAX 16384
#define SSDU_MAX_PAYLOAD 18432
#define SSDU_MAX_FRAME (SSDU_MAX_PAYLOAD + 14)
#define SSDU_MAX_NAME 128

/* Static file/IO buffers: on P4 they live in the custom high-SRAM
 * `.sdu_buffers` section (before expandable BSS); elsewhere plain .bss. */
#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4
#define SDU_BUFFER __attribute__((section(".sdu_buffers")))
#else
#define SDU_BUFFER
#endif

typedef enum {
    SSDU_OK = 0,
    SSDU_ERR_BAD = 1,
    SSDU_ERR_PATH = 2,
    SSDU_ERR_RANGE = 3,
    SSDU_ERR_OFFSET = 4,
    SSDU_ERR_NO_MEDIA = 5,
    SSDU_ERR_MOUNT = 6,
    SSDU_ERR_SPACE = 7,
    SSDU_ERR_IO = 8,
    SSDU_ERR_DIGEST = 9,
    SSDU_ERR_EXISTS = 10,
    SSDU_ERR_BUSY = 11,
    SSDU_ERR_UNCERTAIN = 12,
} ssdu_status_t;

const char *ssdu_status_str(ssdu_status_t s);

/** IEEE CRC-32 (same polynomial as zlib; chunk and frame checks). */
uint32_t ssdu_crc32(const uint8_t *data, size_t len);

/**
 * Feed one received byte into the reassembly buffer.
 *
 * On success returns true and sets out_type, out_payload and out_len to the
 * complete frame contents (pointers into an internal static buffer, valid
 * until the next accepted frame). Never inspects payload semantics.
 */
bool ssdu_feed(uint8_t byte, int64_t now_us, uint8_t *out_type,
               const uint8_t **out_payload, size_t *out_len);

/** Serialize one frame into out (must hold SSDU_MAX_FRAME bytes). */
size_t ssdu_encode(uint8_t *out, uint8_t type, const uint8_t *payload,
                   size_t len);

/** Filename policy for normal (non-temp) files. */
bool ssdu_valid_name(const char *name);

/**
 * Decimal u64 print (no printf-family dependency: newlib-nano 64-bit
 * formatting is unavailable in this pure-RAM configuration).
 * buf must hold >= 21 bytes. Returns length sans NUL (0 on bad cap).
 */
size_t ssdu_print_u64(char *buf, size_t cap, uint64_t v);

#ifdef __cplusplus
}
#endif
