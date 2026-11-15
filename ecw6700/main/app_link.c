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
#include "app_assets.h"

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

/* LOGO 分块读取的"读时服务"模型（与 SDK 官方从机示例一致）：
 *   1. 触发任务解析 [0x20, 0x5A, seq, off, len, chk]，只记录 pending；
 *   2. 主机发起读事务且 TX 缓冲为空 -> 驱动 ISR 回调，只 post 信号量；
 *   3. 高优先级 tx 任务被唤醒，按 pending 现场读 SD 并 drv_i2c_slave_write。
 *      I2C 硬件在 FIFO 空时拉伸 SCL，主机 HALL 读会一直等到数据写入，
 *      因此不存在"读得太早读到占位数据"的竞争。
 * 若读事务没有对应的 pending（杂散读），填 0xFF 防止欠载。 */
static uint8_t s_tx_dummy[APP_LINK_LOGO_CHUNK + 1u];
static os_sem_handle_t s_tx_req_sem;
static volatile int s_logo_pending;
static volatile uint8_t s_logo_pend_seq;
static volatile uint32_t s_logo_pend_off;
static volatile uint32_t s_logo_pend_len;

static void link_request_tx(void *arg)
{
    (void)arg;
    if (s_tx_req_sem != NULL) {
        os_sem_post(s_tx_req_sem);
    }
}

static void link_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (os_sem_wait(s_tx_req_sem, WAIT_FOREVER) != 0) {
            continue;
        }
        if (s_slave == NULL) {
            continue;
        }
        if (s_logo_pending) {
            uint8_t seq = s_logo_pend_seq;
            uint32_t off = s_logo_pend_off;
            uint32_t len = s_logo_pend_len;

            s_logo_pending = 0;
            (void)app_link_serve_logo(seq, off, len);
        } else {
            memset(s_tx_dummy, 0xFF, sizeof(s_tx_dummy));
            (void)drv_i2c_slave_write(s_slave, s_tx_dummy, sizeof(s_tx_dummy));
        }
    }
}

void app_link_request_logo(uint8_t seq, uint32_t off, uint32_t len)
{
    s_logo_pend_off = off;
    s_logo_pend_len = len;
    s_logo_pend_seq = seq;
    s_logo_pending = 1;
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
    if (s_tx_req_sem == NULL) {
        s_tx_req_sem = os_sem_create(1, 0);
    }
    if (s_tx_req_sem != NULL) {
        static int s_tx_task_started;

        drv_i2c_slave_register_request_tx(s_slave, link_request_tx, NULL);
        if (!s_tx_task_started) {
            /* 优先级高于采集任务，保证主机读事务的时钟拉伸窗口内尽快补数。 */
            (void)os_task_create("app-linktx", APP_TASK_TRIGGER_PRIO + 2, 2048,
                                 link_tx_task, NULL);
            s_tx_task_started = 1;
        }
    }
    app_link_restore_pins();

    s_ready = 1;
    os_printf(LL_INFO, "[link] i2c slave ready: addr=0x%02x\r\n",
              (unsigned)APP_LINK_I2C_ADDR7);
    return 0;
}

void app_link_close(void)
{
    if (s_slave != NULL) {
        drv_i2c_slave_close(s_slave);
        s_slave = NULL;
    }
    s_ready = 0;
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

int app_link_read_args(uint8_t *args, unsigned int n, unsigned int timeout_ms)
{
    static uint8_t s_args[8];
    int got;

    if (!s_ready || args == NULL || n == 0u || n > sizeof(s_args)) {
        return -1;
    }
    got = drv_i2c_slave_read(s_slave, s_args, n, timeout_ms);
    if (got != (int)n) {
        return -1;
    }
    memcpy(args, s_args, n);
    return 0;
}

/* LOGO 分块读取：SD 读一次（<=200 B），预装 [seq, data] 进从机 TX 缓冲。
 * 主机在写请求后等约 10 ms 再读；主机校验回传的 seq，过期数据会被丢弃重试。 */
static uint8_t s_logo_tx[APP_LINK_LOGO_CHUNK + 1u] __attribute__((aligned(4)));

int app_link_serve_logo(uint8_t seq, uint32_t off, uint32_t len)
{
    int ok = 0;

    if (!s_ready || len == 0u || len > APP_LINK_LOGO_CHUNK) {
        return -1;
    }
    /* 始终填满整个 TX 缓冲，数据后面补 0xFF：主机只读 len+1 字节，
     * 从机缓冲里仍有剩余数据，I2C 控制器在主机 NACK/STOP 时不会进入
     * "FIFO 空 -> 拉伸 SCL" 状态（那会让主机发不出 STOP，后续事务
     * 全部 HAL_BUSY）。 */
    memset(s_logo_tx, 0xFF, sizeof(s_logo_tx));
    if (assets_logo_read(off, &s_logo_tx[1], len) != 0) {
        os_printf(LL_WARN, "[link] logo read off=%lu len=%lu failed\r\n",
                  (unsigned long)off, (unsigned long)len);
        /* 未就绪：seq 取反，让主机识别为无效数据并重试 */
        s_logo_tx[0] = (uint8_t)(seq ^ 0xFFu);
        ok = -1;
    } else {
        s_logo_tx[0] = seq;
    }
    (void)drv_i2c_slave_write(s_slave, s_logo_tx, sizeof(s_logo_tx));
    return ok;
}
