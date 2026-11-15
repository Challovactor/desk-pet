/*
 * 情绪音效二进制资产（由 ecw6700/tools/build_sound_blob.py 生成）。
 *
 * 构建脚本会把 main/assets/animal_sounds.bin 复制到 SDK 的板级目录，
 * 这里的 .incbin 以该目录为基准；app_assets.c 通过 g_sound_blob 引用。
 * 资产约 190 KB，放在 flash 的 .rodata（XIP），首次启动写入 SD 保留区。
 */
#include <stdint.h>

__asm__(
    ".section .rodata\n"
    ".balign 4\n"
    ".global g_sound_blob\n"
    "g_sound_blob:\n"
    ".incbin \"animal_sounds.bin\"\n"
    ".global g_sound_blob_end\n"
    "g_sound_blob_end:\n"
    ".previous\n"
);

extern const uint8_t g_sound_blob[];
extern const uint8_t g_sound_blob_end[];

const uint8_t *app_sound_blob(void)
{
    return g_sound_blob;
}

unsigned int app_sound_blob_size(void)
{
    return (unsigned int)(g_sound_blob_end - g_sound_blob);
}
