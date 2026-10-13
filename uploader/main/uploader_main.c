/** @file uploader_main.c
 * @brief RAM-only UART service: SSDU framing + SD transactions.
 *
 * Runs after `esptool load_ram`; owns UART0 at CONFIG_SDU_UART_BAUD.
 * Command set and safety model mirror the reference sd_uploader
 * (atomic .part publish, double SHA-256 readback, per-chunk CRC-32)
 * minus the CBOR/session/fixture layers — JSON responses keep the
 * host to stdlib + pyserial + esptool.
 *
 * Lifecycle: HELLO mounts the card and reports identity; BEGIN/WRITE/
 * COMMIT streams one file; RELEASE unmounts (chip stays in the uploader
 * until reset, so the host can stage several files per load).
 * ABORT recovers a half-open transaction; a 60 s RX stall during
 * RECEIVING auto-aborts to IDLE.
 *
 * Never calls flash/partition/OTA/NVS/eFuse APIs.
 */
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ssdu_proto.h"
#include "uploader_storage.h"

/* Anchors the P4 SRAM heap reservations (ram_reservations.c); the actual
 * cache-split + BSS init runs as an early ESP_SYSTEM_INIT_FN. S3 uses the
 * stock pure-RAM layout and needs no anchor. */
#if CONFIG_IDF_TARGET_ESP32P4
void sdu_reservations_link(void);
#endif

static const char *TAG = "SSDU";

static uint8_t s_tx_frame[SSDU_MAX_FRAME] SDU_BUFFER;
static uint8_t s_resp_payload[SSDU_MAX_PAYLOAD] SDU_BUFFER;
static char s_json[4096];
static int64_t s_activity_us;

static uint64_t rd_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void send_resp(ssdu_status_t status, const char *json_body)
{
    size_t jlen = strlen(json_body);
    if (jlen + 4 > sizeof(s_resp_payload))
        jlen = sizeof(s_resp_payload) - 4;
    wr_be32(s_resp_payload, (uint32_t)status);
    memcpy(s_resp_payload + 4, json_body, jlen);
    size_t n = ssdu_encode(s_tx_frame, SSDU_OP_RESP, s_resp_payload, jlen + 4);
    if (n)
        uart_write_bytes(UART_NUM_0, s_tx_frame, n);
}

/* NOTE: no 64-bit printf formats anywhere in this app. newlib-nano %llu
 * faults in this pure-RAM configuration (ROM strlen(NULL) via _printf_i);
 * all u64 values render through ssdu_print_u64. 32-bit %d/%u/%x and %s
 * are verified working on hardware. */
static void state_json(char *out, size_t cap)
{
    char off_s[21], len_s[21], cap_s[21];
    ssdu_print_u64(off_s, sizeof(off_s), ssdu_offset);
    ssdu_print_u64(len_s, sizeof(len_s), ssdu_length);
    ssdu_print_u64(cap_s, sizeof(cap_s), ssdu_capacity);
    snprintf(out, cap,
             "{\"state\":\"%s\",\"offset\":%s,\"length\":%s,"
             "\"capacity\":%s,\"baud\":%d,\"chunk_max\":%d,"
             "\"target\":\"%s\",\"service\":\"seedsigner-sd-uploader\",\"proto\":1}",
             ssdu_state, off_s, len_s, cap_s, CONFIG_SDU_UART_BAUD,
             SSDU_CHUNK_MAX, CONFIG_IDF_TARGET);
}

