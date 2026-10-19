#!/usr/bin/env bash
# ==============================================================================
# run_emu_ci.sh — full stateless boot chain on esp-emu, down to a live
# MicroPython REPL, checked headlessly (CI variant of Phase_21's run_emu.sh).
#
# esp-emu models no SD card, so the signed Specter bundle rides in raw flash at
# 0x13FF00 via the emulation-only overlay `emu-flash-payload.patch` applied to
# a scratch copy of this repo (the repo itself is never modified). The scratch
# image is then booted in esp-emu, CR-terminated MicroPython commands are
# injected into the REPL, and `check_repl.py` fails the run unless every
# expected UART marker (loader signature check, MicroPython banner, evaluated
# command output) is present in the log.
#
# Usage:
#   tools/emu/run_emu_ci.sh build s3|p4    # builds + signs + merges (idf.py,
#                                          # esptool, ecdsa/bech32 required)
#   tools/emu/run_emu_ci.sh run   s3|p4    # boots the merged image (esp-emu
#                                          # on PATH) and checks the REPL
#   tools/emu/run_emu_ci.sh all   s3|p4    # build + run locally
#
# P4 uses the rev-1.3 route that matches the physical boards: production-
# defaults loader + `--rom` esp32p4_rev0_rom.elf + `--efuse` rev-1.3 blob.
# esp-emu's default P4 model is rev 3.1 / eco5, where pre-v3 images (the stock
# seedsigner-micropython-builder build) do not execute — see Phase 21 §4.6.
#
# Env:
#   REPO_ROOT    loader repo root (default: this script's ../..)
#   SCRATCH      scratch dir (default /tmp/emu_ci); `run` reads the artifacts
#                `build` wrote there, so the two must agree when split across
#                steps/containers
#   PAYLOAD_BIN  raw ESP image to sign & boot (required for build/all)
#   P4_REV0_ROM  esp32p4_rev0_rom.elf (required for p4 run/all; build does not
#                need it)
#   EMU_TIMEOUT  esp-emu --timeout value (default 900s; --exit-on normally
#                ends the run much earlier)
#   EMU_INJECT / EMU_INJECT_TRIGGER / EMU_EXIT_ON  REPL probe overrides
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
SCRATCH="${SCRATCH:-/tmp/emu_ci}"
EMU_TIMEOUT="${EMU_TIMEOUT:-900s}"
EMU_BUNDLE_OFF=0x13FF00   # Specter header; ESP image lands at 0x140000 (64K-aligned)

# Injected REPL probe. The markers ('SYS esp32', 'CI-REPL 4') only appear after
# MicroPython *evaluates* the lines: the echoed input text does not contain
# them, so --exit-on cannot fire on an echo. The exit-on marker must come from
# the LAST line so the emulator does not exit before the earlier outputs are
# transmitted. Lines are CR-terminated because MicroPython's readline executes
# on '\r' (a terminal Enter sends '\n', which is swallowed — Phase 21 E6).
# The payload carries *literal* \r / \n escapes (esp-emu interprets them);
# ${VAR:-...} does NOT strip inline quotes, so defaults live in plain
# assignments that are expanded without any inline quoting.
EMU_INJECT_DEFAULT="import sys\r\nprint('SYS', sys.platform)\r\nprint('CI' + '-REPL', 2+2)\r\n"
EMU_INJECT="${EMU_INJECT:-$EMU_INJECT_DEFAULT}"
EMU_INJECT_TRIGGER_DEFAULT='>>> '
EMU_INJECT_TRIGGER="${EMU_INJECT_TRIGGER:-$EMU_INJECT_TRIGGER_DEFAULT}"
EMU_EXIT_ON_DEFAULT='CI-REPL 4'
EMU_EXIT_ON="${EMU_EXIT_ON:-$EMU_EXIT_ON_DEFAULT}"

log() { echo "[emu] $*"; }
die() { echo "[emu] ERROR: $*" >&2; exit 1; }

