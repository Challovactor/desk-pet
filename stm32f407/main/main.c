/*
 * MOCE 触摸事件桥（STM32F407VET6）
 *
 * 新架构职责（2026-11 迭代）：
 *   2.0TP 触摸屏（CST816D, I2C3） -> 检测到按下 -> 通过 I2C1 主机写 1 字节事件
 *   给 ECW6700（I2C 从机，地址 0x42）。录音、SD 存储、Wi-Fi 上传全部在 ECW6700
 *   侧完成，本板不再接 SD 卡、不再驱动墨水屏。
 *
 * 板级连线（扩展板 expansion-20260915 / 主板 schematic-20261005）：
 *   - 触摸屏 U1: I2C3  PA8=SCL, PC9=SDA, RESET=PD15, INT=PD10（轮询，不用 INT）
 *   - 板间链路 : I2C1  PB8=SCL, PB9=SDA（接 ECW6700 的 IO24/IO23）
 *   - 调试串口 : USART3 PB10=TX, PB11=RX, 115200 8N1
 *
 * 设计要点：
 *   - 只做单向 1 字节命令，不实现变长帧/应答，避免 HAL 从机时序问题；
 *   - 触摸按下沿触发，800ms 去抖，整屏任意位置都算数；
 *   - I2C1 发送失败时做总线恢复（9 个 SCL 脉冲 + STOP）后重试；
 *   - 触摸控制器初始化失败会周期重试，不阻塞主循环。
 */
#include "stm32f4xx_hal.h"
#include "app_epd.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ 配置 */
#define TOUCH_I2C_ADDR      (0x15u << 1)  /* CST816D 7-bit 0x15 */
#define LINK_I2C_ADDR7      0x42u         /* ECW6700 I2C 从机 7-bit 地址 */
#define LINK_I2C_ADDR8      (LINK_I2C_ADDR7 << 1)
#define LINK_EVT_TOUCH      0x01u         /* 触摸 -> 记录并上传 */
#define LINK_EVT_PING       0x02u         /* 预留：心跳 */

#define TOUCH_POLL_MS       20u
#define TOUCH_DEBOUNCE_MS   800u
#define TOUCH_RETRY_MS      2000u
#define LINK_TX_TIMEOUT_MS  30u
#define LINK_RETRY          3u

/* ------------------------------------------------------------- HAL 句柄 */
static I2C_HandleTypeDef hi2c1;   /* 板间链路，主机 */
static I2C_HandleTypeDef hi2c3;   /* 触摸屏 */
static UART_HandleTypeDef huart3; /* 调试日志 */

/* --------------------------------------------------------------- 计数器 */
static volatile uint32_t stat_touch_events;
static volatile uint32_t stat_link_ok;
static volatile uint32_t stat_link_fail;
static volatile uint32_t stat_touch_err;
static volatile uint32_t stat_bus_recover;

/* ----------------------------------------------------------------- 日志 */
static void log_printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)buf, (uint16_t)n, 50);
}

/* ----------------------------------------------------------------- 时钟 */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef o = {0};
    RCC_ClkInitTypeDef c = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    o.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    o.HSEState = RCC_HSE_ON;
    o.PLL.PLLState = RCC_PLL_ON;
    o.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    o.PLL.PLLM = 8;    /* 8 MHz / 8 = 1 MHz */
    o.PLL.PLLN = 336;  /* 336 MHz VCO */
    o.PLL.PLLP = RCC_PLLP_DIV2; /* 168 MHz SYSCLK */
    o.PLL.PLLQ = 7;    /* 48 MHz */
    if (HAL_RCC_OscConfig(&o) != HAL_OK) { for (;;) { } }

    c.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    c.AHBCLKDivider = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV4;  /* 42 MHz: I2C1/I2C3/USART3 */
    c.APB2CLKDivider = RCC_HCLK_DIV2;  /* 84 MHz */
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_5) != HAL_OK) { for (;;) { } }
}

/* ----------------------------------------------------------------- GPIO */
static void gpio_out(GPIO_TypeDef *bank, uint32_t pin)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = pin;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(bank, &g);
}

static void gpio_in(GPIO_TypeDef *bank, uint32_t pin, uint32_t pull)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = pin;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = pull;
    HAL_GPIO_Init(bank, &g);
}

static void gpio_af(GPIO_TypeDef *bank, uint32_t pin, uint32_t af, uint32_t mode,
                    uint32_t pull)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = pin;
    g.Mode = mode;
    g.Pull = pull;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = af;
    HAL_GPIO_Init(bank, &g);
}

