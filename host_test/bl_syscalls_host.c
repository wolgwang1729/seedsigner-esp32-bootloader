/**
 * @file       bl_syscalls_host.c
 * @brief      Host syscalls shim for host_test (no ESP-IDF).
 *
 * Ported from specter-bootloader-mike/test/bl_syscalls_test.c, adapted to
 * components/specter_crypto/src/bl_syscalls.h (which has no
 * blsys_payload_format / blsys_flash_finalize):
 *  - flash is emulated in RAM (flash_emu_buf, see flash_buf.hpp)
 *  - files use stdio, find uses opendir/fnmatch
 *  - CRC uses crc32_fast (no esp_rom_crc on host)
 */

#include "bl_syscalls.h"

#include <dirent.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bl_util.h"
#include "crc32.h"

/// Flags used with fnmatch() to match file names
#define FNMATCH_FLAGS (FNM_FILE_NAME | FNM_PERIOD)

/// Base address of emulated flash memory
const bl_addr_t flash_emu_base = 0x08000000U;
/// Buffer in RAM used to emulate flash memory
uint8_t *flash_emu_buf = NULL;
/// Size of currently allocated flash emulation buffer
size_t flash_emu_size = 0U;

const char *blsys_platform_id(void) { return "seedsigner_host_test"; }

bool blsys_init(void) { return true; }

void blsys_deinit(void) {}

bool blsys_flash_map_get_items(int items, ...) { return false; }

/**
 * Checks if area in flash memory falls in valid address range.
 */
static bool check_flash_area(bl_addr_t addr, size_t size) {
  if (addr >= flash_emu_base && addr <= SIZE_MAX - size &&
      addr + size <= flash_emu_base + flash_emu_size) {
    return true;
  }
  return false;
}

bool blsys_flash_erase(bl_addr_t addr, size_t size) {
  if (flash_emu_buf && check_flash_area(addr, size)) {
    memset(flash_emu_buf + (addr - flash_emu_base), 0xFFU, size);
    return true;
  }
  return false;
}

bool blsys_flash_read(bl_addr_t addr, void *buf, size_t len) {
  // 1) Emulated STM32-flash window (used by flash_buf.hpp tests).
  if (flash_emu_buf && buf && check_flash_area(addr, len)) {
    memcpy(buf, flash_emu_buf + (addr - flash_emu_base), len);
    return true;
  }
  // 2) Direct host-pointer reads so blsect_hash_over_flash() works with heap
  // buffers the way seedsigner's target bl_syscalls.c treats PSRAM pointers.
  // The STM32 firmware window is explicitly excluded: low addresses such as
  // 0x08000000 are never valid host pointers (a corrupted pl_size must fail
  // cleanly, not segfault — see test_bl_integrity_check "internals").
  if (buf && len && addr >= 0x10000U &&
      (addr < 0x06000000U || addr >= 0x0C000000U)) {
    memcpy(buf, (const void *)addr, len);
    return true;
  }
  return false;
}

bool blsys_flash_write(bl_addr_t addr, const void *buf, size_t len) {
  if (flash_emu_buf && buf && check_flash_area(addr, len)) {
    size_t offset = addr - flash_emu_base;
    for (size_t i = offset; i < offset + len; ++i) {
      if (flash_emu_buf[i] != 0xFFU) {
        return false;
      }
    }
    memcpy(flash_emu_buf + offset, buf, len);
    return true;
  }
  return false;
}

bool blsys_flash_crc32(uint32_t *p_crc, bl_addr_t addr, size_t len) {
  if (p_crc && len) {
    uint8_t buf[128];
    size_t rm = len;
    bl_addr_t cur = addr;
    while (rm) {
      size_t n = rm < sizeof(buf) ? rm : sizeof(buf);
      if (!blsys_flash_read(cur, buf, n)) {
        return false;
      }
      *p_crc = crc32_fast(buf, n, *p_crc);
      cur += n;
      rm -= n;
    }
    return true;
  }
  return false;
}

