# SeedSigner SD uploader (RAM-only dev tool)

Runs entirely from internal SRAM — loaded over UART with `esptool load_ram` —
and writes **only the removable microSD card**. No flash erase/write, no
eFuse, no Secure Boot interaction, no partition table. The idea is ported from
[`miketlk/specter-bootloader`](https://github.com/miketlk/specter-bootloader/tree/port_esp32-p4)'s
`sd_uploader`; the protocol is simplified to
plain binary framing + JSON so the host needs nothing beyond what ESP-IDF
already ships (`pyserial` + `esptool`).

Typical dev loop (no more card ejection):

```bash
# Build the signed payload, then push it straight to the board's SD card:
python3 tools/generate_signed_payload.py build/micropython.bin seedsigner_esp32p4.bin
./run.sh --target esp32p4 upload-sd seedsigner_esp32p4.bin
```

What happens under the hood:

1. `run.sh upload-sd` builds `uploader/` for the selected target as a
   standard IDF RAM app (`CONFIG_APP_BUILD_TYPE_RAM=y`) into
   `uploader/build/<target>/seedsigner_sd_uploader.bin`.
2. `tools/sd_upload.py` runs `esptool load_ram` (ROM baud 921600, no stub,
   no reset-after) to execute it, then reopens the port at the app baud
   (`CONFIG_SDU_UART_BAUD`, default 921600).
3. The SSDU protocol streams the file in CRC-32-checked chunks to a reserved
   `_ssdu_*.part` temp file, then publishes atomically: flush + fsync +
   independent SHA-256 readback of temp → rename (never overwrites) →
   independent SHA-256 readback of final. A digest mismatch deletes the temp
   and reports both digests; I/O failures park the device in `UNCERTAIN`
   until `ABORT`.
4. `RELEASE` unmounts cleanly (LDO power handle freed on P4, SPI bus freed on
   S3). The chip stays in the uploader until reset, so several files can be
   staged per load.

Uploads are overwrite-unless-identical: if the destination already exists,
the host first `VERIFY`s it against the local SHA-256. Identical → skipped
(`SKIPPED`, no bytes sent). Different → `REMOVE`d, then uploaded fresh. The
device still refuses overwrites at `BEGIN` as a second line of defence.

## Manual use

```bash
# Build once per target:
idf.py -C uploader -B uploader/build/esp32p4 set-target esp32p4 && idf.py -C uploader -B uploader/build/esp32p4 build
idf.py -C uploader -B uploader/build/esp32s3 set-target esp32s3 && idf.py -C uploader -B uploader/build/esp32s3 build

# Push (loads RAM image, then uploads):
python3 tools/sd_upload.py --chip esp32p4 --port /dev/ttyACM0 \
    --uploader-bin uploader/build/esp32p4/seedsigner_sd_uploader.bin \
    upload seedsigner_esp32p4.bin

# Talk to the card (each command loads the RAM image first in the same
# process — the image is RAM-resident, so a reopened port would lose it):
python3 tools/sd_upload.py --chip esp32p4 --port /dev/ttyACM0 status
python3 tools/sd_upload.py --chip esp32p4 --port /dev/ttyACM0 list
python3 tools/sd_upload.py --chip esp32p4 --port /dev/ttyACM0 \
    verify seedsigner_esp32p4.bin --name seedsigner_esp32p4.bin
```

Faster benches: rebuild the uploader at 2000000/4000000 baud
(`idf.py -C uploader menuconfig` → Uploader UART baud, or
`-DSDKCONFIG_DEFAULTS=...` profile) and pass the same `--baud`.

## Protocol

SSDU v1 frames both ways: `SSDU | 0x01 | type | u32BE len | payload | u32BE crc32`
(CRC over ver+type+len+payload). Host→device `0x01..0x09`
(HELLO/BEGIN/WRITE/COMMIT/ABORT/LIST/VERIFY/REMOVE/RELEASE);
device→host `0x81 RESP` = `u32BE status | JSON`. See `main/ssdu_proto.h`.
Device JSON uses only 32-bit `%d/%u/%x` and `%s`: 64-bit printf formats
fault in this pure-RAM newlib-nano configuration, so all u64 values render
through `ssdu_print_u64()` (see note in `main/uploader_main.c`).

## Validation status

- **ESP32-P4 (rev 1.3, Waveshare 4.3C, CH343): fully validated on hardware**
  (`tools/test_sd_upload.py` + live `upload/verify/list/remove` of 100KB and
  4.5MB files with byte-exact SHA-256 readbacks).
- **ESP32-S3: builds and links** (pure-RAM image, SRAM-only segments,
  checksum valid, no flash/eFuse symbols) **but not yet run on hardware**.
  S3 uses the stock pure-RAM layout plus two small stubs (`cache_stub_s3.c`)
  for cache helpers IDF omits on S3-pure-RAM; needs a bench run before trust.
- Because the image is RAM-resident, every command loads it first **in the
  same process**; reopening the port between load and talk resets the board
  and loses the image (no `--no-load` mode by design).

## Safety

- Mount uses `format_if_mount_failed = false` — a bad card is reported, never wiped.
- Same SD slot/pins as the loader (`storage_p4.c` / `storage_s3.c`), so what you
  upload is what the loader boots.
- No `spi_flash`/`partition`/`ota`/`nvs`/`efuse` calls anywhere in `uploader/main/`;
  the top-level `CMakeLists.txt` fails the build if bootloader/flash-sections,
  Secure Boot, or flash encryption ever get enabled.
