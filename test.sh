#!/usr/bin/env bash
# test.sh — Run the SeedSigner Stateless Bootloader pytest suite
# Usage: ./test.sh [PORT]
#   PORT defaults to /dev/ttyACM0

set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source ~/esp/esp-idf-v5.5/export.sh

pytest -s --embedded-services esp,idf \
       --target esp32p4 \
       --port "$PORT" \
       "$SCRIPT_DIR/pytest_seedsigner_bootloader_p4_stateless_os.py"
