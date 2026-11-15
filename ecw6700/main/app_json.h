#ifndef APP_JSON_H
#define APP_JSON_H

#include <stddef.h>

/*
 * 极简 JSON 取值器：只用于服务端返回的小对象（job_id/state/minutes）。
 * 不是通用解析器；第一个匹配的 key 生效，适合 desk-recorder/v1 的扁平字段。
 */
int app_json_get_string(const char *json, const char *key, char *out, size_t cap);
/* 服务端时间戳约 1.8e12，rv32 的 long 只有 32 位，必须用 long long。 */
int app_json_get_int(const char *json, const char *key, long long *out);

#endif
