/*
 * 最小 HTTPS 客户端（mbedTLS）
 *
 * 参考 SDK examples/https_mbedtls：TLS 1.2、CA 校验 REQUIRED、SNI。
 * 每个请求独立建连；desk-recorder/v1 的保存任务频率很低，实现简单优先。
 */
#include "app_config.h"
#include "app_https.h"
#include "app_ca_cert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oshal.h"

#include "lwip/sockets.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#define HTTPS_HEADER_MAX     768
#define HTTPS_RESPONSE_MAX   1024
#define HTTPS_READ_TIMEOUT_MS 10000u
#define HTTPS_MAX_TRIES      300u

static mbedtls_net_context s_net;
static mbedtls_ssl_context s_ssl;
static mbedtls_ssl_config s_conf;
static mbedtls_x509_crt s_ca;
static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_entropy_context s_entropy;
static int s_open;

static void https_close(void)
{
    if (s_open) {
        (void)mbedtls_ssl_close_notify(&s_ssl);
    }
    mbedtls_net_free(&s_net);
    mbedtls_x509_crt_free(&s_ca);
    mbedtls_ssl_free(&s_ssl);
    mbedtls_ssl_config_free(&s_conf);
    mbedtls_ctr_drbg_free(&s_drbg);
    mbedtls_entropy_free(&s_entropy);
    s_open = 0;
}

static void https_log_mbedtls(const char *what, int ret)
{
    char err[96];

    mbedtls_strerror(ret, err, sizeof(err));
    os_printf(LL_ERR, "[https] %s: -0x%04x (%s)\r\n", what, (unsigned)-ret, err);
}

static int https_open(const char *host, unsigned short port)
{
    char portstr[8];
    int ret;
    static const char pers[] = "moce-https";

    if (app_ca_is_placeholder()) {
        os_printf(LL_ERR, "[https] CA certificate is still a placeholder "
                          "(edit app_ca_cert.c)\r\n");
        return -2;
    }

    mbedtls_net_init(&s_net);
    mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_init(&s_conf);
    mbedtls_x509_crt_init(&s_ca);
    mbedtls_ctr_drbg_init(&s_drbg);
    mbedtls_entropy_init(&s_entropy);

    ret = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                                (const unsigned char *)pers, sizeof(pers) - 1u);
    if (ret != 0) {
        https_log_mbedtls("ctr_drbg_seed", ret);
        goto fail;
    }
    ret = mbedtls_x509_crt_parse(&s_ca,
                                 (const unsigned char *)app_ca_pem(),
                                 strlen(app_ca_pem()) + 1u);
    if (ret < 0) {
        https_log_mbedtls("ca parse", ret);
        goto fail;
    }

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    ret = mbedtls_net_connect(&s_net, host, portstr, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        https_log_mbedtls("tcp connect", ret);
        goto fail;
    }

    ret = mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        https_log_mbedtls("config_defaults", ret);
        goto fail;
    }
    mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&s_conf, &s_ca, NULL);
    mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_drbg);
    mbedtls_ssl_conf_read_timeout(&s_conf, HTTPS_READ_TIMEOUT_MS);

    ret = mbedtls_ssl_setup(&s_ssl, &s_conf);
    if (ret != 0) {
        https_log_mbedtls("ssl_setup", ret);
        goto fail;
    }
    ret = mbedtls_ssl_set_hostname(&s_ssl, host);
    if (ret != 0) {
        https_log_mbedtls("set_hostname", ret);
        goto fail;
    }
    mbedtls_ssl_set_bio(&s_ssl, &s_net, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);

    for (unsigned tries = 0; (ret = mbedtls_ssl_handshake(&s_ssl)) != 0; ++tries) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            https_log_mbedtls("handshake", ret);
            if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                uint32_t flags = mbedtls_ssl_get_verify_result(&s_ssl);
                char info[256];
                mbedtls_x509_crt_verify_info(info, sizeof(info), "  ! ", flags);
                os_printf(LL_ERR, "%s", info);
            }
            goto fail;
        }
        if (tries > 2000u) {
            goto fail;
        }
        os_msleep(2);
    }
    if (mbedtls_ssl_get_verify_result(&s_ssl) != 0u) {
        os_printf(LL_ERR, "[https] server certificate verify failed\r\n");
        goto fail;
    }

    os_printf(LL_INFO, "[https] connected %s:%u %s %s\r\n", host, (unsigned)port,
              mbedtls_ssl_get_version(&s_ssl), mbedtls_ssl_get_ciphersuite(&s_ssl));
    s_open = 1;
    return 0;