static void dispatch(uint8_t type, const uint8_t *pl, size_t len)
{
    s_activity_us = esp_timer_get_time();
    ssdu_status_t rc = SSDU_ERR_BAD;

    switch (type) {
    case SSDU_OP_HELLO: {
        if (len != 0)
            break;
        size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        rc = ssdu_mount();
        if (rc == SSDU_OK) {
            state_json(s_json, sizeof(s_json));
            /* Append heap diagnostics: replace trailing } with ,...} . */
            size_t n = strlen(s_json);
            if (n && s_json[n - 1] == '}' && n + 64 < sizeof(s_json)) {
                s_json[n - 1] = ',';
                snprintf(s_json + n, sizeof(s_json) - n,
                         "\"heap_free\":%u,\"heap_largest\":%u}",
                         (unsigned)heap_free, (unsigned)heap_largest);
            }
        } else {
            /* HELLO itself succeeds; mount failure is reported in-band so
             * the host can surface NO_MEDIA vs wiring issues. */
            snprintf(s_json, sizeof(s_json),
                     "{\"state\":\"%s\",\"mount_status\":\"%s\","
                     "\"target\":\"%s\",\"baud\":%d,\"chunk_max\":%d,"
                     "\"service\":\"seedsigner-sd-uploader\",\"proto\":1}",
                     ssdu_state, ssdu_status_str(rc), CONFIG_IDF_TARGET,
                     CONFIG_SDU_UART_BAUD, SSDU_CHUNK_MAX);
            rc = SSDU_OK;
        }
        send_resp(rc, s_json);
        return;
    }
    case SSDU_OP_BEGIN:
    case SSDU_OP_VERIFY: {
        /* u8 name_len | name | u64 size | 32B sha256 */
        if (len < 1 + 1 + 8 + 32)
            break;
        uint8_t nl = pl[0];
        if (nl == 0 || nl > SSDU_MAX_NAME || (size_t)1 + nl + 8 + 32 != len)
            break;
        char name[SSDU_MAX_NAME + 1];
        memcpy(name, pl + 1, nl);
        name[nl] = 0;
        uint64_t size = rd_be64(pl + 1 + nl);
        const uint8_t *dg = pl + 1 + nl + 8;
        if (type == SSDU_OP_BEGIN)
            rc = ssdu_begin(name, size, dg);
        else
            rc = ssdu_verify_existing(name, size, dg);
        if (rc == SSDU_OK) {
            char hex[65], size_s[21], off_s[21];
            ssdu_digest_hex(hex, dg);
            ssdu_print_u64(size_s, sizeof(size_s), size);
            ssdu_print_u64(off_s, sizeof(off_s), ssdu_offset);
            snprintf(s_json, sizeof(s_json),
                     "{\"state\":\"%s\",\"name\":\"%s\",\"length\":%s,"
                     "\"sha256\":\"%s\",\"offset\":%s}",
                     ssdu_state, name, size_s, hex, off_s);
        } else if (rc == SSDU_ERR_DIGEST) {
            char exp[65], got[65], olen_s[21];
            ssdu_digest_hex(exp, dg);
            ssdu_digest_hex(got, ssdu_observed);
            ssdu_print_u64(olen_s, sizeof(olen_s), ssdu_observed_len);
            snprintf(s_json, sizeof(s_json),
                     "{\"state\":\"%s\",\"observed_sha256\":\"%s\","
                     "\"observed_length\":%s,\"expected_sha256\":\"%s\"}",
                     ssdu_state, got, olen_s, exp);
        } else {
            state_json(s_json, sizeof(s_json));
        }
        send_resp(rc, s_json);
        return;
    }
    case SSDU_OP_WRITE: {
        /* u64 offset | u32 chunk_crc | data */
        if (len < 8 + 4 + 1 || len - 12 > SSDU_CHUNK_MAX)
            break;
        uint64_t off = rd_be64(pl);
        uint32_t want = rd_be32(pl + 8);
        const uint8_t *data = pl + 12;
        size_t dlen = len - 12;
        if (ssdu_crc32(data, dlen) != want) {
            rc = SSDU_ERR_BAD;
            state_json(s_json, sizeof(s_json));
            send_resp(rc, s_json);
            return;
        }
        rc = ssdu_write(off, data, dlen);
        {
            char off_s[21];
            ssdu_print_u64(off_s, sizeof(off_s), ssdu_offset);
            snprintf(s_json, sizeof(s_json),
                     "{\"state\":\"%s\",\"offset\":%s}",
                     ssdu_state, off_s);
        }
        send_resp(rc, s_json);
        return;
    }
    case SSDU_OP_COMMIT: {
        if (len != 0)
            break;
        rc = ssdu_commit();
        if (rc == SSDU_OK) {
            char hex[65], len_s[21];
            ssdu_digest_hex(hex, ssdu_digest);
            ssdu_print_u64(len_s, sizeof(len_s), ssdu_length);
            snprintf(s_json, sizeof(s_json),
                     "{\"state\":\"%s\",\"name\":\"%s\",\"length\":%s,"
                     "\"sha256\":\"%s\"}",
                     ssdu_state, ssdu_final, len_s, hex);
        } else if (rc == SSDU_ERR_DIGEST) {
            char exp[65], got[65], olen_s[21];
            ssdu_digest_hex(exp, ssdu_digest);
            ssdu_digest_hex(got, ssdu_observed);
            ssdu_print_u64(olen_s, sizeof(olen_s), ssdu_observed_len);
            snprintf(s_json, sizeof(s_json),
                     "{\"state\":\"%s\",\"observed_sha256\":\"%s\","
                     "\"observed_length\":%s,\"expected_sha256\":\"%s\"}",
                     ssdu_state, got, olen_s, exp);
        } else {
            state_json(s_json, sizeof(s_json));
        }
        send_resp(rc, s_json);
        return;
    }
    case SSDU_OP_ABORT: {
        if (len != 0)
            break;
        rc = ssdu_abort();
        state_json(s_json, sizeof(s_json));
        send_resp(rc, s_json);
        return;
    }
    case SSDU_OP_LIST: {
        if (len != 0)
            break;
        if (!strcmp(ssdu_state, "NO_MEDIA") || !strcmp(ssdu_state, "UNCERTAIN")) {
            rc = (!strcmp(ssdu_state, "NO_MEDIA")) ? SSDU_ERR_NO_MEDIA
                                                   : SSDU_ERR_UNCERTAIN;
            state_json(s_json, sizeof(s_json));
        } else {
            char entries[3072];
            ssdu_list_json(entries, sizeof(entries));
            snprintf(s_json, sizeof(s_json),
                     "{\"state\":\"%s\",\"entries\":%s}", ssdu_state, entries);
            rc = SSDU_OK;
        }
        send_resp(rc, s_json);
        return;
    }
    case SSDU_OP_REMOVE: {
        if (len < 2)
            break;
        uint8_t nl = pl[0];
        if (nl == 0 || nl > SSDU_MAX_NAME || (size_t)1 + nl != len)
            break;
        char name[SSDU_MAX_NAME + 1];
        memcpy(name, pl + 1, nl);
        name[nl] = 0;
        rc = ssdu_remove(name);
        state_json(s_json, sizeof(s_json));
        send_resp(rc, s_json);
        return;
    }
    case SSDU_OP_RELEASE: {
        if (len != 0)
            break;
        rc = ssdu_release();
        state_json(s_json, sizeof(s_json));
        send_resp(rc, s_json);
        return;
    }
    default:
        break;
    }

    state_json(s_json, sizeof(s_json));
    send_resp(rc, s_json);
}

