#ifndef APP_HTTPS_H
#define APP_HTTPS_H

#include <stdint.h>

/*
 * 最小 HTTPS 客户端（mbedTLS，TLS 1.2，CA 校验，SNI/主机名校验）。
 *
 * desk-recorder/v1 的 HTTP 接口都在 443；这里不实现 keep-alive/重定向，
 * 每个请求新建一条 TLS 连接，适合低频的保存任务。
 */

struct app_http_response {
    int status;          /* HTTP 状态码；<0 表示传输/TLS 失败 */
    int body_len;        /* 复制到 body 的字节数 */
    long long date_ms;   /* 响应头 Date 的 UTC Unix 毫秒；0=没解析到 */
    char body[768];      /* 响应体（截断），用于解析 job_id/state/minutes 等 */
};

/* 普通请求：body 一次性提供（小 JSON）。headers 可含多行，必须以 \r\n 结尾。 */
int app_https_request(const char *method, const char *host, unsigned short port,
                      const char *path, const char *headers,
                      const void *body, unsigned int body_len,
                      struct app_http_response *resp);

/* 流式请求：按 content_length 从 read_body 取数据（用于大 WAV PUT）。 */
int app_https_request_stream(const char *method, const char *host, unsigned short port,
                             const char *path, const char *headers,
                             unsigned int content_length,
                             int (*read_body)(void *ctx, void *buf, unsigned int len),
                             void *ctx, struct app_http_response *resp);

#endif
