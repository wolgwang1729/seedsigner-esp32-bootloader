#pragma once

#include "bl_signature.h"

// Minimum number of valid signatures required before the loader boots a
// payload. This deployment authorizes exactly one vendor key (single-key
// multisig), so at least 1 valid signature is required. blsig_verify_multisig()
// returns the *count* of valid signatures (0 = none recognized) and only uses
// negative values for errors, so the caller MUST enforce a lower bound itself —
// blsig_is_error() alone would accept 0 as success (upstream Specter enforces
// this per-keyset threshold in bootloader.c verify_multisig(); the loader has
// to replicate it here).
#define SIG_THRESHOLD 1

// Vendor keys — the only keys authorized to sign the payload. Selected at build
// time via the VENDOR_KEYS_PROFILE environment variable (see main/CMakeLists.txt):
//   VENDOR_KEYS_PROFILE=test       -> keys/test/vendor_keys.c   (default)
//   VENDOR_KEYS_PROFILE=production -> keys/production/vendor_keys.c
extern const bl_pubkey_t vendor_keys[];
extern const bl_pubkey_t *pubkeys_boot[];
