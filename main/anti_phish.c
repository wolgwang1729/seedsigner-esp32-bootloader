#include "anti_phish.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "sha2.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RANDOM_FILL_PARTITION_LABEL "random_fill"
#define NVS_PARTITION_LABEL         "nvs"
#define FILL_CHUNK_SIZE             4096   // one flash sector
#define BIP39_BITS_PER_WORD         11     // 2048 words = 11 bits each

static const char *TAG = "anti_phish";

extern const char *bip39_wordlist[2048];

typedef struct {
    uint32_t magic;
    uint8_t hash[32];
} __attribute__((packed)) anti_phish_state_t;

#define AP_MAGIC 0x41504F4B // "APOK"

static uint16_t extract_bits(const uint8_t *data, int bit_pos, int num_bits) {
    uint16_t result = 0;
    for (int i = 0; i < num_bits; i++) {
        int byte_idx = (bit_pos + i) / 8;
        int bit_idx = 7 - ((bit_pos + i) % 8);
        result = (result << 1) | ((data[byte_idx] >> bit_idx) & 1);
    }
    return result;
}

void derive_bip39_words(const uint8_t hash[32], char words[4][12]) {
    for (int i = 0; i < 4; i++) {
        uint16_t index = extract_bits(hash, i * BIP39_BITS_PER_WORD, BIP39_BITS_PER_WORD);
        strncpy(words[i], bip39_wordlist[index], 11);
        words[i][11] = '\0';
    }
}

esp_err_t provision_flash_fill(void) {
    const esp_partition_t *nvs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x99, NVS_PARTITION_LABEL);
    if (!nvs_part) {
        ESP_LOGE(TAG, "nvs partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    anti_phish_state_t state;
    esp_partition_read(nvs_part, 0, &state, sizeof(state));
    if (state.magic == AP_MAGIC) {
        return ESP_OK;  // already provisioned
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x06, RANDOM_FILL_PARTITION_LABEL);
    if (!part) {
        ESP_LOGE(TAG, "random_fill partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Erasing %lu bytes at 0x%08lx...", part->size, part->address);
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) return err;

    uint8_t buf[FILL_CHUNK_SIZE];
    ESP_LOGI(TAG, "Filling with TRNG random data...");
    for (size_t off = 0; off < part->size; off += FILL_CHUNK_SIZE) {
        if ((off % (FILL_CHUNK_SIZE * 64)) == 0) {
            ESP_LOGI(TAG, "Filled %zu / %zu bytes...", off, part->size);
            vTaskDelay(1); // yield to IDLE task
        }
        esp_fill_random(buf, FILL_CHUNK_SIZE);
        err = esp_partition_write(part, off, buf, FILL_CHUNK_SIZE);
        if (err != ESP_OK) return err;
    }

    SHA256_CTX ctx;
    sha256_Init(&ctx);
    for (size_t off = 0; off < part->size; off += FILL_CHUNK_SIZE) {
        if ((off % (FILL_CHUNK_SIZE * 64)) == 0) {
            ESP_LOGI(TAG, "Hashing %zu / %zu bytes...", off, part->size);
            vTaskDelay(1); // yield to IDLE task
        }
        esp_partition_read(part, off, buf, FILL_CHUNK_SIZE);
        sha256_Update(&ctx, buf, FILL_CHUNK_SIZE);
    }
    sha256_Final(&ctx, state.hash);

    state.magic = AP_MAGIC;
    esp_partition_erase_range(nvs_part, 0, 4096);
    esp_partition_write(nvs_part, 0, &state, sizeof(state));

    ESP_LOGI(TAG, "Flash fill provisioned. Hash stored.");
    return ESP_OK;
}

esp_err_t verify_anti_phishing_proof(char words[BIP39_WORD_COUNT][12]) {
    const esp_partition_t *nvs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x99, NVS_PARTITION_LABEL);
    if (!nvs_part) return ESP_ERR_NOT_FOUND;

    anti_phish_state_t state;
    esp_partition_read(nvs_part, 0, &state, sizeof(state));
    if (state.magic != AP_MAGIC) {
        ESP_LOGE(TAG, "Not provisioned during verification");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x06, RANDOM_FILL_PARTITION_LABEL);
    if (!part) return ESP_ERR_NOT_FOUND;

    uint8_t buf[FILL_CHUNK_SIZE];
    uint8_t current_hash[32];
    SHA256_CTX ctx;
    sha256_Init(&ctx);
    for (size_t off = 0; off < part->size; off += FILL_CHUNK_SIZE) {
        if ((off % (FILL_CHUNK_SIZE * 64)) == 0) {
            ESP_LOGI(TAG, "Verifying hash %zu / %zu bytes...", off, part->size);
            vTaskDelay(1); // yield to IDLE task
        }
        esp_partition_read(part, off, buf, FILL_CHUNK_SIZE);
        sha256_Update(&ctx, buf, FILL_CHUNK_SIZE);
    }
    sha256_Final(&ctx, current_hash);

    if (memcmp(state.hash, current_hash, 32) != 0) {
        ESP_LOGE(TAG, "⚠️  FLASH TAMPERED! Hash mismatch detected.");
        return ESP_ERR_INVALID_STATE;
    }

    derive_bip39_words(current_hash, words);

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  ANTI-PHISHING PROOF: %s %s %s %s",
             words[0], words[1], words[2], words[3]);
    ESP_LOGI(TAG, "================================================");

    return ESP_OK;
}
