#ifndef APP_SHA256_H
#define APP_SHA256_H

#include <stdint.h>

/* SHA256（mbedTLS 封装），用于 desk-recorder/v1 的 WAV 完整性校验。 */
void app_sha256_begin(void);
void app_sha256_update(const void *data, uint32_t len);
void app_sha256_end(uint8_t out[32]);

/* 32 字节摘要 -> 64 字符小写十六进制 + NUL。 */
void app_sha256_hex(const uint8_t digest[32], char out[65]);

#endif
