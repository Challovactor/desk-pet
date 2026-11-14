/*
 * 板间 I2C 从机（ECW6700F 侧）
 *
 * 角色分配：STM32 作主机，触摸按下时向 0x42 写 1 字节命令；
 * ECW 作从机，在任务上下文里把命令读出来。这样避开了 STM32 侧
 * 阻塞式 I2C 从机的所有时序问题，链路只剩"主机写、从机收"。
 *
 * 引脚：BOARD_I2C_0 = IO24(SCL) / IO23(SDA)，pinmux 由 board.c 的
 * __wrap_chip_i2c_pinmux_init 在每次 open 时绑定。
 */
#include "app_config.h"
#include "app_link.h"

#include <stddef.h>
#include <string.h>

#include "oshal.h"
#include "i2c_slave.h"
#include "gpio.h"
#include "chip_pinmux.h"
#include "chip_smuaon.h"

static void *s_slave;
static volatile uint32_t s_rx_events;
static int s_ready;

static void link_rx_done(unsigned int rx_num, void *arg)
{
    (void)rx_num;
    (void)arg;
    ++s_rx_events;
}

void app_link_restore_pins(void)
{
    /* I2S 初始化会重映射默认引脚，可能覆盖 IO24/IO23；这里恢复 I2C0。 */
    set_pin_function(24u, FUNC_I2C0_SCL);
    set_pin_function(23u, FUNC_I2C0_SDA);
    if (drv_gpio_set_pull_type(24u, DRV_GPIO_ARG_PULL_UP) != GPIO_RET_SUCCESS ||
        drv_gpio_set_pull_type(23u, DRV_GPIO_ARG_PULL_UP) != GPIO_RET_SUCCESS) {
        os_printf(LL_WARN, "[link] internal pull-up setup failed\r\n");
    }
}

int app_link_init(void)
{
    T_DRV_I2C_SLAVE_CONFIG cfg;

    if (s_ready) {
        return 0;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.num = APP_LINK_I2C_NUM;
    cfg.addr_len = I2C_ADDR_S_LEN_7;
    cfg.addr_val = APP_LINK_I2C_ADDR7;
    cfg.rx_buffer_len = APP_LINK_RX_BUF;
    cfg.tx_buffer_len = APP_LINK_TX_BUF;

    s_slave = drv_i2c_slave_open(&cfg);
    if (s_slave == NULL) {
        os_printf(LL_ERR, "[link] i2c slave open failed\r\n");
        return -1;
    }
    drv_i2c_slave_register_rx_done(s_slave, link_rx_done, NULL);
    app_link_restore_pins();

    s_ready = 1;
    os_printf(LL_INFO, "[link] i2c slave ready: addr=0x%02x\r\n",
              (unsigned)APP_LINK_I2C_ADDR7);
    return 0;
}

int app_link_read_byte(uint8_t *cmd, unsigned int timeout_ms)
{
    /* 驱动在部分读超时后仍把 buf_user 指向调用者缓冲，ISR 可能迟到写入；
     * 因此必须用静态缓冲，不能把栈地址交给驱动。 */
    static uint8_t s_rx_byte;
    int n;

    if (!s_ready || cmd == NULL) {
        return -1;
    }
    /* 注意：该 API 在超时时返回 0，正常读到 1 字节时返回 1。 */
    n = drv_i2c_slave_read(s_slave, &s_rx_byte, 1u, timeout_ms);
    if (n != 1) {
        return -1;
    }
    *cmd = s_rx_byte;
    return 0;
}
