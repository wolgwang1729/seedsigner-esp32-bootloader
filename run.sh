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
TARGET="${TARGET:-esp32p4}"
ACTION="all"
UPLOAD_FILE=""
UPLOAD_NAME=""
UPLOAD_BAUD="${UPLOAD_BAUD:-921600}"
NO_MONITOR=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target|-t)
            TARGET="$2"
            shift 2
            ;;
        --port|-p)
            ESPPORT="$2"
            shift 2
            ;;
        --baud)
            UPLOAD_BAUD="$2"
            shift 2
            ;;
        --name)
            UPLOAD_NAME="$2"
            shift 2
            ;;
        --no-monitor)
            NO_MONITOR=1
            shift
            ;;
        esp32s3|esp32p4)
            TARGET="$1"
            shift
            ;;
        build|flash|monitor|all|build-uploader|upload-sd)
            ACTION="$1"
            shift
            # upload-sd consumes an optional file argument: ./run.sh upload-sd <file>
            if [[ "$ACTION" == "upload-sd" && $# -gt 0 && "$1" != -* ]]; then
                UPLOAD_FILE="$1"
                shift
            fi
            ;;
        *)
            echo "Usage: $0 [--target esp32s3|esp32p4] [--port PORT] [--no-monitor] [build|flash|monitor|all|build-uploader|upload-sd [FILE] [--name SD_NAME] [--baud BAUD]]"
            echo ""
            echo "  upload-sd pushes a payload straight to the board's SD card over UART"
            echo "  (RAM-only uploader, no card ejection), then resets into the loader"
            echo "  and opens the monitor. --no-monitor skips the monitor. Example:"
            echo "    $0 --target esp32p4 upload-sd seedsigner_esp32p4.bin"
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
# Step 1b: Build RAM-only SD uploader (no flash writes, load_ram image)
# ------------------------------------------------------------------------------
build_uploader() {
    local ub_dir="$BOOTLOADER_DIR/uploader/build/$TARGET"
    local ub_bin="$ub_dir/seedsigner_sd_uploader.bin"
    log_step "Build SD uploader (RAM-only) for $TARGET"
    mkdir -p "$ub_dir"
    # sdkconfig.defaults already pins APP_BUILD_TYPE_RAM + SDU baud default;
    # UPLOAD_BAUD override is applied via a generated defaults fragment so the
    # host (--baud) and device (CONFIG_SDU_UART_BAUD) always agree.
    # NOTE: all -D paths are absolute: -C makes relative paths resolve under
    # uploader/ and silently breaks kconfgen otherwise. The fragment lives
    # OUTSIDE the build dir because `idf.py set-target` wipes it.
    local baud_defaults="$BOOTLOADER_DIR/uploader/.baud-$TARGET.defaults"
    echo "CONFIG_SDU_UART_BAUD=$UPLOAD_BAUD" > "$baud_defaults"
    local need_target=1
    if [ -f "$ub_dir/sdkconfig" ]; then
        cur=$(grep '^CONFIG_IDF_TARGET=' "$ub_dir/sdkconfig" | cut -d'"' -f2 || true)
        [ "$cur" = "$TARGET" ] && need_target=0
        # Baud change without target change still needs a reconfigure.
        cur_baud=$(grep '^CONFIG_SDU_UART_BAUD=' "$ub_dir/sdkconfig" | cut -d= -f2 || true)
        if [ "$cur_baud" != "$UPLOAD_BAUD" ]; then
            log_info "Baud change ($cur_baud -> $UPLOAD_BAUD): forcing reconfigure."
            rm -f "$ub_dir/CMakeCache.txt"
        fi
    fi
    if [ "$need_target" = "1" ]; then
        idf.py -C "$BOOTLOADER_DIR/uploader" -B "$ub_dir" \
            -D "SDKCONFIG=$ub_dir/sdkconfig" \
            -D "SDKCONFIG_DEFAULTS=$BOOTLOADER_DIR/uploader/sdkconfig.defaults;$BOOTLOADER_DIR/uploader/sdkconfig.defaults.$TARGET;$baud_defaults" \
            set-target "$TARGET"
    fi
    idf.py -C "$BOOTLOADER_DIR/uploader" -B "$ub_dir" build
    if [ ! -f "$ub_bin" ] || [ ! -s "$ub_bin" ]; then
        log_error "Missing uploader artifact: $ub_bin"
        exit 1
    fi
    log_success "Uploader ready: $ub_bin (${UPLOAD_BAUD} baud)."
}

if [[ "$ACTION" == "build-uploader" ]]; then
    build_uploader
    exit 0
fi

# ------------------------------------------------------------------------------
# Step 1c: Push a file to the SD card over UART (no ejection)
# ------------------------------------------------------------------------------
if [[ "$ACTION" == "upload-sd" ]]; then
    if [ -z "$UPLOAD_FILE" ]; then
        # Default to the conventional signed payload for the target.
        UPLOAD_FILE="$BOOTLOADER_DIR/seedsigner_${TARGET}.bin"
    fi
    if [ ! -f "$UPLOAD_FILE" ]; then
        log_error "File not found: $UPLOAD_FILE"
        log_error "Sign one first, e.g.: python3 tools/generate_signed_payload.py <micropython.bin> $UPLOAD_FILE"
        exit 1
    fi
    build_uploader
    log_step "Upload $(basename "$UPLOAD_FILE") to SD via $PORT @ ${UPLOAD_BAUD} baud"
    if [ ! -e "$PORT" ]; then
        log_error "Port $PORT not found. Connect the board or set ESPPORT/--port."
        exit 1
    fi
    UP_ARGS=(--chip "$TARGET" --port "$PORT" --baud "$UPLOAD_BAUD"
             --uploader-bin "$BOOTLOADER_DIR/uploader/build/$TARGET/seedsigner_sd_uploader.bin"
             upload "$UPLOAD_FILE")
    if [ -n "$UPLOAD_NAME" ]; then
        UP_ARGS+=(--name "$UPLOAD_NAME")
    fi
    python3 "$BOOTLOADER_DIR/tools/sd_upload.py" "${UP_ARGS[@]}"
    rc=$?
    if [ $rc -ne 0 ]; then
        log_error "SD upload failed (exit $rc). The card was not modified."
        exit $rc
    fi
    log_success "SD upload complete."
    if [ "$NO_MONITOR" = "1" ]; then
        log_info "Skipping monitor (--no-monitor). Reset the board to boot the new payload."
        exit 0
    fi
    # The board is still running the RAM uploader: reset it into the flashed
    # loader so it boots the new payload, then follow the boot in the monitor
    # (same ending as the `all` action). Reset via ROM (works from any state).
    log_step "Reset into loader and monitor $PORT (Ctrl-] to exit)"
    esptool.py --chip "$TARGET" --port "$PORT" \
        --before default_reset --after hard_reset chip_id > /dev/null 2>&1 || true
    cd "$BOOTLOADER_DIR" && idf.py -p "$PORT" monitor
    exit 0
fi

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
        FLASH_FREQ="80m"
    else
        BL_OFFSET="0x2000"
        PTABLE_OFFSET="0x20000"
        LOADER_OFFSET="0x30000"
        FLASH_FREQ="40m"
    fi

    esptool.py --chip "$TARGET" --port "$PORT" --baud "$BAUD" \
        --before default_reset --after hard_reset write_flash \
        --flash_mode keep --flash_freq keep --flash_size keep \
        "$BL_OFFSET" "$BOOTLOADER_BIN" \
        "$PTABLE_OFFSET" "$PARTITION_BIN" \
        "$LOADER_OFFSET" "$LOADER_BIN"

    log_success "Flash complete for $TARGET."
    if [ "$TARGET" == "esp32s3" ]; then
        log_warn "Next: ./run.sh --target esp32s3 upload-sd seedsigner_esp32s3.bin  (no card ejection needed)"
    else
        log_warn "Next: ./run.sh --target esp32p4 upload-sd seedsigner_esp32p4.bin  (no card ejection needed)"
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
