#!/usr/bin/env bash
# ==============================================================================
# Build & Flash Loader Script
# Target  : ESP32-P4 Stateless Secure Loader
# Payload : Specter-signed firmware bundle read from the SD card
#           (/sdcard/seedsigner_esp32p4.bin). Build + sign the payload first
#           (e.g. with tools/generate_signed_payload.py), copy the signed file
#           to a FAT32 SD card root, then run this script. The loader mounts the
#           SD card at boot, reads + verifies the bundle, unmounts, and executes
#           from PSRAM. This script only builds and flashes the loader artifacts
#           (bootloader, partition table, loader app) — it is payload-agnostic.
# ==============================================================================
set -uo pipefail

# ------------------------------------------------------------------------------
# Logging
# ------------------------------------------------------------------------------
log_info()    { echo -e "\033[1;34m[INFO]\033[0m $*"; }
log_success() { echo -e "\033[1;32m[PASS]\033[0m $*"; }
log_warn()    { echo -e "\033[1;33m[WARN]\033[0m $*"; }
log_error()   { echo -e "\033[1;31m[FAIL]\033[0m $*" >&2; }
log_step()    { echo -e "\n\033[1;35m=== $* ===\033[0m"; }

# ------------------------------------------------------------------------------
# Path resolution
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -d "$SCRIPT_DIR/seedsigner_bootloader_p4_stateless_os" ]; then
    ROOT_DIR="$SCRIPT_DIR"
    BOOTLOADER_DIR="$SCRIPT_DIR/seedsigner_bootloader_p4_stateless_os"
elif [ -f "$SCRIPT_DIR/CMakeLists.txt" ]; then
    BOOTLOADER_DIR="$SCRIPT_DIR"
    ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
else
    log_error "Cannot locate project directories from $SCRIPT_DIR"
    exit 1
fi

# ------------------------------------------------------------------------------
# ESP-IDF environment
# ------------------------------------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    log_info "idf.py not in PATH — sourcing ESP-IDF..."
    for candidate in \
        "${IDF_PATH:-__none__}/export.sh" \
        "$HOME/esp/esp-idf-v5.5/export.sh" \
        "$HOME/esp/esp-idf/export.sh"; do
        [ -f "$candidate" ] && { . "$candidate" >/dev/null 2>&1 || true; break; }
    done
fi
if ! command -v idf.py >/dev/null 2>&1; then
    log_error "idf.py not found. Source ESP-IDF export.sh first."
    exit 1
fi

# ------------------------------------------------------------------------------
# Configuration
# ------------------------------------------------------------------------------
PORT="${ESPPORT:-/dev/ttyACM0}"
BAUD="${ESPBAUD:-921600}"

# ------------------------------------------------------------------------------
# Step 0: Pre-flight safety — Secure Boot V2 is enabled, so flashing must be
# virtual-eFuse-protected. Abort before any physical eFuse can be burned.
# ------------------------------------------------------------------------------
log_step "Step 0: Pre-Flight Safety Verification (Virtual eFuse Protection)"
check_virtual_efuse() {
    local cfg="$1"
    local label="$2"
    if [ -f "$cfg" ]; then
        if grep -q "CONFIG_EFUSE_VIRTUAL=y" "$cfg"; then
            log_info "  - $label ($cfg): CONFIG_EFUSE_VIRTUAL=y verified."
        else
            log_error "CRITICAL SAFETY VIOLATION: CONFIG_EFUSE_VIRTUAL=y NOT set in $cfg!"
            log_error "Flashing aborted to prevent potential physical eFuse modification."
            exit 1
        fi
    else
        log_warn "  - Config file $cfg not found, skipping check."
    fi
}
check_virtual_efuse "$BOOTLOADER_DIR/sdkconfig.defaults" "Bootloader (defaults)"
check_virtual_efuse "$BOOTLOADER_DIR/sdkconfig" "Bootloader (sdkconfig)"

# ------------------------------------------------------------------------------
# Step 1: Build loader
# ------------------------------------------------------------------------------
log_step "Step 1: Build loader (seedsigner_bootloader_p4_stateless_os)"
( cd "$BOOTLOADER_DIR" && idf.py build )

BOOTLOADER_BIN="$BOOTLOADER_DIR/build/bootloader/bootloader.bin"
PARTITION_BIN="$BOOTLOADER_DIR/build/partition_table/partition-table.bin"
LOADER_BIN="$BOOTLOADER_DIR/build/seedsigner_secure_loader.bin"

for bin in "$BOOTLOADER_BIN" "$PARTITION_BIN" "$LOADER_BIN"; do
    if [ ! -f "$bin" ] || [ ! -s "$bin" ]; then
        log_error "Missing build artifact: $bin"
        exit 1
    fi
done
log_success "Bootloader build artifacts verified."

# ------------------------------------------------------------------------------
# Step 2: Flash everything
# ------------------------------------------------------------------------------
log_step "Step 2: Flash to $PORT at ${BAUD} baud"
if [ ! -e "$PORT" ]; then
    log_error "Port $PORT not found. Connect the board or set ESPPORT."
    exit 1
fi

# Flash the bootloader, partition table, and the loader app only. The signed
# payload is NOT flashed — it boots from the SD card.
esptool.py --chip esp32p4 --port "$PORT" --baud "$BAUD" \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_freq 40m --flash_size 8MB \
    0x2000 "$BOOTLOADER_BIN" \
    0x20000 "$PARTITION_BIN" \
    0x30000 "$LOADER_BIN"

log_success "Flash complete."
log_warn "Reminder: put seedsigner_esp32p4.bin on the SD card root before booting."

# ------------------------------------------------------------------------------
# Step 3: Serial capture (timeout 30s)
# ------------------------------------------------------------------------------
log_step "Step 3: Serial capture"
( cd "$BOOTLOADER_DIR" && idf.py -p "$PORT" monitor )

exit 0
