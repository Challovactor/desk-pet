#ifndef APP_LOGO_BLOB_H
#define APP_LOGO_BLOB_H

#include <stdint.h>

/* 固件内嵌的开机 LOGO 资产块（tools/build_logo_blob.py 生成，
 * app_logo_blob.c 用 .incbin 引入）。 */
const uint8_t *app_logo_blob(void);
unsigned int app_logo_blob_size(void);

#endif