/* ------------------------------------------------------------- 外设初始化 */
static int peripherals_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_I2C3_CLK_ENABLE();
    __HAL_RCC_USART3_CLK_ENABLE();

    /* 板间 I2C1：PB8/PB9，开漏 + 内部上拉（ECW 侧也有内部上拉） */
    gpio_af(GPIOB, GPIO_PIN_8, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);
    gpio_af(GPIOB, GPIO_PIN_9, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);

    /* 触摸 I2C3：PA8/PC9 */
    gpio_af(GPIOA, GPIO_PIN_8, GPIO_AF4_I2C3, GPIO_MODE_AF_OD, GPIO_PULLUP);
    gpio_af(GPIOC, GPIO_PIN_9, GPIO_AF4_I2C3, GPIO_MODE_AF_OD, GPIO_PULLUP);

    gpio_out(GPIOD, GPIO_PIN_15);              /* TOUCH RESET */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15, GPIO_PIN_SET);
    gpio_in(GPIOD, GPIO_PIN_10, GPIO_PULLUP);  /* TOUCH INT（轮询模式不依赖） */

    /* USART3：PB10=TX, PB11=RX, AF7 */
    gpio_af(GPIOB, GPIO_PIN_10, GPIO_AF7_USART3, GPIO_MODE_AF_PP, GPIO_PULLUP);
    gpio_af(GPIOB, GPIO_PIN_11, GPIO_AF7_USART3, GPIO_MODE_AF_PP, GPIO_PULLUP);

    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 100000;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) return -1;

    hi2c3.Instance = I2C3;
    hi2c3.Init = hi2c1.Init;
    if (HAL_I2C_Init(&hi2c3) != HAL_OK) return -1;

    huart3.Instance = USART3;
    huart3.Init.BaudRate = 115200;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart3) != HAL_OK) return -1;

    return 0;
}

/* ---------------------------------------------------------- I2C 总线恢复 */
/* 主机被从机拉住 SCL 或状态机异常时，用 GPIO 模拟 9 个时钟 + STOP 复位总线。 */
static void i2c1_bus_recover(void)
{
    GPIO_InitTypeDef g = {0};

    stat_bus_recover++;
    (void)HAL_I2C_DeInit(&hi2c1);

    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_8;
    HAL_GPIO_Init(GPIOB, &g);          /* SCL */
    g.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOB, &g);          /* SDA */

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
    for (int i = 0; i < 9; ++i) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);
        HAL_Delay(1);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
        HAL_Delay(1);
    }
    /* 手动 STOP：SCL 高时 SDA 由低变高 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
    HAL_Delay(1);

    gpio_af(GPIOB, GPIO_PIN_8, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);
    gpio_af(GPIOB, GPIO_PIN_9, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);
    __HAL_RCC_I2C1_CLK_ENABLE();   /* DeInit 会关时钟，Init 不会重新开 */
    (void)HAL_I2C_Init(&hi2c1);
}

/* --------------------------------------------------------------- 触摸驱动 */
static int touch_ready;
static int module_powered;

/* 2.0TP 模组（触摸与显示同一块板）的显示侧控制脚：
 * LCD RESET=PA11, CS=PC11, DC=PC13, 背光/使能=PD14。
 * 之前只驱动了触摸 RESET(PD15)，模组可能没有完全上电/释放复位，
 * 导致 CST816D 不应答 I2C。这里按“可行代码”的顺序先把模组唤醒。 */
static void board_module_power_on(void)
{
    if (module_powered) {
        return;
    }
    module_powered = 1;

    gpio_out(GPIOA, GPIO_PIN_11);
    gpio_out(GPIOC, GPIO_PIN_11);
    gpio_out(GPIOC, GPIO_PIN_13);
    gpio_out(GPIOD, GPIO_PIN_14);

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_11, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_11, GPIO_PIN_RESET);
    HAL_Delay(5);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_11, GPIO_PIN_SET);
    HAL_Delay(150);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET); /* 背光/模组使能 */
    HAL_Delay(50);
    log_printf("[stm32] display module powered (PA11/PC11/PC13/PD14)\r\n");
}

