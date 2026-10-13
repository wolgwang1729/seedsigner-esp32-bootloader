/** @file uploader_storage.h
 * @brief Mount + transactional SD writes for the RAM-only uploader.
 *
 * Reuses the exact pin maps from the stateless loader
 * (main/targets/esp32p4/storage_p4.c, main/targets/esp32s3/storage_s3.c)
 * so the uploader talks to the same card slot the loader boots from.
 * Never formats the card; publish is atomic (.part + double SHA-256
 * readback + rename), mirroring the reference sd_uploader design.
 *
 * This component intentionally never calls flash/partition/OTA/NVS APIs,
 * keeping the RAM image flash-touch-free (enforced by the top-level
 * CMake guard + code review; no audit harness needed for this size).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ssdu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSDU_MOUNT_POINT "/sdcard"
#define SSDU_MAX_FILE (8u * 1024u * 1024u)

/* Device state strings reported in RESP JSON ("state"). */
extern const char *ssdu_state; /* NO_MEDIA | IDLE | RECEIVING | COMMITTED | RELEASED | UNCERTAIN */
extern uint64_t ssdu_offset, ssdu_length, ssdu_capacity;
extern uint8_t ssdu_digest[32];
extern uint8_t ssdu_observed[32];
extern uint64_t ssdu_observed_len;
extern char ssdu_final[SSDU_MAX_NAME + 1];

/** Mount the SD card if needed. Returns SSDU_OK or SSDU_ERR_*. */
ssdu_status_t ssdu_mount(void);

/** Begin a new upload. Creates a reserved temp file; refuses overwrite. */
ssdu_status_t ssdu_begin(const char *name, uint64_t size, const uint8_t sha256[32]);

/** Write one chunk at the current offset. */
ssdu_status_t ssdu_write(uint64_t offset, const uint8_t *data, size_t len);

/** Finish: flush+fsync, verify temp, rename, verify final. */
ssdu_status_t ssdu_commit(void);

/** Abort the in-progress upload, deleting the temp file. */
ssdu_status_t ssdu_abort(void);

/** SHA-256 readback of an existing file; OK only on exact match. */
ssdu_status_t ssdu_verify_existing(const char *name, uint64_t size,
                                   const uint8_t sha256[32]);

/** Delete a regular file. Missing file counts as OK (idempotent). */
ssdu_status_t ssdu_remove(const char *name);

/** Unmount and deinit the SD stack. Does not reset the chip. */
ssdu_status_t ssdu_release(void);

/** Emit a root-directory listing as JSON array into buf (truncated safely). */
void ssdu_list_json(char *buf, size_t cap);

/** Lowercase hex of a 32-byte digest. */
void ssdu_digest_hex(char out[65], const uint8_t d[32]);

#ifdef __cplusplus
}
#endif
