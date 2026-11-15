#ifndef APP_LINK_H
#define APP_LINK_H

#include <stdint.h>

/* 板间 I2C：ECW 作从机，STM32 作主机写命令。
 *
 * 1 字节命令：触摸/手势/PING/回放。
 * LOGO 读取：主机先写 [0x20, off_lo, off_hi, len_lo, len_hi]，再读 len 字节；
 *            从机在 app_link_serve_logo() 里把 SD 上的 LOGO 块预装进发送缓冲。 */
int  app_link_init(void);
/* 暂时释放 I2C0 从机，供 codec 主机配置使用；配置完再 app_link_init()。 */
void app_link_close(void);
int  app_link_read_byte(uint8_t *cmd, unsigned int timeout_ms);
void app_link_restore_pins(void);

/* 1 字节命令之后的参数字节（同一笔主机写事务里排队的剩余字节）。 */
int app_link_read_args(uint8_t *args, unsigned int n, unsigned int timeout_ms);

/* 记录一次 LOGO 读请求；真正的 SD 读取+发送缓冲填充在主机读事务时由
 * 高优先级任务完成（硬件时钟拉伸等数据）。len 不能超过 APP_LINK_LOGO_CHUNK。 */
void app_link_request_logo(uint8_t seq, uint32_t off, uint32_t len);

/* 在 tx 任务上下文里执行：读 SD 并写 [seq, data] 到从机发送缓冲。 */
int app_link_serve_logo(uint8_t seq, uint32_t off, uint32_t len);

#endif