fail:
    https_close();
    return -1;
}

static int https_send_all(const void *buf, unsigned int len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned int retries = 0u;

    while (len > 0u) {
        int ret = mbedtls_ssl_write(&s_ssl, p, len);
        if (ret > 0) {
            p += ret;
            len -= (unsigned int)ret;
            retries = 0u;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                   ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (++retries > 15000u) {   /* ~30 s at 2 ms */
                os_printf(LL_ERR, "[https] ssl_write stalled\r\n");
                return -1;
            }
            os_msleep(2);
        } else {
            https_log_mbedtls("ssl_write", ret);
            return -1;
        }
    }
    return 0;
}

static int build_header(char *buf, size_t cap, const char *method,
                        const char *host, const char *path, const char *headers,
                        unsigned int content_length)
{
    int n = snprintf(buf, cap,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: moce-ecw6700/v2\r\n"
                     "%s"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     method, path, host, headers != NULL ? headers : "",
                     content_length);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

static int contains_ci(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);

    for (const char *p = haystack; *p != '\0'; ++p) {
        size_t i;

        for (i = 0; i < n; ++i) {
            char a = p[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
        if (i == n) {
            return 1;
        }
    }
    return 0;
}

static int parse_content_length(const char *raw)
{
    const char *p = raw;

    while ((p = strchr(p, '\n')) != NULL) {
        ++p;
        if (contains_ci(p, "content-length:")) {
            const char *colon = strchr(p, ':');
            if (colon != NULL) {
                return atoi(colon + 1);
            }
        }
    }
    return -1;
}

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static long long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2u;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2u ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (long long)era * 146097ll + (long long)doe - 719468ll;
}

/* Parse "Date: Sat, 14 Nov 2026 20:29:45 GMT" -> UTC epoch milliseconds. */
static long long parse_date_ms(const char *raw)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *p = strstr(raw, "\r\nDate: ");
    char mon[4] = {0};
    int day, year, hh, mm, ss;

    if (p == NULL) {
        p = strstr(raw, "\nDate: ");
    }
    if (p == NULL) {
        return 0;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return 0;
    }
    ++p;
    while (*p == ' ') {
        ++p;
    }
    if (sscanf(p, "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year,
               &hh, &mm, &ss) != 6) {
        return 0;
    }
    const char *mp = strstr(months, mon);
    if (mp == NULL) {
        return 0;
    }
    unsigned month = (unsigned)((mp - months) / 3) + 1u;
    long long days = days_from_civil(year, month, (unsigned)day);
    return (days * 86400ll + hh * 3600ll + mm * 60ll + ss) * 1000ll;
}

