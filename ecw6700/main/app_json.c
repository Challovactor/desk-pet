#include "app_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_value(const char *json, const char *key)
{
    char pattern[48];
    int n;
    const char *p;

    if (json == NULL || key == NULL) {
        return NULL;
    }
    n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return NULL;
    }
    p = strstr(json, pattern);
    if (p == NULL) {
        return NULL;
    }
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (*p != ':') {
        return NULL;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    return p;
}

int app_json_get_string(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = find_value(json, key);
    size_t n = 0;

    if (p == NULL || out == NULL || cap == 0u || *p != '"') {
        return -1;
    }
    ++p;
    while (*p != '\0' && *p != '"' && n + 1u < cap) {
        if (*p == '\\' && p[1] != '\0') {
            ++p;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return *p == '"' ? 0 : -1;
}

int app_json_get_int(const char *json, const char *key, long long *out)
{
    const char *p = find_value(json, key);
    char *end = NULL;
    long long v;

    if (p == NULL || out == NULL) {
        return -1;
    }
    v = strtoll(p, &end, 10);
    if (end == p) {
        return -1;
    }
    *out = v;
    return 0;
}