need_idf() {
    if ! command -v idf.py >/dev/null 2>&1; then
        # shellcheck disable=SC1091
        for candidate in \
            "${IDF_PATH:-__none__}/export.sh" \
            "$HOME/esp/esp-idf-v5.5/export.sh" \
            "$HOME/esp/esp-idf/export.sh"; do
            [ -f "$candidate" ] && { . "$candidate" >/dev/null 2>&1 || true; break; }
        done
    fi
    command -v idf.py >/dev/null 2>&1 || die "idf.py not found — source ESP-IDF export.sh first"
}

prep_scratch() {
    log "scratch copy: $REPO_ROOT -> $SCRATCH (repo itself untouched)"
    rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
    local f
    for f in main components keys tools CMakeLists.txt partitions_esp32s3.csv \
             partitions_esp32p4.csv sdkconfig.defaults sdkconfig.defaults.esp32s3 \
             sdkconfig.defaults.esp32p4 secure_boot_signing_key.pem \
             payload_signing_key.pem; do
        [ -e "$REPO_ROOT/$f" ] && cp -r "$REPO_ROOT/$f" "$SCRATCH/"
    done
    patch -d "$SCRATCH" -p1 < "$SCRIPT_DIR/emu-flash-payload.patch" \
        || die "overlay patch failed to apply (loader source drifted?)"
}

ensure_keys() {
    # A fresh CI checkout has neither key (both are gitignored). Generate a
    # matching pair the loader build and the payload signing both use.
    if [ ! -f "$SCRATCH/payload_signing_key.pem" ]; then
        log "generating ephemeral payload signing key + keys/test/vendor_keys.c"
        ( cd "$SCRATCH" && python3 tools/generate_vendor_key.py --profile test -f )
    fi
    if [ ! -f "$SCRATCH/secure_boot_signing_key.pem" ]; then
        # esptool's default is ECDSA-P256 (version 1); this repo selects
        # CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME, so the key MUST be RSA-3072
        # (--version 2) or the 2nd-stage bootloader rejects the app with
        # "RSA signing scheme selected but signature block generated for
        # ECDSA scheme" (S3 + P4 alike).
        log "generating ephemeral RSA-3072 secure boot signing key"
        espsecure.py generate_signing_key --version 2 "$SCRATCH/secure_boot_signing_key.pem"
    fi
}

build_target() {
    local t="$1" idf="$2" platform="$3" bl="$4" pt="$5" ld="$6"
    [ -n "${PAYLOAD_BIN:-}" ] || die "Set PAYLOAD_BIN=<raw ESP image>"
    [ -f "$PAYLOAD_BIN" ] || die "Missing PAYLOAD_BIN: $PAYLOAD_BIN"
    need_idf
    prep_scratch
    # E8: call_start_cpu0-path payloads need the 32-byte image-header staging
    # (dropped in 354e0bf, restored upstream in 0361736).
    grep -q "last_page_start" "$SCRATCH/main/esp_image.c" \
        || die "loader source predates the E8 header-staging fix (upstream 0361736)"
    ensure_keys

    local B="$SCRATCH/build-$t"
    export EMU_FLASH_PAYLOAD=1   # CMake hook: bundle from flash, skip anti-phish fill
    log "building loader for $idf (EMU_FLASH_PAYLOAD overlay) ..."
    idf.py -C "$SCRATCH" -B "$B" -D IDF_TARGET="$idf" -D "SDKCONFIG=$B/sdkconfig" \
        -D "SDKCONFIG_DEFAULTS=$SCRATCH/sdkconfig.defaults;$SCRATCH/sdkconfig.defaults.$idf" \
        reconfigure
    idf.py -C "$SCRATCH" -B "$B" build

    log "signing payload: $(basename "$PAYLOAD_BIN")"
    python3 "$SCRATCH/tools/generate_signed_payload.py" --platform "$platform" \
        --profile test --key "$SCRATCH/payload_signing_key.pem" \
        "$PAYLOAD_BIN" "$SCRATCH/bundle_$t.bin"

    log "merging flash image (bundle @ $EMU_BUNDLE_OFF) ..."
    python3 -m esptool --chip "$idf" merge_bin -o "$SCRATCH/emu_$t.bin" \
        --flash_mode keep --flash_freq keep --flash_size keep \
        "$bl" "$B/bootloader/bootloader.bin" \
        "$pt" "$B/partition_table/partition-table.bin" \
        "$ld" "$B/seedsigner_secure_loader.bin" \
        "$EMU_BUNDLE_OFF" "$SCRATCH/bundle_$t.bin"

    if [ "$t" = "p4" ]; then
        # Fresh rev-1.3 virtual-eFuse blob = clean first-burn (esp-emu saves
        # eFuse state back on exit, so never reuse a blob across runs).
        python3 "$SCRIPT_DIR/make-efuse.py" --chip esp32p4 --chip-rev 1.3 \
            -o "$SCRATCH/efuse_p4_rev1.3.bin"
    fi
    log "merged image ready: $SCRATCH/emu_$t.bin"
}

