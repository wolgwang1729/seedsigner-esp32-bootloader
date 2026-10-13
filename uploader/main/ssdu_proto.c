/** @file ssdu_proto.c
 * @brief Allocation-free SSDU v1 framing and admission checks.
 */
#include "ssdu_proto.h"

#include <string.h>

const char *ssdu_status_str(ssdu_status_t s)
{
    switch (s) {
    case SSDU_OK: return "OK";
    case SSDU_ERR_BAD: return "BAD_REQUEST";
    case SSDU_ERR_PATH: return "BAD_PATH";
    case SSDU_ERR_RANGE: return "BAD_RANGE";
    case SSDU_ERR_OFFSET: return "BAD_OFFSET";
    case SSDU_ERR_NO_MEDIA: return "NO_MEDIA";
    case SSDU_ERR_MOUNT: return "MOUNT_ERROR";
    case SSDU_ERR_SPACE: return "NO_SPACE";
    case SSDU_ERR_IO: return "IO_ERROR";
    case SSDU_ERR_DIGEST: return "DIGEST_MISMATCH";
    case SSDU_ERR_EXISTS: return "EXISTS";
    case SSDU_ERR_BUSY: return "BUSY";
    case SSDU_ERR_UNCERTAIN: return "UNCERTAIN";
    default: return "UNKNOWN";
    }
}

uint32_t ssdu_crc32(const uint8_t *data, size_t len)
{
    /* Nibble table keeps .rodata to 64 bytes; same digest as zlib crc32. */
    static const uint32_t kTable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ kTable[crc & 15];
        crc = (crc >> 4) ^ kTable[crc & 15];
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

bool ssdu_feed(uint8_t byte, int64_t now_us, uint8_t *out_type,
               const uint8_t **out_payload, size_t *out_len)
{
    static uint8_t s_buf[SSDU_MAX_FRAME] SDU_BUFFER;
    static size_t s_used;
    static int64_t s_since;
    static bool s_delivered;

    if (s_delivered || (s_used && now_us - s_since > 2000000))
        s_used = 0;
    s_delivered = false;
    if (!s_used)
        s_since = now_us;
    s_buf[s_used++] = byte;

    while (s_used >= 4) {
        if (s_buf[0] != SSDU_MAGIC_0 || s_buf[1] != SSDU_MAGIC_1 ||
            s_buf[2] != SSDU_MAGIC_2 || s_buf[3] != SSDU_MAGIC_3) {
            memmove(s_buf, s_buf + 1, --s_used);
            s_since = now_us;
            continue;
        }
        if (s_used < 10)
            return false;
        uint8_t ver = s_buf[4];
        uint8_t type = s_buf[5];
        uint32_t len = rd_be32(s_buf + 6);
        bool is_req = (type >= SSDU_OP_HELLO && type <= SSDU_OP_RELEASE);
        if (ver != SSDU_VERSION || !is_req || len > SSDU_MAX_PAYLOAD) {
            memmove(s_buf, s_buf + 1, --s_used);
            s_since = now_us;
            continue;
        }
        if (s_used < (size_t)len + 14)
            return false;
        /* CRC covers ver|type|len|payload (bytes s_buf[4 .. 10+len)). */
        {
            static const uint32_t kTable[16] = {
                0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
                0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
                0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
            };
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 4; i < 10 + len; i++) {
                c ^= s_buf[i];
                c = (c >> 4) ^ kTable[c & 15];
                c = (c >> 4) ^ kTable[c & 15];
            }
            c ^= 0xFFFFFFFFu;
            uint32_t want = rd_be32(s_buf + 10 + len);
            if (c != want) {
                memmove(s_buf, s_buf + 1, --s_used);
                s_since = now_us;
                continue;
            }
        }
        s_delivered = true;
        *out_type = type;
        *out_payload = s_buf + 10;
        *out_len = len;
        return true;
    }
    return false;
}

size_t ssdu_encode(uint8_t *out, uint8_t type, const uint8_t *payload,
                   size_t len)
{
    if (len > SSDU_MAX_PAYLOAD)
        return 0;
    out[0] = SSDU_MAGIC_0;
    out[1] = SSDU_MAGIC_1;
    out[2] = SSDU_MAGIC_2;
    out[3] = SSDU_MAGIC_3;
    out[4] = SSDU_VERSION;
    out[5] = type;
    wr_be32(out + 6, (uint32_t)len);
    if (len && payload)
        memcpy(out + 10, payload, len);
    /* CRC over ver|type|len|payload. */
    {
        static const uint32_t kTable[16] = {
            0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
            0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
            0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
            0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
        };
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 4; i < 10 + len; i++) {
            c ^= out[i];
            c = (c >> 4) ^ kTable[c & 15];
            c = (c >> 4) ^ kTable[c & 15];
        }
        c ^= 0xFFFFFFFFu;
        wr_be32(out + 10 + len, c);
    }
    return len + 14;
}

bool ssdu_valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > SSDU_MAX_NAME)
        return false;
    /* Reserved temp namespace and dot entries are never valid targets. */
    if (name[0] == '_' && strncmp(name, "_ssdu_", 6) == 0)
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    if (name[n - 1] == '.' || name[n - 1] == ' ')
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c > 126)
            return false;
        if (c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' ||
            c == '?' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return true;
}

size_t ssdu_print_u64(char *buf, size_t cap, uint64_t v)
{
    char tmp[21];
    size_t n = 0;
    if (cap < 21)
        return 0;
    if (v == 0)
        tmp[n++] = '0';
    while (v && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (size_t i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}
