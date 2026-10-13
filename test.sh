#!/usr/bin/env bash
# test.sh — Run the SeedSigner Stateless Bootloader pytest suite
# Usage: ./test.sh [TARGET] [PORT]
#   TARGET defaults to esp32p4 (esp32p4|esp32s3)
#   PORT defaults to /dev/ttyACM0
#   For backwards compatibility, ./test.sh [PORT] is still accepted.

set -euo pipefail

TARGET="esp32p4"
PORT="/dev/ttyACM0"

if [ $# -ge 1 ]; then
    case "$1" in
        esp32p4|esp32s3|p4|s3)
            case "$1" in
                p4) TARGET="esp32p4" ;;
                s3) TARGET="esp32s3" ;;
                *) TARGET="$1" ;;
            esac
            shift
            ;;
        *)
            # First arg is a port path (legacy usage)
            PORT="$1"
            shift
            ;;
    esac
fi
if [ $# -ge 1 ]; then
    PORT="$1"
    shift
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source ~/esp/esp-idf-v5.5/export.sh

if [ "$TARGET" = "esp32s3" ]; then
    PYTEST_FILE="$SCRIPT_DIR/pytest_seedsigner_bootloader_esp32s3_stateless_os.py"
else
    PYTEST_FILE="$SCRIPT_DIR/pytest_seedsigner_bootloader_p4_stateless_os.py"
fi

pytest -s --embedded-services esp,idf \
       --target "$TARGET" \
       --port "$PORT" \
       "$PYTEST_FILE"
