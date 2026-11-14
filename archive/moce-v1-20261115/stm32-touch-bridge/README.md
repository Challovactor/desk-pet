# MOCE 触摸事件桥（STM32F407VET6）

## 职责（2026-11 迭代后）

本板只做一件事：**检测 2.0TP 触摸屏的按下，并通过板间 I²C 通知 ECW6700**。
录音、SD 存储、Wi-Fi 上传、墨水屏/蓝牙都已在本次需求变更中移除。

```
2.0TP (CST816D) --I2C3--> STM32 --I2C1 主机写 1 字节 0x01--> ECW6700 (从机 0x42)
```

- 触摸按下沿触发，800ms 去抖，整屏任意位置都算一次"记录"；
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

STM32 作为 I²C 主机时，HAL 的 `DevAddress` 必须是 8 位形式：`0x42 << 1 = 0x84`。
ECW 侧 `drv_i2c_slave_open` 的 `addr_val` 是未左移的 7 位地址 `0x42`。

## 板间命令

| 命令 | 值 | 含义 |
|---|---|---|
| `LINK_EVT_TOUCH` | 0x01 | 用户触摸，ECW 取最近 N 秒并上传 |
| `LINK_EVT_PING` | 0x02 | 上电心跳（预留） |

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
