#ifndef APP_SDSPI_H
#define APP_SDSPI_H

#include <stdint.h>

/* SPI 模式 SD 卡（6 线：CLK/MOSI/MISO/CS + 3V3/GND）裸扇区访问。 */
int      app_sd_init(void);
int      app_sd_is_ready(void);
uint32_t app_sd_sector_count(void);
uint32_t app_sd_sector_size(void);

/* 返回 0 成功；lba 为块地址（SDHC/SDXC）或扇区地址，驱动内部已处理。 */
int app_sd_read_blocks(uint32_t lba, void *dst, uint32_t count);
int app_sd_write_blocks(uint32_t lba, const void *src, uint32_t count);

#endif
