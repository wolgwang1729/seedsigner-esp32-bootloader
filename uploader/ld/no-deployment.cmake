# From miketlk/specter-bootloader sd_uploader/ld/no-deployment.cmake verbatim
# (https://github.com/miketlk/specter-bootloader/tree/port_esp32-p4):
# the RAM-only uploader must never gain flash/erase/merge targets.
message(FATAL_ERROR "SD uploader is RAM-only. Flash, erase and deployment targets are forbidden. Use the checked no-stub RAM loader.")