void app_main(void)
{
#if CONFIG_IDF_TARGET_ESP32P4
    sdu_reservations_link();
#endif
    uart_config_t cfg = {
        .baud_rate = CONFIG_SDU_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &cfg));
    /* Keep the board's default TX/RX pins (console mapping) — works on both
     * Waveshare P4 (CH343) and S3 (USB-JTAG / external bridge) without
     * per-board headers. */
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 32768, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "SeedSigner SD uploader ready: target=%s baud=%d chunk_max=%d",
             CONFIG_IDF_TARGET, CONFIG_SDU_UART_BAUD, SSDU_CHUNK_MAX);
    ESP_LOGI(TAG, "Writes SD card only. No flash, no eFuse, no format.");

    s_activity_us = esp_timer_get_time();
    uint8_t rx[256];
    while (1) {
        size_t pending = 0;
        ESP_ERROR_CHECK(uart_get_buffered_data_len(UART_NUM_0, &pending));
        size_t want = pending ? pending : 1;
        if (want > sizeof(rx))
            want = sizeof(rx);
        int n = uart_read_bytes(UART_NUM_0, rx, want, pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < n; i++) {
            uint8_t ftype;
            const uint8_t *fpl;
            size_t flen;
            if (ssdu_feed(rx[i], now, &ftype, &fpl, &flen))
                dispatch(ftype, fpl, flen);
        }
        /* Stall guard: a half-open upload that goes quiet returns to IDLE so
         * the next BEGIN is not stuck behind a dead peer. */
        if (!strcmp(ssdu_state, "RECEIVING") &&
            esp_timer_get_time() - s_activity_us > 60000000) {
            ESP_LOGW(TAG, "RX stall during RECEIVING — auto-abort");
            ssdu_abort();
        }
    }
}
