#include "app_sha256.h"

#include <stdio.h>
#include <string.h>

#include "oshal.h"
#include "mbedtls/sha256.h"

static mbedtls_sha256_context s_ctx;
static int s_active;

void app_sha256_begin(void)
{
    if (s_active) {
        mbedtls_sha256_free(&s_ctx);
        s_active = 0;
    }
    mbedtls_sha256_init(&s_ctx);
    if (mbedtls_sha256_starts(&s_ctx, 0) != 0) {
        os_printf(LL_ERR, "[sha] start failed\r\n");
        return;
    }
    s_active = 1;
}

void app_sha256_update(const void *data, uint32_t len)
{
    if (!s_active || data == NULL || len == 0u) {
        return;
    }
    if (mbedtls_sha256_update(&s_ctx, (const unsigned char *)data, len) != 0) {
        os_printf(LL_ERR, "[sha] update failed\r\n");
        s_active = 0;
    }
}

void app_sha256_end(uint8_t out[32])
{
    if (out == NULL) {
        return;
    }
    if (!s_active) {
        memset(out, 0, 32);
        return;
    }
    if (mbedtls_sha256_finish(&s_ctx, out) != 0) {
        os_printf(LL_ERR, "[sha] finish failed\r\n");
        memset(out, 0, 32);
    }
    mbedtls_sha256_free(&s_ctx);
    s_active = 0;
}

void app_sha256_hex(const uint8_t digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";

    if (digest == NULL || out == NULL) {
        return;
    }
    for (unsigned i = 0; i < 32u; ++i) {
        out[i * 2u] = hex[digest[i] >> 4];
        out[i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    out[64] = '\0';
}
