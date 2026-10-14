/**
 * @file test_esp_image.cpp
 * @brief Host tests for main/esp_image.c (ESP32 image parser + load plan).
 *
 * Covers Review §3.3 / fix b04415e: every segment header/data read is
 * bounds-checked against fw_size, mapping/copy capacities are enforced,
 * unknown load windows are rejected fail-closed (plan.fatal on host).
 */
#include "catch2/catch.hpp"

extern "C" {
#include "esp_image.h"
#include "loader_config.h"
#include "esp_heap_caps.h"
#include <string.h>
}
#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/ext_mem_defs.h"
#endif

static void free_plan(image_plan_t &plan) {
  if (plan.fake_flash) {
    heap_caps_free(plan.fake_flash);
    plan.fake_flash = nullptr;
  }
}

// Minimal packed image header (must match esp_image_header_t layout).
static size_t build_hdr(uint8_t *buf, size_t cap, uint8_t seg_count,
                        uint32_t entry = 0x48000000U) {
  REQUIRE(cap >= sizeof(esp_image_header_t));
  esp_image_header_t hdr{};
  hdr.magic = ESP_IMAGE_MAGIC;
  hdr.segment_count = seg_count;
  hdr.entry_addr = entry;
  memcpy(buf, &hdr, sizeof(hdr));
  return sizeof(hdr);
}

static size_t append_seg(uint8_t *buf, size_t cap, size_t off, uint32_t vaddr,
                         const uint8_t *data, uint32_t len) {
  REQUIRE(off + sizeof(esp_image_segment_header_t) + len <= cap);
  esp_image_segment_header_t seg{};
  seg.load_addr = vaddr;
  seg.data_len = len;
  memcpy(buf + off, &seg, sizeof(seg));
  if (len) {
    memcpy(buf + off + sizeof(seg), data, len);
  }
  return off + sizeof(seg) + len;
}

#if CONFIG_IDF_TARGET_ESP32P4
static constexpr uint32_t MMU_ADDR = 0x48010000U;
static constexpr uint32_t SRAM_ADDR = 0x4FF00000U;
#elif CONFIG_IDF_TARGET_ESP32S3
static constexpr uint32_t MMU_ADDR = 0x42010000U;
static constexpr uint32_t SRAM_ADDR = 0x3FC88000U;
#else
#error "host_test must define CONFIG_IDF_TARGET_ESP32P4 or ESP32S3"
#endif

TEST_CASE("esp_image_plan rejects bad inputs") {
  uint8_t buf[1024]{};
  size_t hdr_len = build_hdr(buf, sizeof(buf), 1);

  SECTION("null buffer") {
    image_plan_t p = esp_image_plan(nullptr, 0, sizeof(buf));
    REQUIRE(p.fatal);
  }
  SECTION("payload offset out of bounds") {
    image_plan_t p = esp_image_plan(buf, (uint32_t)sizeof(buf) + 1, sizeof(buf));
    REQUIRE(p.fatal);
  }
  SECTION("truncated image header") {
    image_plan_t p = esp_image_plan(buf, 0, sizeof(esp_image_header_t) - 1);
    REQUIRE(p.fatal);
  }
  SECTION("bad magic") {
    buf[0] = 0x00;
    image_plan_t p = esp_image_plan(buf, 0, sizeof(buf));
    REQUIRE(p.fatal);
    free_plan(p);
  }
  SECTION("zero segments") {
    build_hdr(buf, sizeof(buf), 0);
    image_plan_t p = esp_image_plan(buf, 0, sizeof(buf));
    REQUIRE(p.fatal);
    free_plan(p);
  }
  SECTION("too many segments") {
    build_hdr(buf, sizeof(buf), MAX_SEGMENT_COUNT + 1);
    image_plan_t p = esp_image_plan(buf, 0, sizeof(buf));
    REQUIRE(p.fatal);
    free_plan(p);
  }
  SECTION("truncated segment header") {
    build_hdr(buf, sizeof(buf), 1);
    image_plan_t p =
        esp_image_plan(buf, 0, sizeof(esp_image_header_t) + 3);
    REQUIRE(p.fatal);
    free_plan(p);
  }
  SECTION("segment data past end of bundle") {
    size_t off = build_hdr(buf, sizeof(buf), 1);
    uint8_t data[16]{};
    // Claim 64 bytes but only provide 16.
    esp_image_segment_header_t seg{MMU_ADDR, 64};
    memcpy(buf + off, &seg, sizeof(seg));
    memcpy(buf + off + sizeof(seg), data, sizeof(data));
    image_plan_t p = esp_image_plan(
        buf, 0, off + sizeof(seg) + sizeof(data));
    REQUIRE(p.fatal);
    free_plan(p);
  }
  SECTION("unknown load window") {
    size_t off = build_hdr(buf, sizeof(buf), 1);
    uint8_t data[8]{1, 2, 3, 4, 5, 6, 7, 8};
    off = append_seg(buf, sizeof(buf), off, 0xDEAD0000U, data, sizeof(data));
    image_plan_t p = esp_image_plan(buf, 0, off);
    REQUIRE(p.fatal);
    free_plan(p);
  }
  (void)hdr_len;
}

