#!/usr/bin/env bash
# ==============================================================================
# Build & Flash Loader Script
# Target  : ESP32-S3 or ESP32-P4 Stateless Secure Loader
# Payload : Specter-signed firmware bundle read from the SD card
#           (/sdcard/seedsigner_esp32s3.bin or /sdcard/seedsigner_esp32p4.bin).
# ==============================================================================
set -uo pipefail

log_info()    { echo -e "\033[1;34m[INFO]\033[0m $*"; }
log_success() { echo -e "\033[1;32m[PASS]\033[0m $*"; }
log_warn()    { echo -e "\033[1;33m[WARN]\033[0m $*"; }
log_error()   { echo -e "\033[1;31m[FAIL]\033[0m $*" >&2; }
log_step()    { echo -e "\n\033[1;35m=== $* ===\033[0m"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOOTLOADER_DIR="$SCRIPT_DIR"

# ------------------------------------------------------------------------------
# Target Selection
# ------------------------------------------------------------------------------
TARGET="${TARGET:-esp32s3}"
ACTION="all"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target|-t)
            TARGET="$2"
            shift 2
            ;;
        esp32s3|esp32p4)
            TARGET="$1"
            shift
            ;;
        build|flash|monitor|all)
            ACTION="$1"
            shift
            ;;
        *)
            echo "Usage: $0 [--target esp32s3|esp32p4] [build|flash|monitor|all]"
            exit 1
            ;;
    esac
done

log_info "Selected target: \033[1;32m$TARGET\033[0m | Action: \033[1;32m$ACTION\033[0m"

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

PORT="${ESPPORT:-/dev/ttyACM0}"
BAUD="${ESPBAUD:-921600}"

# ------------------------------------------------------------------------------
# Step 0: Pre-flight safety — Virtual eFuse Check
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
# Step 1: Set Target & Build Loader
# ------------------------------------------------------------------------------
if [[ "$ACTION" == "build" || "$ACTION" == "all" ]]; then
    log_step "Step 1: Set target to $TARGET and build"
    cd "$BOOTLOADER_DIR"
    
    # Check if target change is needed
    if [ -f "sdkconfig" ]; then
        CURRENT_TARGET=$(grep "CONFIG_IDF_TARGET=" sdkconfig | cut -d'"' -f2 || true)
        if [ "$CURRENT_TARGET" != "$TARGET" ]; then
            log_info "Reconfiguring target from '$CURRENT_TARGET' to '$TARGET'..."
            idf.py set-target "$TARGET"
        fi
    else
        idf.py set-target "$TARGET"
    fi

    idf.py build

    BOOTLOADER_BIN="$BOOTLOADER_DIR/build/bootloader/bootloader.bin"
    PARTITION_BIN="$BOOTLOADER_DIR/build/partition_table/partition-table.bin"
    LOADER_BIN="$BOOTLOADER_DIR/build/seedsigner_secure_loader.bin"

    for bin in "$BOOTLOADER_BIN" "$PARTITION_BIN" "$LOADER_BIN"; do
        if [ ! -f "$bin" ] || [ ! -s "$bin" ]; then
            log_error "Missing build artifact: $bin"
            exit 1
        fi
    done
    log_success "Bootloader build artifacts verified for $TARGET."
fi

# ------------------------------------------------------------------------------
# Step 2: Flash
# ------------------------------------------------------------------------------
if [[ "$ACTION" == "flash" || "$ACTION" == "all" ]]; then
    log_step "Step 2: Flash to $PORT at ${BAUD} baud"
    if [ ! -e "$PORT" ]; then
        log_error "Port $PORT not found. Connect the board or set ESPPORT."
        exit 1
    fi

    BOOTLOADER_BIN="$BOOTLOADER_DIR/build/bootloader/bootloader.bin"
    PARTITION_BIN="$BOOTLOADER_DIR/build/partition_table/partition-table.bin"
    LOADER_BIN="$BOOTLOADER_DIR/build/seedsigner_secure_loader.bin"

    if [ "$TARGET" == "esp32s3" ]; then
        BL_OFFSET="0x0"
        PTABLE_OFFSET="0x10000"
        LOADER_OFFSET="0x20000"
    else
        BL_OFFSET="0x2000"
        PTABLE_OFFSET="0x20000"
        LOADER_OFFSET="0x30000"
    fi

    esptool.py --chip "$TARGET" --port "$PORT" --baud "$BAUD" \
        --before default_reset --after hard_reset write_flash \
        --flash_mode dio --flash_freq 40m --flash_size 8MB \
        "$BL_OFFSET" "$BOOTLOADER_BIN" \
        "$PTABLE_OFFSET" "$PARTITION_BIN" \
        "$LOADER_OFFSET" "$LOADER_BIN"

    log_success "Flash complete for $TARGET."
    if [ "$TARGET" == "esp32s3" ]; then
        log_warn "Reminder: place seedsigner_esp32s3.bin on the SD card root before booting."
    else
        log_warn "Reminder: place seedsigner_esp32p4.bin on the SD card root before booting."
    fi
fi

# ------------------------------------------------------------------------------
# Step 3: Monitor
# ------------------------------------------------------------------------------
if [[ "$ACTION" == "monitor" || "$ACTION" == "all" ]]; then
    log_step "Step 3: Serial monitor"
    cd "$BOOTLOADER_DIR" && idf.py -p "$PORT" monitor
fi

exit 0