/* I2C3 = PA8(SCL) / PC9(SDA)。总线被拉死时用 9 个时钟 + STOP 复位。 */
static void i2c3_bus_recover(void)
{
    GPIO_InitTypeDef g = {0};

    (void)HAL_I2C_DeInit(&hi2c3);

    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_8;
    HAL_GPIO_Init(GPIOA, &g);          /* SCL */
    g.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOC, &g);          /* SDA */

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);
    for (int i = 0; i < 9; ++i) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
        HAL_Delay(1);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
        HAL_Delay(1);
    }
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);
    HAL_Delay(1);

    gpio_af(GPIOA, GPIO_PIN_8, GPIO_AF4_I2C3, GPIO_MODE_AF_OD, GPIO_PULLUP);
    gpio_af(GPIOC, GPIO_PIN_9, GPIO_AF4_I2C3, GPIO_MODE_AF_OD, GPIO_PULLUP);
    __HAL_RCC_I2C3_CLK_ENABLE();   /* DeInit 会关时钟，Init 不会重新开 */
    (void)HAL_I2C_Init(&hi2c3);
    log_printf("[stm32] i2c3 bus recovered\r\n");
}

/* ---- 软件 I2C 诊断：不依赖 HAL 外设，在候选引脚对上扫描 ---- */
struct bb_pair {
    const char *name;
    GPIO_TypeDef *scl_port;
    uint16_t scl_pin;
    GPIO_TypeDef *sda_port;
    uint16_t sda_pin;
};

static void bb_delay(void)
{
    for (volatile unsigned i = 0; i < 100u; ++i) {
    }
}