run_target() {
    local t="$1" idf="$2" psram="$3"
    command -v esp-emu >/dev/null 2>&1 || die "esp-emu not found on PATH"
    local fw="$SCRATCH/emu_$t.bin"
    [ -f "$fw" ] || die "missing $fw — run 'build $t' first"
    local logf="$SCRATCH/emu_$t.log"

    local -a args=(--chip "$idf" --firmware "$fw" --psram-size "$psram"
                   --timeout "$EMU_TIMEOUT"
                   --inject "$EMU_INJECT" --inject-on "$EMU_INJECT_TRIGGER"
                   --exit-on "$EMU_EXIT_ON")
    if [ "$t" = "p4" ]; then
        [ -n "${P4_REV0_ROM:-}" ] || die "Set P4_REV0_ROM=<esp32p4_rev0_rom.elf> for the rev-1.3 route"
        [ -f "$P4_REV0_ROM" ] || die "Missing P4_REV0_ROM: $P4_REV0_ROM"
        [ -f "$SCRATCH/efuse_p4_rev1.3.bin" ] || die "missing eFuse blob — run 'build p4' first"
        args+=(--rom "$P4_REV0_ROM" --efuse "$SCRATCH/efuse_p4_rev1.3.bin")
    fi

    log "booting $fw (timeout $EMU_TIMEOUT, exit-on '$EMU_EXIT_ON') ..."
    local rc
    set +e
    esp-emu "${args[@]}" 2>&1 | tee "$logf"
    rc=${PIPESTATUS[0]}
    set -e
    log "esp-emu exit code: $rc (log: $logf)"

    python3 "$SCRIPT_DIR/check_repl.py" "$logf" \
        "Signature verification PASSED!" \
        "MicroPython v" \
        "$EMU_EXIT_ON" \
        "SYS esp32"
}

TARGET="${2:-}"
case "$TARGET" in
    s3|esp32s3) TARGET=s3; TARGET_IDF=esp32s3; PLATFORM=seedsigner_esp32s3
                BL=0x0; PT=0x10000; LD=0x20000; PSRAM=8M ;;
    p4|esp32p4) TARGET=p4; TARGET_IDF=esp32p4; PLATFORM=seedsigner_esp32p4
                BL=0x2000; PT=0x20000; LD=0x30000; PSRAM=32M ;;
    *) die "usage: $0 {build|run|all} {s3|p4}";;
esac

case "${1:-}" in
    build) build_target "$TARGET" "$TARGET_IDF" "$PLATFORM" "$BL" "$PT" "$LD" ;;
    run)   run_target "$TARGET" "$TARGET_IDF" "$PSRAM" ;;
    all)   build_target "$TARGET" "$TARGET_IDF" "$PLATFORM" "$BL" "$PT" "$LD"
           run_target "$TARGET" "$TARGET_IDF" "$PSRAM" ;;
    *) die "usage: $0 {build|run|all} {s3|p4}";;
esac
