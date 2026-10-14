# host_test — Host unit tests (no hardware, no ESP-IDF)

Ported from `specter-bootloader-mike/test/` (Catch2 + `BL_NO_FATFS` +
`UNIT_TEST`). Runs natively on Linux x86_64 with `gcc`/`g++` in seconds,
in CI without dev boards.

## Run

```bash
make -C host_test test      # P4 + S3 runners
make -C host_test test-p4   # ESP32-P4 memory windows only
make -C host_test test-s3   # ESP32-S3 windows + JMP-zone checks
```

`esp_image.c` selects MMU/SRAM windows with
`CONFIG_IDF_TARGET_ESP32P4/S3`, so two runners are built:
`build/host_test/p4/test_runner.out` and `build/host_test/s3/test_runner.out`.

## Layout

| Path | Origin |
|---|---|
| `catch2/catch.hpp`, `flash_buf.hpp`, `progress_monitor.hpp`, `test_main.cpp` | verbatim from upstream `test/` |
| `tests/test_bech32.cpp`, `tests/test_bl_signature.cpp`, `tests/test_bl_integrity_check.cpp` | verbatim — pure crypto, no porting needed |
| `tests/test_bl_section.cpp` | adapted: `bootloader.c`-only cases (`check_sect_compatibility`, `check_version`) and `blsect_get_attr_bytes` (absent from this fork) dropped, marked with `NOTE(seedsigner)` |
| `bl_syscalls_host.c`, `bl_syscalls_fs.h` | ported from `test/bl_syscalls_test.c` + `test/bl_syscalls_fs.h`; flash emulated in RAM, files via stdio, CRC via `crc32_fast`. Shadows `components/specter_crypto/include/bl_syscalls_fs.h` via `-Ihost_test` first |
| `stubs/` | minimal ESP-IDF fakes: `esp_log`, `esp_err`, `freertos`, `esp_heap_caps` (`posix_memalign`), `esp_mmu_map` (fake paddr), `esp_cache` (no-op), `soc/ext_mem_defs`, `esp_partition` (RAM `nvs` + `random_fill`), `esp_random` (xorshift) |
| `tests/test_esp_image.cpp` | new: `esp_image_plan()` bounds, bad magic/count, OOB data, unknown windows, MMU+copy plan, S3 JMP-zone (Review §3.3) |
| `tests/test_anti_phish.cpp` | new: `derive_bip39_words()` determinism, provision/verify roundtrip + tamper detection |
| `tests/test_bundle_bounds.cpp` | new: `main/bundle_validate.c` guards (Review §3.1, `67c34ee` regression lock) |

## Source changes required (kept target-behavior identical)

* `main/esp_image.h`: added `bool fatal` to `image_plan_t` (always false on target).
* `main/esp_image.c`: `while(1) vTaskDelay()` halts → `IMAGE_FATAL_RETURN()`; on `HOST_TEST` it frees `fake_flash`, sets `plan.fatal`, returns.
* `main/bundle_validate.{h,c}` (new): pure `bundle_validate_main_size()` + `bundle_validate_sig_section()` extracted from `main.c`; `main.c` now calls them.
* `main/anti_phish.h`: added missing `#include <stdint.h>`.
* `main/CMakeLists.txt`: compiles `bundle_validate.c`.

## Not ported (different architecture)

Upstream `test_bootloader_checks.cpp` / `test_esp32p4_*.cpp` /
`test_bl_kats.cpp` need `core/bootloader.c` / `bl_kats.c` / ESP32-P4
journal+trailer code, which this stateless loader does not compile —
covered instead by `test_esp_image` / `test_bundle_bounds` / target pytest.
