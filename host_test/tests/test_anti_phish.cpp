/**
 * @file test_anti_phish.cpp
 * @brief Host tests for main/anti_phish.c BIP-39 derivation + provision/verify.
 */
#include "catch2/catch.hpp"

extern "C" {
#include "anti_phish.h"
#include "esp_err.h"
#include "esp_partition.h"
#include <string.h>

extern const char *bip39_wordlist[2048];
void host_part_reset(void);
void host_rng_seed(uint64_t seed);
}

TEST_CASE("derive_bip39_words is deterministic and in-range") {
  uint8_t hash[32];
  for (int i = 0; i < 32; ++i) {
    hash[i] = (uint8_t)i;
  }
  char w1[4][12], w2[4][12];
  derive_bip39_words(hash, w1);
  derive_bip39_words(hash, w2);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(0 == strcmp(w1[i], w2[i]));
    bool found = false;
    for (int j = 0; j < 2048; ++j) {
      if (0 == strcmp(w1[i], bip39_wordlist[j])) {
        found = true;
        break;
      }
    }
    REQUIRE(found);
  }
  // A single bit flip changes at least one word (11-bit groups).
  uint8_t hash2[32];
  memcpy(hash2, hash, sizeof(hash2));
  hash2[0] ^= 0x80;
  char w3[4][12];
  derive_bip39_words(hash2, w3);
  REQUIRE(0 != strcmp(w1[0], w3[0]));
}

TEST_CASE("anti-phishing provision + verify roundtrip") {
  host_part_reset();
  host_rng_seed(0x1234);

  REQUIRE(provision_flash_fill() == ESP_OK);

  char words[4][12];
  REQUIRE(verify_anti_phishing_proof(words) == ESP_OK);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(strlen(words[i]) > 0);
  }

  // Second provision is idempotent; words must be stable across reboots.
  char words2[4][12];
  REQUIRE(provision_flash_fill() == ESP_OK);
  REQUIRE(verify_anti_phishing_proof(words2) == ESP_OK);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(0 == strcmp(words[i], words2[i]));
  }
}

TEST_CASE("anti-phishing detects flash tamper") {
  host_part_reset();
  host_rng_seed(0xABCD);
  REQUIRE(provision_flash_fill() == ESP_OK);

  // Corrupt one byte of the provisioned fill via the raw partition API.
  const esp_partition_t *part = esp_partition_find_first(1, 0x06, "random_fill");
  REQUIRE(part != nullptr);
  uint8_t bad = 0x00;
  REQUIRE(esp_partition_write(part, 0, &bad, 1) == ESP_OK);

  char words[4][12];
  REQUIRE(verify_anti_phishing_proof(words) == ESP_ERR_INVALID_STATE);
}
