/**
 * @file test_bundle_bounds.cpp
 * @brief Host tests for main/bundle_validate.c (Review §3.1, fix 67c34ee).
 *
 * Locks in the pl_size / signature-extent guards so the 354e0bf regression
 * (dropped checks → OOB read + OOB sig_hdr deref) cannot silently return.
 */
#include "catch2/catch.hpp"

extern "C" {
#include "bundle_validate.h"
#include "bl_section.h"
#include "crc32.h"
#include <string.h>
#include <vector>
}

static bl_section_t make_hdr(uint32_t pl_size) {
  bl_section_t hdr{};
  hdr.magic = BL_SECT_MAGIC;
  hdr.struct_rev = BL_SECT_STRUCT_REV;
  strcpy(hdr.name, "main");
  hdr.pl_ver = 1;
  hdr.pl_size = pl_size;
  hdr.struct_crc =
      crc32_fast(&hdr, offsetof(bl_section_t, struct_crc), 0U);
  return hdr;
}

TEST_CASE("bundle_validate_main_size") {
  SECTION("exact fit passes") {
    REQUIRE(bundle_validate_main_size(100, sizeof(bl_section_t) + 100) ==
            ESP_OK);
  }
  SECTION("one byte over fails") {
    REQUIRE(bundle_validate_main_size(101, sizeof(bl_section_t) + 100) ==
            ESP_ERR_INVALID_SIZE);
  }
  SECTION("empty bundle fails") {
    REQUIRE(bundle_validate_main_size(1, 0) == ESP_ERR_INVALID_SIZE);
  }
}

TEST_CASE("bundle_validate_sig_section") {
  SECTION("valid bundle") {
    const uint32_t main_pl = 64;
    const uint32_t sig_pl = 80;
    std::vector<uint8_t> bundle(sizeof(bl_section_t) + main_pl +
                                sizeof(bl_section_t) + sig_pl);
    bl_section_t main = make_hdr(main_pl);
    memcpy(bundle.data(), &main, sizeof(main));
    bl_section_t sig = make_hdr(sig_pl);
    strcpy(sig.name, "sign");
    sig.struct_crc =
        crc32_fast(&sig, offsetof(bl_section_t, struct_crc), 0U);
    memcpy(bundle.data() + sizeof(main) + main_pl, &sig, sizeof(sig));

    const bl_section_t *out = nullptr;
    REQUIRE(bundle_validate_sig_section(bundle.data(), bundle.size(), main_pl,
                                        &out) == ESP_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out->pl_size == sig_pl);
  }
  SECTION("truncated main payload fails") {
    std::vector<uint8_t> bundle(sizeof(bl_section_t) + 10);
    const bl_section_t *out = (const bl_section_t *)0x1;
    REQUIRE(bundle_validate_sig_section(bundle.data(), bundle.size(), 100,
                                        &out) == ESP_ERR_INVALID_SIZE);
  }
  SECTION("truncated sig header fails") {
    const uint32_t main_pl = 32;
    std::vector<uint8_t> bundle(sizeof(bl_section_t) + main_pl +
                                sizeof(bl_section_t) - 1);
    const bl_section_t *out = nullptr;
    REQUIRE(bundle_validate_sig_section(bundle.data(), bundle.size(), main_pl,
                                        &out) == ESP_ERR_INVALID_SIZE);
  }
  SECTION("sig payload past bundle end fails") {
    const uint32_t main_pl = 32;
    std::vector<uint8_t> bundle(sizeof(bl_section_t) + main_pl +
                                sizeof(bl_section_t) + 16);
    bl_section_t sig = make_hdr(1024); // lies about its size
    strcpy(sig.name, "sign");
    memcpy(bundle.data() + sizeof(bl_section_t) + main_pl, &sig, sizeof(sig));
    const bl_section_t *out = nullptr;
    REQUIRE(bundle_validate_sig_section(bundle.data(), bundle.size(), main_pl,
                                        &out) == ESP_ERR_INVALID_SIZE);
  }
  SECTION("null args fail") {
    const bl_section_t *out = nullptr;
    REQUIRE(bundle_validate_sig_section(nullptr, 100, 10, &out) ==
            ESP_ERR_INVALID_ARG);
    uint8_t buf[64]{};
    REQUIRE(bundle_validate_sig_section(buf, sizeof(buf), 10, nullptr) ==
            ESP_ERR_INVALID_ARG);
  }
}