static void bb_init(const struct bb_pair *p)
{
    GPIO_InitTypeDef g = {0};

    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = p->scl_pin;
    HAL_GPIO_Init(p->scl_port, &g);
    g.Pin = p->sda_pin;
    HAL_GPIO_Init(p->sda_port, &g);
    HAL_GPIO_WritePin(p->scl_port, p->scl_pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(p->sda_port, p->sda_pin, GPIO_PIN_SET);
    bb_delay();
}

static int bb_probe(const struct bb_pair *p, uint8_t addr7)
{
    int ack;

    /* START */
    HAL_GPIO_WritePin(p->sda_port, p->sda_pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(p->scl_port, p->scl_pin, GPIO_PIN_SET);
    bb_delay();
    HAL_GPIO_WritePin(p->sda_port, p->sda_pin, GPIO_PIN_RESET);
    bb_delay();
    HAL_GPIO_WritePin(p->scl_port, p->scl_pin, GPIO_PIN_RESET);
    bb_delay();

    for (int bit = 7; bit >= 0; --bit) {
        if ((addr7 >> bit) & 1u) {
            HAL_GPIO_WritePin(p->sda_port, p->sda_pin, GPIO_PIN_SET);
        } else {
            HAL_GPIO_WritePin(p->sda_port, p->sda_pin, GPIO_PIN_RESET);
        }
        bb_delay();
        HAL_GPIO_WritePin(p->scl_port, p->scl_pin, GPIO_PIN_SET);
        bb_delay();
        HAL_GPIO_WritePin(p->scl_port, p->scl_pin, GPIO_PIN_RESET);
        bb_delay();
    }

    /* 释放 SDA，第 9 个时钟读 ACK */
    HAL_GPIO_WritePin(p->sda_port, p->sda_pin, GPIO_PIN_SET);
    bb_delay();
    HAL_GPIO_WritePin(p->scl_port, p->scl_pin, GPIO_PIN_SET);
    bb_delay();
    ack = (HAL_GPIO_ReadPin(p->sda_port, p->sda_pin) == GPIO_PIN_RESET);
    HAL_GPIO_WritePin(p->scl_port, p->scl_pin, GPIO_PIN_RESET);
    bb_delay();

    /* STOP */
    HAL_GPIO_WritePin(p->sda_port, p->sda_pin, GPIO_PIN_RESET);
    bb_delay();
    HAL_GPIO_WritePin(p->scl_port, p->scl_pin, GPIO_PIN_SET);
    bb_delay();
    HAL_GPIO_WritePin(p->sda_port, p->sda_pin, GPIO_PIN_SET);
    bb_delay();
    return ack;
}

static void bitbang_scan_pairs(void)
{
    static const struct bb_pair pairs[] = {
        {"PA8->SCL PC9->SDA", GPIOA, GPIO_PIN_8, GPIOC, GPIO_PIN_9},
        {"PC9->SCL PA8->SDA", GPIOC, GPIO_PIN_9, GPIOA, GPIO_PIN_8},
        {"PB6->SCL PB7->SDA", GPIOB, GPIO_PIN_6, GPIOB, GPIO_PIN_7},
        {"PB7->SCL PB6->SDA", GPIOB, GPIO_PIN_7, GPIOB, GPIO_PIN_6},
        {"PB8->SCL PB9->SDA", GPIOB, GPIO_PIN_8, GPIOB, GPIO_PIN_9},
        {"PB9->SCL PB8->SDA", GPIOB, GPIO_PIN_9, GPIOB, GPIO_PIN_8},
    };
    const unsigned n = sizeof(pairs) / sizeof(pairs[0]);

    for (unsigned i = 0; i < n; ++i) {
        int any = 0;
        bb_init(&pairs[i]);
        for (uint8_t a = 0x08u; a < 0x78u; ++a) {
            if (bb_probe(&pairs[i], a)) {
                log_printf("[stm32] bb %s: ACK 0x%02X\r\n", pairs[i].name, a);
                any = 1;
            }
        }
        if (!any) {
            log_printf("[stm32] bb %s: none\r\n", pairs[i].name);
        }
    }

    /* 恢复被借用的硬件总线引脚 */
    gpio_af(GPIOA, GPIO_PIN_8, GPIO_AF4_I2C3, GPIO_MODE_AF_OD, GPIO_PULLUP);
    gpio_af(GPIOC, GPIO_PIN_9, GPIO_AF4_I2C3, GPIO_MODE_AF_OD, GPIO_PULLUP);
    gpio_af(GPIOB, GPIO_PIN_8, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);
    gpio_af(GPIOB, GPIO_PIN_9, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);
    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_I2C3_CLK_ENABLE();
    (void)HAL_I2C_Init(&hi2c1);
    (void)HAL_I2C_Init(&hi2c3);
}

/* 触摸地址未知时的上板诊断：扫描 I2C3 上所有应答的 7 位地址。 */
static void touch_scan_bus(void)
{
    int found = 0;

    bitbang_scan_pairs();
    i2c3_bus_recover();
    log_printf("[stm32] scanning I2C3 (PA8/PC9)...\r\n");
    for (uint8_t a = 0x08u; a < 0x78u; ++a) {
        if (HAL_I2C_IsDeviceReady(&hi2c3, (uint16_t)((uint16_t)a << 1), 1, 5) == HAL_OK) {
            log_printf("[stm32]   found 0x%02X\r\n", a);
            found = 1;
        }
    }
    if (!found) {
        log_printf("[stm32] no I2C device on PA8/PC9\r\n");
    }
    log_printf("[stm32] scan done\r\n");
}

static int touch_init(void)
{
    uint8_t id = 0;

    /* 先给整个 2.0TP 模组上电/释放 LCD 复位，再复位触摸控制器。 */
    board_module_power_on();

    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15, GPIO_PIN_SET);
    HAL_Delay(200);

    /* 只读 1 字节：部分 CST816 批次不会自动递增到 0xA8/0xA9 */
    if (HAL_I2C_Mem_Read(&hi2c3, TOUCH_I2C_ADDR, 0xa7, I2C_MEMADD_SIZE_8BIT,
                         &id, 1, 50) != HAL_OK) {
        static int scanned;
        if (!scanned) {
            scanned = 1;
            touch_scan_bus();
        }
        return -1;
    }
    log_printf("[stm32] touch chip id=0x%02X\r\n", id);
    /* CST816 系列常见 ID 为 0xB4/B5/B6；不匹配也继续尝试，避免误判整屏不可用 */
    if (id != 0xb6u && id != 0xb5u && id != 0xb4u) {
        log_printf("[stm32] touch: unexpected chip id, continue anyway\r\n");
    }

    {
        uint8_t awake = 1;
        if (HAL_I2C_Mem_Write(&hi2c3, TOUCH_I2C_ADDR, 0xfe, I2C_MEMADD_SIZE_8BIT,
                              &awake, 1, 50) != HAL_OK) {
            return -1;
        }
    }
    touch_ready = 1;
    stat_touch_err = 0;
    return 0;
}

/* 返回 1 表示读到有效触摸信息；down=1 表示当前有单指按下 */
static int touch_poll(int *down, uint16_t *x, uint16_t *y)
{
    uint8_t d[6] = {0};

    if (!touch_ready) return -1;
    if (HAL_I2C_Mem_Read(&hi2c3, TOUCH_I2C_ADDR, 0x01, I2C_MEMADD_SIZE_8BIT,
                         d, sizeof(d), 20) != HAL_OK) {
        return -1;
    }
    /* d = [gesture, finger_num, xh, xl, yh, yl] */
    *down = ((d[1] & 0x0fu) == 1u);
    if (*down) {
        *x = (uint16_t)(((d[2] & 0x0fu) << 8) | d[3]);
        *y = (uint16_t)(((d[4] & 0x0fu) << 8) | d[5]);
    }
    return 0;
}

/* ------------------------------------------------------------ 板间链路发送 */
static int link_send(uint8_t cmd)
{
    HAL_StatusTypeDef st;

    st = HAL_I2C_Master_Transmit(&hi2c1, LINK_I2C_ADDR8, &cmd, 1,
                                 LINK_TX_TIMEOUT_MS);
    if (st == HAL_OK) {
        stat_link_ok++;
        return 0;
    }
    stat_link_fail++;
    if (st == HAL_ERROR && (HAL_I2C_GetError(&hi2c1) & HAL_I2C_ERROR_AF)) {
        /* 从机只是没应答（未上电/未就绪），总线没有被拉死，不必做 9 脉冲 */
        return -1;
    }
    if (st == HAL_ERROR || st == HAL_TIMEOUT) {
        i2c1_bus_recover();
    } else if (HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY) {
        (void)HAL_I2C_DeInit(&hi2c1);
        (void)HAL_I2C_Init(&hi2c1);
    }
    return -1;
}

static int link_send_retry(uint8_t cmd)
{
    for (unsigned i = 0; i < LINK_RETRY; ++i) {
        if (link_send(cmd) == 0) return 0;
        HAL_Delay(5);
    }
    return -1;
}

/* ------------------------------------------------------------------ main */
int main(void)
{
    uint32_t last_poll = 0;
    uint32_t last_touch_init_try = 0;
    uint32_t last_trigger;
    int prev_down = 0;
    uint32_t last_stat = 0;

    HAL_Init();
    SystemClock_Config();
    if (peripherals_init() != 0) {
        for (;;) { HAL_Delay(1000); }
    }
    /* 允许开机立即触摸：把上次触发时间回拨一个去抖窗口 */
    last_trigger = HAL_GetTick() - TOUCH_DEBOUNCE_MS;

    log_printf("\r\n[stm32] touch bridge boot, link addr=0x%02X\r\n", LINK_I2C_ADDR7);

    if (touch_init() != 0) {
        log_printf("[stm32] touch init failed, will retry\r\n");
    } else {
        log_printf("[stm32] touch ready\r\n");
    }

    /* 上电先告诉 ECW 一声（对端可能还没起来，失败无妨） */
    (void)link_send(LINK_EVT_PING);

    /* 开机在墨水屏上显示一次 logo（失败只记日志，不阻塞触摸主循环） */
    if (epd_init() == 0) {
        log_printf("[stm32] epd ready\r\n");
        log_printf("[stm32] epd logo %s\r\n",
                   epd_show_logo() == 0 ? "done" : "FAILED (busy timeout)");
    } else {
        log_printf("[stm32] epd init failed, skip\r\n");
    }

    for (;;) {
        uint32_t now = HAL_GetTick();

        /* 触摸控制器掉线/上电未就绪时周期重试 */
        if (!touch_ready && (now - last_touch_init_try) >= TOUCH_RETRY_MS) {
            last_touch_init_try = now;
            if (touch_init() == 0) {
                log_printf("[stm32] touch recovered\r\n");
            }
        }

        if ((now - last_poll) >= TOUCH_POLL_MS) {
            int down = 0;
            uint16_t x = 0, y = 0;

            last_poll = now;
            if (touch_poll(&down, &x, &y) != 0) {
                stat_touch_err++;
                /* 连续读失败视为掉线，走重初始化 */
                if (stat_touch_err >= 5u) {
                    touch_ready = 0;
                    stat_touch_err = 0;
                }
            } else {
                stat_touch_err = 0;
                if (down && !prev_down && (now - last_trigger) >= TOUCH_DEBOUNCE_MS) {
                    /* 按下沿 + 去抖：每次真实触摸只发一次 */
                    last_trigger = now;
                    stat_touch_events++;
                    if (link_send_retry(LINK_EVT_TOUCH) == 0) {
                        log_printf("[stm32] touch (%u,%u) -> evt sent (#%lu)\r\n",
                                   (unsigned)x, (unsigned)y,
                                   (unsigned long)stat_touch_events);
                    } else {
                        log_printf("[stm32] touch (%u,%u) -> link FAILED\r\n",
                                   (unsigned)x, (unsigned)y);
                    }
                }
            }
            prev_down = down;
        }

        /* 每 10s 打一行状态，便于现场确认链路 */
        if ((now - last_stat) >= 10000u) {
            last_stat = now;
            log_printf("[stm32] stat touch=%lu ready=%d link_ok=%lu link_fail=%lu recover=%lu\r\n",
                       (unsigned long)stat_touch_events, touch_ready,
                       (unsigned long)stat_link_ok,
                       (unsigned long)stat_link_fail,
                       (unsigned long)stat_bus_recover);
        }

        HAL_Delay(5);
    }
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}
