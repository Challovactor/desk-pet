#ifndef APP_SOUNDS_BLOB_H
#define APP_SOUNDS_BLOB_H

#include <stdint.h>

/* 固件内嵌的情绪音效二进制块（由 app_sounds_blob.c 的 .incbin 定义）。 */
const uint8_t *app_sound_blob(void);
unsigned int app_sound_blob_size(void);

#endif
