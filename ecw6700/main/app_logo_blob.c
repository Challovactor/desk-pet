/*
 * 开机 LOGO 资产块（tools/build_logo_blob.py 生成到 main/assets/logo.bin）。
 *
 * 与音效资产一样由构建脚本复制到 SDK 板级目录，.incbin 以该目录为基准；
 * 首次启动由 app_assets 写入 SD 的音效区尾部（LBA 1536 起），
 * STM32 开机经 I2C 分块读取后刷到墨水屏。
 */
#include <stdint.h>

#include "app_logo_blob.h"

__asm__(
    ".section .rodata\n"
    ".balign 4\n"
    ".global g_logo_blob\n"
    "g_logo_blob:\n"
    ".incbin \"logo.bin\"\n"
    ".global g_logo_blob_end\n"
    "g_logo_blob_end:\n"
    ".previous\n"
);

extern const uint8_t g_logo_blob[];
extern const uint8_t g_logo_blob_end[];

const uint8_t *app_logo_blob(void)
{
    return g_logo_blob;
}

unsigned int app_logo_blob_size(void)
{
    return (unsigned int)(g_logo_blob_end - g_logo_blob);
}
