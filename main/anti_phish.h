#pragma once

#include "esp_err.h"

#define BIP39_WORD_COUNT 4

/**
 * @brief Fill the random_fill partition with TRNG data and store its hash in NVS.
 *        If already provisioned, does nothing.
 */
esp_err_t provision_flash_fill(void);

/**
 * @brief Verify the SHA-256 hash of the random_fill partition against NVS,
 *        and derive 4 BIP-39 words from it.
 * @param words Output array for the 4 BIP-39 words
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if tamper detected.
 */
esp_err_t verify_anti_phishing_proof(char words[BIP39_WORD_COUNT][12]);

/**
 * @brief Helper to derive 4 BIP-39 words from a 32-byte hash/HMAC.
 */
void derive_bip39_words(const uint8_t hash[32], char words[4][12]);