TEST_CASE("esp_image_plan builds MMU + direct-copy plan") {
  uint8_t buf[8192]{};
  size_t off = build_hdr(buf, sizeof(buf), 2, 0x48000000U);
  uint8_t mmu_data[32];
  uint8_t sram_data[16];
  for (int i = 0; i < 32; ++i) {
    mmu_data[i] = (uint8_t)(0xA0 + i);
  }
  for (int i = 0; i < 16; ++i) {
    sram_data[i] = (uint8_t)(0x10 + i);
  }
  off = append_seg(buf, sizeof(buf), off, MMU_ADDR, mmu_data, sizeof(mmu_data));
  off = append_seg(buf, sizeof(buf), off, SRAM_ADDR, sram_data, sizeof(sram_data));

  image_plan_t p = esp_image_plan(buf, 0, off);
  REQUIRE(!p.fatal);
  REQUIRE(p.entry_addr == 0x48000000U);
  REQUIRE(p.mmu_mapping_count == 1);
  REQUIRE(p.copy_count == 1);
  REQUIRE(p.fake_flash != nullptr);
  REQUIRE(p.max_offset > 0);
  // Payload staged at linear offset inside fake_flash.
#if CONFIG_IDF_TARGET_ESP32P4
  uint32_t write_off = MMU_ADDR - 0x48000000U;
#elif CONFIG_IDF_TARGET_ESP32S3
  uint32_t write_off = MMU_ADDR & SOC_MMU_LINEAR_ADDR_MASK;
#endif
  REQUIRE(0 == memcmp(p.fake_flash + write_off, mmu_data, sizeof(mmu_data)));
  // Direct copy still points into the source bundle.
  REQUIRE(p.direct_copies[0].len == sizeof(sram_data));
  REQUIRE(0 == memcmp(p.direct_copies[0].src, sram_data, sizeof(sram_data)));
  free_plan(p);
}

TEST_CASE("esp_image_plan handles many segments") {
  // MAX_SEGMENT_COUNT MMU segments must be accepted (capacity is 20 > 16,
  // so the guard is unreachable via header alone — this locks the behavior).
  uint8_t buf[1 << 20]{};
  size_t off = build_hdr(buf, sizeof(buf), MAX_SEGMENT_COUNT);
  uint8_t data[64]{};
  for (int i = 0; i < MAX_SEGMENT_COUNT; ++i) {
#if CONFIG_IDF_TARGET_ESP32P4
    uint32_t va = 0x48000000U + (uint32_t)i * 0x10000U;
#elif CONFIG_IDF_TARGET_ESP32S3
    uint32_t va = 0x42000000U + (uint32_t)i * 0x10000U;
#endif
    off = append_seg(buf, sizeof(buf), off, va, data, sizeof(data));
  }
  image_plan_t p = esp_image_plan(buf, 0, off);
  REQUIRE(!p.fatal);
  REQUIRE(p.mmu_mapping_count == MAX_SEGMENT_COUNT);
  free_plan(p);
}

#if CONFIG_IDF_TARGET_ESP32S3
TEST_CASE("esp_image_plan rejects JMP-zone collisions (S3)") {
  uint8_t buf[1024]{};
  size_t off = build_hdr(buf, sizeof(buf), 1);
  uint8_t data[16]{};
  // Segment starting inside the loader JMP zone 0x403A0000-0x403B8000.
  off = append_seg(buf, sizeof(buf), off, 0x403A0000U, data, sizeof(data));
  image_plan_t p = esp_image_plan(buf, 0, off);
  REQUIRE(p.fatal);
  free_plan(p);
}
#endif
