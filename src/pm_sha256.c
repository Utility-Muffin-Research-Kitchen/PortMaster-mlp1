#include "pm_sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char buffer[64];
} pm_sha256_ctx;

static const uint32_t PM_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24u) |
           ((uint32_t)p[1] << 16u) |
           ((uint32_t)p[2] << 8u) |
           (uint32_t)p[3];
}

static void store_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24u);
    p[1] = (unsigned char)(v >> 16u);
    p[2] = (unsigned char)(v >> 8u);
    p[3] = (unsigned char)v;
}

static void store_be64(unsigned char *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (unsigned char)v;
        v >>= 8u;
    }
}

static void transform(pm_sha256_ctx *ctx, const unsigned char block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = load_be32(block + (i * 4));
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3u);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10u);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + PM_K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void init(pm_sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bit_count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void update(pm_sha256_ctx *ctx, const unsigned char *data, size_t len)
{
    size_t used = (size_t)((ctx->bit_count >> 3u) & 63u);
    ctx->bit_count += (uint64_t)len * 8u;
    if (used > 0) {
        size_t free_space = 64u - used;
        if (len < free_space) {
            memcpy(ctx->buffer + used, data, len);
            return;
        }
        memcpy(ctx->buffer + used, data, free_space);
        transform(ctx, ctx->buffer);
        data += free_space;
        len -= free_space;
    }
    while (len >= 64u) {
        transform(ctx, data);
        data += 64u;
        len -= 64u;
    }
    if (len > 0) {
        memcpy(ctx->buffer, data, len);
    }
}

static void final(pm_sha256_ctx *ctx, unsigned char digest[32])
{
    unsigned char pad[64];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80u;
    unsigned char len_be[8];
    store_be64(len_be, ctx->bit_count);
    size_t used = (size_t)((ctx->bit_count >> 3u) & 63u);
    size_t pad_len = (used < 56u) ? (56u - used) : (120u - used);
    update(ctx, pad, pad_len);
    update(ctx, len_be, sizeof(len_be));
    for (int i = 0; i < 8; i++) {
        store_be32(digest + (i * 4), ctx->state[i]);
    }
}

int pm_sha256_file_hex(const char *path, char out_hex[65], char *err, size_t err_size)
{
    if (out_hex) {
        out_hex[0] = '\0';
    }
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!path || !out_hex) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "missing SHA-256 input");
        }
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot open %s", path);
        }
        return -1;
    }

    pm_sha256_ctx ctx;
    init(&ctx);
    unsigned char buf[32768];
    while (1) {
        size_t got = fread(buf, 1, sizeof(buf), fp);
        if (got > 0) {
            update(&ctx, buf, got);
        }
        if (got < sizeof(buf)) {
            if (ferror(fp)) {
                fclose(fp);
                if (err && err_size > 0) {
                    snprintf(err, err_size, "cannot read %s", path);
                }
                return -1;
            }
            break;
        }
    }
    fclose(fp);

    unsigned char digest[32];
    final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4u];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0fu];
    }
    out_hex[64] = '\0';
    return 0;
}

int pm_sha256_buffer_hex(const void *data, size_t size, char out_hex[65],
                         char *err, size_t err_size)
{
    if (out_hex) {
        out_hex[0] = '\0';
    }
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!out_hex || (!data && size > 0)) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "missing SHA-256 buffer input");
        }
        return -1;
    }

    pm_sha256_ctx ctx;
    init(&ctx);
    if (size > 0) {
        update(&ctx, (const unsigned char *)data, size);
    }
    unsigned char digest[32];
    final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4u];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0fu];
    }
    out_hex[64] = '\0';
    return 0;
}

typedef struct {
    uint32_t state[4];
    uint64_t bit_count;
    unsigned char buffer[64];
} pm_md5_ctx;

static uint32_t rotl32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

static uint32_t load_le32(const unsigned char *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static void store_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8u);
    p[2] = (unsigned char)(v >> 16u);
    p[3] = (unsigned char)(v >> 24u);
}

static void store_le64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)v;
        v >>= 8u;
    }
}

#define PM_MD5_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define PM_MD5_G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define PM_MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define PM_MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define PM_MD5_STEP(f, a, b, c, d, x, t, s) \
    do { \
        (a) += f((b), (c), (d)) + (x) + (uint32_t)(t); \
        (a) = rotl32((a), (s)); \
        (a) += (b); \
    } while (0)

