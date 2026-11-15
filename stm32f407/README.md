# MOCE 触摸事件桥（STM32F407VET6）

## 职责（2026-11 迭代后）

本板负责两件事：**检测 2.0TP 触摸屏的手势并通过板间 I²C 通知 ECW6700**，以及
**开机在 800×480 墨水屏上显示一次 LOGO（黑白灰抖动）**。
录音、SD 存储、Wi-Fi 上传、蓝牙都在 ECW 侧。

```
2.0TP (CST816D) --I2C3--> STM32 手势状态机
    --I2C1 主机写 1 字节 0x11..0x14/0x01--> ECW6700 (从机 0x42)
```

- v2 识别单击、双击、上滑、下滑；优先用 CST816 寄存器 0x01 的硬件 GestureID，
  不可用时用按下时长/位移/两次点击间隔做软件兜底；
- 单击延迟 300 ms 发送，以区分双击；长按/横向滑动发 0x01（仅录音）；
- 只发 1 字节命令，不做变长帧/应答，避免 HAL 从机时序问题；
- I²C1 发送失败做 9 个 SCL 脉冲 + STOP 的总线恢复，最多重试 3 次；
- 触摸控制器初始化失败会每 2 秒重试；连续 5 次读失败视为掉线并重初始化；
- USART3 输出调试日志，10 秒打印一次链路统计。

## 接线（expansion-20260915 / schematic-20261005）

| 外设 | 接口 | 引脚 |
|---|---|---|
| 2.0TP 触摸（CST816D） | I2C3 控制器 0x15 | PA8=SCL, PC9=SDA, PD15=RESET, PD10=INT（轮询，不用 INT） |
| 板间链路 | I2C1 主机 100kHz | PB8=SCL, PB9=SDA → ECW6700 IO24/IO23 |
| 调试串口 | USART3 | PB10=TX, PB11=RX, 115200 8N1 |
| EPD 墨水屏 | SPI3 共享总线 | SSD1677 系，800×480 1bit：SCK=PC10, MOSI=PC12（与 LCD 共用）, CS=PA15, RES=PA13, BUSY=PD8（高=忙）；D/C=PC3 固定低（9-bit 字模式） |

## 墨水屏（app_epd.c）

- 驱动：SSD1677 系 COF，**800×480 1bit，整帧 48000 字节**（与已实机点亮的
  `firmware-code-stm32-1115` 一致），SPI3 单线发送；
- 命令/数据用 **9-bit 字**区分：D/C 固定低，bit8=0 命令、bit8=1 数据（MSB-first 拼包）；
- 初始化序列与更新触发照搬参考实现（0x12/0x18/0x0C/0x01/0x11/0x3C/0x21/窗口/0x22=0xF7/0x20），
  全刷实测约 3.7s；
- 开机显示一次 `epd_logo_bits`（48000 字节，1=白）。图由
  `python tools/build_logo_blob.py LOGO.png` 生成：先把 PNG 的 alpha 合成到白底
  （直接转灰度会把透明背景变成黑底，v1 的整屏发黑就是这个问题），等比缩放进
  800×480 居中，再用 8×8 Bayer 抖动成"黑白灰"；
- SD 取图（ECW 的 LBA 1536 存有同款 48032 字节块）已实现但默认关闭
  （`main.c` 的 `LOGO_FETCH_FROM_SD 0`）：ECW 的 I2C 从机长块 TX 在事务边界
  会时钟拉伸、把总线拉死，触摸链路也会一起失效；内置图已满足开机显示；
- BUSY 等待 8s 超时，屏未接/失败只打日志，不阻塞触摸主循环；
- 完整的尝试与未解决问题（SD 取图的 I2C 总线死锁）见
  `docs/09-logo-sd-i2c-handoff.md`；
- 引脚：CS=PA15（SPI_CS0）、SCK=PC10、MOSI=PC12、RES=PA13（SWDIO，SWD 调试期间被占用，
  串口 Bootloader 烧录不受影响）、BUSY=PD8。

STM32 作为 I²C 主机时，HAL 的 `DevAddress` 必须是 8 位形式：`0x42 << 1 = 0x84`。
ECW 侧 `drv_i2c_slave_open` 的 `addr_val` 是未左移的 7 位地址 `0x42`。

## 板间命令

| 命令 | 值 | 含义 |
|---|---|---|
| `LINK_EVT_TOUCH` | 0x01 | 仅录音/上传（长按、横向滑动、未知手势） |
| `LINK_EVT_PING` | 0x02 | 上电心跳（预留） |
| `GESTURE_CLICK` | 0x11 | 单击 → 录音/上传 + 正面音效 |
| `GESTURE_DOUBLE` | 0x12 | 双击 → 录音/上传 + 负面音效 |
| `GESTURE_UP` | 0x13 | 上滑 → 录音/上传 + 正面音效 |
| `GESTURE_DOWN` | 0x14 | 下滑 → 录音/上传 + 负面音效 |

## 构建

```bash
# 在工程根目录执行（Git Bash）
./build_firmware.sh
# 产物：build/firmware.elf / firmware.bin / firmware.hex / firmware.map
```

脚本会：
1. 在 `C:\temp\armgcc` 建立指向工具链根目录的目录联接（工程路径含中文时必需）；
2. 用本地 BSP 的 `integration/arm-gcc.cmake` 配置，构建目录放在 `C:\temp\moce_stm32_build`；
3. 把产物拷回 `build/`。

编译资源：Flash 约 40KB / 512KB，RAM 约 15KB / 128KB。

## 烧录

- UART Bootloader：115200 8E1，BOOT0=1、BOOT1=0，上电后写入 `build/firmware.bin`（0x08000000）；
- 或使用 MOCE Workbench 的 STM32F407 烧录流程。

## 排障

- 串口无输出：确认 USART3 PB10/PB11、115200；触摸初始化会打印 chip id。
- 触摸无反应：看 `stat touch=` 是否增长；若增长但 `link_fail` 也增长，检查 ECW 是否上电、
  是否已进入 I²C 从机模式（ECW 日志 `[link] i2c slave ready`）。
- 上电时 ECW 可能还没起来，`PING` 失败是正常的。

## 与旧版的关系

旧版固件（SD 卡环形录音 + SSD1677 墨水屏 + CST816D + I²C 从机协议）已整体废止。
当前文件只保留触摸与板间链路；旧代码可从 git/备份中找回，但不再参与构建。
