#ifndef APP_LINK_H
#define APP_LINK_H

#include <stdint.h>

/* 板间 I2C：ECW 作从机，STM32 作主机写 1 字节命令。 */
int  app_link_init(void);
int  app_link_read_byte(uint8_t *cmd, unsigned int timeout_ms);
void app_link_restore_pins(void);

#endif