static int https_read_response(struct app_http_response *resp)
{
    char raw[HTTPS_RESPONSE_MAX];
    unsigned int total = 0u;
    unsigned int tries = 0u;
    int content_length = -1;
    int header_end = -1;
    int ret;

    memset(resp, 0, sizeof(*resp));
    resp->status = -1;

    for (;;) {
        if (total >= sizeof(raw) - 1u) {
            break;
        }
        ret = mbedtls_ssl_read(&s_ssl, (unsigned char *)raw + total,
                               sizeof(raw) - 1u - total);
        if (ret > 0) {
            total += (unsigned int)ret;
            raw[total] = '\0';
            if (header_end < 0) {
                char *e = strstr(raw, "\r\n\r\n");
                if (e != NULL) {
                    header_end = (int)(e - raw) + 4;
                    content_length = parse_content_length(raw);
                }
            }
            if (header_end >= 0 && content_length >= 0 &&
                (int)total >= header_end + content_length) {
                break;
            }
            tries = 0u;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                   ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (++tries > HTTPS_MAX_TRIES * 10u) {
                break;
            }
            os_msleep(2);
        } else if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            /* 头部已收到但 body 可能还在后续 TLS 记录里（服务端常见）。
             * 只要 Content-Length 还没满足（或 chunked 未知长度），就再等
             * 约 300ms；否则直接按已收到的解析。 */
            if (header_end >= 0 &&
                (content_length < 0 ||
                 (int)total < header_end + content_length) &&
                ++tries <= 150u) {
                os_msleep(2);
                continue;
            }
            break;
        } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
            break;
        } else {
            https_log_mbedtls("ssl_read", ret);
            break;
        }
    }

    raw[total] = '\0';
    if (total == 0u) {
        os_printf(LL_WARN, "[https] empty response\r\n");
        return -1;
    }
    resp->date_ms = parse_date_ms(raw);
    if (sscanf(raw, "HTTP/%*d.%*d %d", &resp->status) != 1) {
        resp->status = -1;
        return -1;
    }
    if (header_end < 0) {
        header_end = 0;
    }
    {
        int body_avail = (int)total - header_end;
        int copy = body_avail < (int)sizeof(resp->body) - 1
                       ? body_avail
                       : (int)sizeof(resp->body) - 1;
        if (copy > 0) {
            memcpy(resp->body, raw + header_end, (size_t)copy);
            resp->body[copy] = '\0';
        }
        resp->body_len = copy > 0 ? copy : 0;
    }
    os_printf(LL_INFO, "[https] %d (%u bytes)\r\n", resp->status,
              (unsigned)total);
    if (resp->body_len > 0) {
        /* 打印响应体便于定位业务错误（error.code）和 202 的 job_id */
        os_printf(LL_INFO, "[https] body: %.220s\r\n", resp->body);
    } else if (resp->status >= 300) {
        os_printf(LL_WARN, "[https] no body (status %d)\r\n", resp->status);
    }
    return resp->status >= 0 ? 0 : -1;
}

static int https_do_request(const char *method, const char *host,
                            unsigned short port, const char *path,
                            const char *headers, unsigned int content_length,
                            const void *body,
                            int (*read_body)(void *, void *, unsigned int),
                            void *ctx, struct app_http_response *resp)
{
    char hdr[HTTPS_HEADER_MAX];
    int hn;
    int ret;

    if (resp != NULL) {
        memset(resp, 0, sizeof(*resp));
        resp->status = -1;
    }
    if (host == NULL || path == NULL || resp == NULL) {
        return -1;
    }
    if (https_open(host, port) != 0) {
        return -1;
    }
    hn = build_header(hdr, sizeof(hdr), method, host, path, headers, content_length);
    if (hn <= 0) {
        https_close();
        return -1;
    }
    if (https_send_all(hdr, (unsigned int)hn) != 0) {
        https_close();
        return -1;
    }

    if (body != NULL) {
        if (https_send_all(body, content_length) != 0) {
            https_close();
            return -1;
        }
    } else if (read_body != NULL) {
        unsigned char chunk[2048];
        unsigned int left = content_length;

        while (left > 0u) {
            unsigned int want = left < sizeof(chunk) ? left : (unsigned int)sizeof(chunk);
            int got = read_body(ctx, chunk, want);

            if (got <= 0) {
                os_printf(LL_ERR, "[https] body reader returned %d\r\n", got);
                https_close();
                return -1;
            }
            if (https_send_all(chunk, (unsigned int)got) != 0) {
                https_close();
                return -1;
            }
            left -= (unsigned int)got;
            /* 大文件上传时 TLS 加密 + SD 读是连续负载：每块留 5ms，
             * 避免 Wi-Fi 驱动被饿死/位翻转噪声干扰 RF。 */
            os_msleep(5);
        }
    }

    ret = https_read_response(resp);
    https_close();
    return ret;
}

int app_https_request(const char *method, const char *host, unsigned short port,
                      const char *path, const char *headers,
                      const void *body, unsigned int body_len,
                      struct app_http_response *resp)
{
    return https_do_request(method, host, port, path, headers, body_len,
                            body, NULL, NULL, resp);
}

int app_https_request_stream(const char *method, const char *host, unsigned short port,
                             const char *path, const char *headers,
                             unsigned int content_length,
                             int (*read_body)(void *ctx, void *buf, unsigned int len),
                             void *ctx, struct app_http_response *resp)
{
    return https_do_request(method, host, port, path, headers, content_length,
                            NULL, read_body, ctx, resp);
}