static void md5_transform(pm_md5_ctx *ctx, const unsigned char block[64])
{
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t x[16];

    for (int i = 0; i < 16; i++) {
        x[i] = load_le32(block + (i * 4));
    }

    PM_MD5_STEP(PM_MD5_F, a, b, c, d, x[ 0], 0xd76aa478u,  7);
    PM_MD5_STEP(PM_MD5_F, d, a, b, c, x[ 1], 0xe8c7b756u, 12);
    PM_MD5_STEP(PM_MD5_F, c, d, a, b, x[ 2], 0x242070dbu, 17);
    PM_MD5_STEP(PM_MD5_F, b, c, d, a, x[ 3], 0xc1bdceeeu, 22);
    PM_MD5_STEP(PM_MD5_F, a, b, c, d, x[ 4], 0xf57c0fafu,  7);
    PM_MD5_STEP(PM_MD5_F, d, a, b, c, x[ 5], 0x4787c62au, 12);
    PM_MD5_STEP(PM_MD5_F, c, d, a, b, x[ 6], 0xa8304613u, 17);
    PM_MD5_STEP(PM_MD5_F, b, c, d, a, x[ 7], 0xfd469501u, 22);
    PM_MD5_STEP(PM_MD5_F, a, b, c, d, x[ 8], 0x698098d8u,  7);
    PM_MD5_STEP(PM_MD5_F, d, a, b, c, x[ 9], 0x8b44f7afu, 12);
    PM_MD5_STEP(PM_MD5_F, c, d, a, b, x[10], 0xffff5bb1u, 17);
    PM_MD5_STEP(PM_MD5_F, b, c, d, a, x[11], 0x895cd7beu, 22);
    PM_MD5_STEP(PM_MD5_F, a, b, c, d, x[12], 0x6b901122u,  7);
    PM_MD5_STEP(PM_MD5_F, d, a, b, c, x[13], 0xfd987193u, 12);
    PM_MD5_STEP(PM_MD5_F, c, d, a, b, x[14], 0xa679438eu, 17);
    PM_MD5_STEP(PM_MD5_F, b, c, d, a, x[15], 0x49b40821u, 22);

    PM_MD5_STEP(PM_MD5_G, a, b, c, d, x[ 1], 0xf61e2562u,  5);
    PM_MD5_STEP(PM_MD5_G, d, a, b, c, x[ 6], 0xc040b340u,  9);
    PM_MD5_STEP(PM_MD5_G, c, d, a, b, x[11], 0x265e5a51u, 14);
    PM_MD5_STEP(PM_MD5_G, b, c, d, a, x[ 0], 0xe9b6c7aau, 20);
    PM_MD5_STEP(PM_MD5_G, a, b, c, d, x[ 5], 0xd62f105du,  5);
    PM_MD5_STEP(PM_MD5_G, d, a, b, c, x[10], 0x02441453u,  9);
    PM_MD5_STEP(PM_MD5_G, c, d, a, b, x[15], 0xd8a1e681u, 14);
    PM_MD5_STEP(PM_MD5_G, b, c, d, a, x[ 4], 0xe7d3fbc8u, 20);
    PM_MD5_STEP(PM_MD5_G, a, b, c, d, x[ 9], 0x21e1cde6u,  5);
    PM_MD5_STEP(PM_MD5_G, d, a, b, c, x[14], 0xc33707d6u,  9);
    PM_MD5_STEP(PM_MD5_G, c, d, a, b, x[ 3], 0xf4d50d87u, 14);
    PM_MD5_STEP(PM_MD5_G, b, c, d, a, x[ 8], 0x455a14edu, 20);
    PM_MD5_STEP(PM_MD5_G, a, b, c, d, x[13], 0xa9e3e905u,  5);
    PM_MD5_STEP(PM_MD5_G, d, a, b, c, x[ 2], 0xfcefa3f8u,  9);
    PM_MD5_STEP(PM_MD5_G, c, d, a, b, x[ 7], 0x676f02d9u, 14);
    PM_MD5_STEP(PM_MD5_G, b, c, d, a, x[12], 0x8d2a4c8au, 20);

    PM_MD5_STEP(PM_MD5_H, a, b, c, d, x[ 5], 0xfffa3942u,  4);
    PM_MD5_STEP(PM_MD5_H, d, a, b, c, x[ 8], 0x8771f681u, 11);
    PM_MD5_STEP(PM_MD5_H, c, d, a, b, x[11], 0x6d9d6122u, 16);
    PM_MD5_STEP(PM_MD5_H, b, c, d, a, x[14], 0xfde5380cu, 23);
    PM_MD5_STEP(PM_MD5_H, a, b, c, d, x[ 1], 0xa4beea44u,  4);
    PM_MD5_STEP(PM_MD5_H, d, a, b, c, x[ 4], 0x4bdecfa9u, 11);
    PM_MD5_STEP(PM_MD5_H, c, d, a, b, x[ 7], 0xf6bb4b60u, 16);
    PM_MD5_STEP(PM_MD5_H, b, c, d, a, x[10], 0xbebfbc70u, 23);
    PM_MD5_STEP(PM_MD5_H, a, b, c, d, x[13], 0x289b7ec6u,  4);
    PM_MD5_STEP(PM_MD5_H, d, a, b, c, x[ 0], 0xeaa127fau, 11);
    PM_MD5_STEP(PM_MD5_H, c, d, a, b, x[ 3], 0xd4ef3085u, 16);
    PM_MD5_STEP(PM_MD5_H, b, c, d, a, x[ 6], 0x04881d05u, 23);
    PM_MD5_STEP(PM_MD5_H, a, b, c, d, x[ 9], 0xd9d4d039u,  4);
    PM_MD5_STEP(PM_MD5_H, d, a, b, c, x[12], 0xe6db99e5u, 11);
    PM_MD5_STEP(PM_MD5_H, c, d, a, b, x[15], 0x1fa27cf8u, 16);
    PM_MD5_STEP(PM_MD5_H, b, c, d, a, x[ 2], 0xc4ac5665u, 23);

    PM_MD5_STEP(PM_MD5_I, a, b, c, d, x[ 0], 0xf4292244u,  6);
    PM_MD5_STEP(PM_MD5_I, d, a, b, c, x[ 7], 0x432aff97u, 10);
    PM_MD5_STEP(PM_MD5_I, c, d, a, b, x[14], 0xab9423a7u, 15);
    PM_MD5_STEP(PM_MD5_I, b, c, d, a, x[ 5], 0xfc93a039u, 21);
    PM_MD5_STEP(PM_MD5_I, a, b, c, d, x[12], 0x655b59c3u,  6);
    PM_MD5_STEP(PM_MD5_I, d, a, b, c, x[ 3], 0x8f0ccc92u, 10);
    PM_MD5_STEP(PM_MD5_I, c, d, a, b, x[10], 0xffeff47du, 15);
    PM_MD5_STEP(PM_MD5_I, b, c, d, a, x[ 1], 0x85845dd1u, 21);
    PM_MD5_STEP(PM_MD5_I, a, b, c, d, x[ 8], 0x6fa87e4fu,  6);
    PM_MD5_STEP(PM_MD5_I, d, a, b, c, x[15], 0xfe2ce6e0u, 10);
    PM_MD5_STEP(PM_MD5_I, c, d, a, b, x[ 6], 0xa3014314u, 15);
    PM_MD5_STEP(PM_MD5_I, b, c, d, a, x[13], 0x4e0811a1u, 21);
    PM_MD5_STEP(PM_MD5_I, a, b, c, d, x[ 4], 0xf7537e82u,  6);
    PM_MD5_STEP(PM_MD5_I, d, a, b, c, x[11], 0xbd3af235u, 10);
    PM_MD5_STEP(PM_MD5_I, c, d, a, b, x[ 2], 0x2ad7d2bbu, 15);
    PM_MD5_STEP(PM_MD5_I, b, c, d, a, x[ 9], 0xeb86d391u, 21);

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

static void md5_init(pm_md5_ctx *ctx)
{
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu;
    ctx->state[3] = 0x10325476u;
    ctx->bit_count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void md5_update(pm_md5_ctx *ctx, const unsigned char *data, size_t len)
{
    size_t used = (size_t)((ctx->bit_count >> 3u) & 63u);
    ctx->bit_count += (uint64_t)len * 8u;
    if (used > 0) {
        size_t free_space = 64u - used;
        if (len < free_space) {
            memcpy(ctx->buffer + used, data, len);
            return;
        }
        memcpy(ctx->buffer + used, data, free_space);
        md5_transform(ctx, ctx->buffer);
        data += free_space;
        len -= free_space;
    }
    while (len >= 64u) {
        md5_transform(ctx, data);
        data += 64u;
        len -= 64u;
    }
    if (len > 0) {
        memcpy(ctx->buffer, data, len);
    }
}

static void md5_final(pm_md5_ctx *ctx, unsigned char digest[16])
{
    unsigned char pad[64];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80u;
    unsigned char len_le[8];
    store_le64(len_le, ctx->bit_count);
    size_t used = (size_t)((ctx->bit_count >> 3u) & 63u);
    size_t pad_len = (used < 56u) ? (56u - used) : (120u - used);
    md5_update(ctx, pad, pad_len);
    md5_update(ctx, len_le, sizeof(len_le));
    for (int i = 0; i < 4; i++) {
        store_le32(digest + (i * 4), ctx->state[i]);
    }
}

int pm_md5_file_hex(const char *path, char out_hex[33], char *err, size_t err_size)
{
    if (out_hex) {
        out_hex[0] = '\0';
    }
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!path || !out_hex) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "missing MD5 input");
        }
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot open %s", path);
        }
        return -1;
    }

    pm_md5_ctx ctx;
    md5_init(&ctx);
    unsigned char buf[32768];
    while (1) {
        size_t got = fread(buf, 1, sizeof(buf), fp);
        if (got > 0) {
            md5_update(&ctx, buf, got);
        }
        if (got < sizeof(buf)) {
            if (ferror(fp)) {
                fclose(fp);
                if (err && err_size > 0) {
                    snprintf(err, err_size, "cannot read %s", path);
                }
                return -1;
            }
            break;
        }
    }
    fclose(fp);

    unsigned char digest[16];
    md5_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4u];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0fu];
    }
    out_hex[32] = '\0';
    return 0;
}

#undef PM_MD5_F
#undef PM_MD5_G
#undef PM_MD5_H
#undef PM_MD5_I
#undef PM_MD5_STEP