bool blsys_flash_write_protect(bl_addr_t addr, size_t size, bool enable) {
  (void)addr;
  (void)size;
  (void)enable;
  return true;
}

bool blsys_flash_read_protect(int level) {
  (void)level;
  return true;
}

int blsys_flash_get_read_protection_level(void) { return -1; }

uint32_t blsys_media_devices(void) { return 1U; }

const char *blsys_media_name(uint32_t device_idx) {
  (void)device_idx;
  return "host";
}

bool blsys_media_check(uint32_t device_idx) { return device_idx == 0U; }

bool blsys_media_mount(uint32_t device_idx) { return device_idx == 0U; }

void blsys_media_umount(void) {}

const char *blsys_ffind_first(bl_ffind_ctx_t *ctx, const char *path,
                              const char *pattern) {
  if (ctx && path && pattern) {
    ctx->pattern = strdup(pattern);
    ctx->dir = opendir(('\0' == *path || bl_streq(path, "/")) ? "./" : path);
    if (ctx->pattern && ctx->dir) {
      struct dirent *ent;
      do {
        ent = readdir(ctx->dir);
        if (ent && 0 == fnmatch(pattern, ent->d_name, FNMATCH_FLAGS)) {
          return ent->d_name;
        }
      } while (ent);
    }
  }
  return NULL;
}

const char *blsys_ffind_next(bl_ffind_ctx_t *ctx) {
  if (ctx && ctx->pattern && ctx->dir) {
    struct dirent *ent;
    do {
      ent = readdir(ctx->dir);
      if (ent && 0 == fnmatch(ctx->pattern, ent->d_name, FNMATCH_FLAGS)) {
        return ent->d_name;
      }
    } while (ent);
  }
  return NULL;
}

void blsys_ffind_close(bl_ffind_ctx_t *ctx) {
  if (ctx) {
    free(ctx->pattern);
    ctx->pattern = NULL;
    if (ctx->dir) {
      closedir(ctx->dir);
      ctx->dir = NULL;
    }
  }
}

bl_file_t blsys_fopen(bl_file_obj_t *p_file_obj, const char *filename,
                      const char *mode) {
  (void)p_file_obj;
  return fopen(filename, mode);
}

size_t blsys_fread(void *ptr, size_t size, size_t count, bl_file_t file) {
  return fread(ptr, size, count, file);
}

bl_foffset_t blsys_ftell(bl_file_t file) { return (bl_foffset_t)ftell(file); }

int blsys_fseek(bl_file_t file, bl_foffset_t offset, int origin) {
  return fseek(file, offset, origin);
}

bl_fsize_t blsys_fsize(bl_file_t file) {
  long cur = ftell(file);
  if (cur >= 0 && 0 == fseek(file, 0L, SEEK_END)) {
    long end = ftell(file);
    fseek(file, cur, SEEK_SET);
    if (end >= 0) {
      return (bl_fsize_t)end;
    }
  }
  return 0U;
}

int blsys_feof(bl_file_t file) { return feof(file); }

int blsys_fclose(bl_file_t file) { return fclose(file); }

/* blsys_fatal_error is defined in test_main.cpp (Catch2 FAIL). */

bl_alert_status_t blsys_alert(blsys_alert_type_t type, const char *caption,
                              const char *text, uint32_t time_ms,
                              uint32_t flags) {
  (void)type;
  (void)caption;
  (void)text;
  (void)time_ms;
  (void)flags;
  return bl_alert_terminated;
}

void blsys_progress(const char *caption, const char *operation,
                    uint32_t percent_x100) {
  (void)caption;
  (void)operation;
  (void)percent_x100;
}

bool blsys_start_firmware(bl_addr_t start_addr, uint32_t argument) {
  (void)start_addr;
  (void)argument;
  return false;
}
