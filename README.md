# seedsigner-esp32-bootloader

Stateless secure bootloader for the SeedSigner ESP32-P4. Boot chain:

1. **Secure Boot V2** - the 2nd-stage bootloader verifies the loader image against the
   eFuse-stored key digest on every boot.
2. **Loader** (`seedsigner_secure_loader.bin`) - mounts the SD card, reads
   `seedsigner_esp32p4.bin`, verifies its secp256k1 multisig signature against
   `vendor_keys[]` (compiled in from `keys/<profile>/vendor_keys.c`), shows an
   anti-phishing proof, then loads and jumps to the MicroPython firmware from
   PSRAM.

Two independent key pairs protect this chain:

| Layer | Key | Purpose | Protects |
|---|---|---|---|
| 1 | RSA secure-boot signing key | bootloader image signing | loader image in flash |
| 2 | ECDSA secp256k1 payload key | payload bundle signing | SD-card firmware bundle |

Both private keys are gitignored and must be generated locally, never commit them.

## Prerequisites

- ESP-IDF v5.5 (`export.sh` sourced, or set `IDF_PATH`)
- Python deps for the payload tooling: `pip install ecdsa bech32`
- `seedsigner-micropython-builder` fork:
  https://github.com/wolgwang1729/seedsigner-micropython-builder

## Layer 1: Secure-boot signing key (RSA)

```bash
espsecure.py generate_signing_key secure_boot_signing_key.pem
```

This key signs the bootloader and the loader image. Its digest is stored in the
eFuse and checked at every boot. `sdkconfig.defaults` already references it via
`CONFIG_SECURE_BOOT_SIGNING_KEY="secure_boot_signing_key.pem"`.

> Dev note: this project uses virtual eFuses (`CONFIG_EFUSE_VIRTUAL=y`), so the
> key digest lives in the `efuse` flash partition, not burned silicon. Safe to
> iterate on real hardware.

## Layer 2: Vendor payload signing key (ECDSA secp256k1)

```bash
python3 tools/generate_vendor_key.py [--profile test|production]
```

This generates `payload_signing_key.pem` (gitignored) and writes the matching
`vendor_keys[]` C file to `keys/<profile>/vendor_keys.c`. The loader verifies
the SD payload against this public key, so the two must match or the firmware
won't boot.

The key profile selects which keys file the build compiles:

```bash
# test is the default profile (dev only)
idf.py build
# production keys (hold the private key offline!)
VENDOR_KEYS_PROFILE=production idf.py build
```

If you regenerate the key, `keys/<profile>/vendor_keys.c` is rewritten
automatically — just rebuild the loader. `tools/generate_signed_payload.py`
warns if the payload signing key doesn't match the compiled keys file.

## Building the MicroPython firmware (SD payload)

Use the fork: https://github.com/wolgwang1729/seedsigner-micropython-builder

```bash
git clone https://github.com/wolgwang1729/seedsigner-micropython-builder
cd seedsigner-micropython-builder
make docker-build-all BOARD=WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43
```

The raw firmware lands at
`build/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/micropython.bin`.

## Signing the payload bundle

```bash
python3 tools/generate_signed_payload.py \
    <micropython.bin> \
    seedsigner_esp32p4.bin
```

Copy `seedsigner_esp32p4.bin` to the **root of a FAT32 SD card** and insert it
into the board.

## Flashing

```bash
bash run.sh
```

This builds the loader, flashes the bootloader, partition table, and loader
(secure-boot signed) to the board at 921600 baud, and opens the serial monitor.
Default port is `/dev/ttyACM0`; override with `ESPPORT=...`. Wait for the
anti-phishing words and the MicroPython REPL prompt (`>>>`).
