# 01 · 接线与硬件

> 2026-11 迭代后：录音、SD 存储、上传、回放全部在 ECW6700 侧闭环；
> STM32F407 只做 2.0TP 触摸输入。本文按当前固件的实际接线编写。

## 0. 总线分工

| 通道 | 接法 | 用途 | 带宽 |
|---|---|---|---|
| 板间 I²C | F407 I2C1（PB8/PB9，主机）↔ ECW6700 I2C0（IO24/IO23，从机 0x42） | 1 字节命令：0x01 触摸 / 0x02 PING / 0x03 回放 / 0x11–0x14 手势 | 100 kHz，足够 |
| F407 调试串口 | USART3（PB10=TX / PB11=RX） | F407 本板日志，115200 8N1，不跨板 | — |
| MIC 连接器 | ECW6700 BOARD_I2S_0（WS=IO6 / SCK=IO7 / SD=IO8，5 线：3V3/WS/SCK/SD/GND） | PDM 数字麦克风，32 kHz；第 4 脚 IO3 保留 MCLK | — |
| SPK-OUT | U2 WAFER-GH1.25-5PWB：1=NC、2=LRCK、3=BCLK、4=DIN、5=GND（屏蔽 6/7=GND） | **纯 I2S 输出，不含供电/ES8311 I2C**；喇叭模块必须另接 3.3V/5V。回放时暂停采集 | — |
| SD 卡 | ECW6700 IO14(SCK) / IO12(MISO) / IO13(MOSI) / IO11(CS) | **软件 SPI**（GPIO 位翻转）SPI 模式裸扇区环形缓冲，从 LBA 2048 起 | 约 0.5–1 MHz |
| TTS | ECW6700 UART（模组自有引脚） | 文本转语音模块，保留接法，固件暂未驱动 | — |
| 2.0TP | F407 I2C3（PA8=SCL / PC9=SDA，RESET=PD15，INT=PD10 轮询不用） | CST816D 触摸屏，7 位地址 0x15；模组 LCD 引脚见 §1，当前只上电不驱动 | — |
| EPD | F407：SCK=PC10 / MOSI=PC12（SPI3，与 LCD 共用）/ CS=PA15 / D/C=PC3（固定低）/ RES=PA13 / BUSY=PD8 | SSD1677 系 800×480 1bit，9-bit 字 SPI；开机显示一次 48000 字节 LOGO（见 `stm32f407/README.md`） | — |

**为什么音频不过板：** 录音（PDM 采集）、存储（SD 环形区）、上传（Wi-Fi HTTP）、
回放（喇叭）全部在 ECW6700 侧完成，板间 I²C 只传 1 字节命令。
旧方案中「F407 采音 + 经 UART（PB10/PB11）向 ECW 批量搬音频」的链路已整体废止——
PB10/PB11 现在是 F407 的调试串口，不再承载音频。

## 1. STM32F407 引脚（expansion-20260915 / schematic-20261005）

| 功能 | 引脚 | 外设/复用 | 备注 |
|---|---|---|---|
| 触摸 SCL | PA8 | I2C3 | CST816D，7 位地址 0x15 |
| 触摸 SDA | PC9 | I2C3 | 寄存器 0x01 读 6 字节 |
| 触摸 RESET | PD15 | GPIO 输出 | |
| 触摸 INT | PD10 | 输入上拉 | 轮询，不用中断 |
| 模组背光 BLK | PD14 | GPIO 输出 | 高有效，启动时拉高 |
| LCD 复位 S_RES | PA11 | GPIO 输出 | 高有效 |
| LCD CS / DC | PC11 / PC13 | GPIO 输出 | 输出高 |
| LCD SPI SCK / MOSI | PC10 / PC12 | SPI3（AF6） | 2.0TP 模组 LCD，当前只上电不驱动画面 |
| 板间 SCL | PB8 | I2C1 主机，100 kHz | 接 ECW6700 IO24 |
| 板间 SDA | PB9 | I2C1 主机 | 接 ECW6700 IO23 |
| 调试串口 TX | PB10 | USART3_TX | 115200 8N1 |
| 调试串口 RX | PB11 | USART3_RX | |
| 4.2EPD BUSY | PD8 | GPIO 输入 | 高=忙 |
| 4.2EPD RES | PA13 | GPIO 输出 | 注意：SWDIO 复用脚，SWD 调试期间被占用 |
| EPD D/C | PC3 | GPIO 输出 | 9-bit 字模式固定拉低（命令/数据靠第 9 位区分） |
| 4.2EPD CS | PA15 | GPIO 输出 | SPI_CS0；LCD 用 SPI_CS1=PC11 |
| 4.2EPD SCK/MOSI | PC10 / PC12 | SPI3（AF6） | 与 2.0TP 模组 LCD 共享总线 |

注意：F407 作为 I²C 主机时，HAL 的 `DevAddress` 必须写 8 位形式 `0x42 << 1 = 0x84`；
ECW 侧 `drv_i2c_slave_open` 用未左移的 7 位地址 `0x42`。

## 2. ECW6700 侧

| 信号 | 接到 | 说明 |
|---|---|---|
| I2C SCL / SDA | IO24 / IO23 | 从机，地址 0x42（7 位），接 F407 PB8 / PB9 |
| 麦克风 | MIC 连接器（BOARD_I2S_0） | PDM 数字麦：时钟 SCK=IO7，数据 SD=IO8，WS=IO6（主机驱动、麦克风不用） |
| 喇叭 | 独立 SPK-OUT（U2 GH1.25-5P） | 纯 I2S：1=NC / 2=LRCK / 3=BCLK / 4=DIN / 5=GND；模块需外部 3.3V/5V 供电。回放时暂停采集 |
| SD 卡 | 背面 SD 卡槽：IO14/IO12/IO13 + CS=IO11 | 软件 SPI（GPIO 位翻转），CMD17/24，512B 扇区 |
| TTS 模块 | 模组自有 UART | 保留接法，发文本即播报；固件暂未驱动 |

**共地是必须的**，两块板各自的 GND 必须连一起，否则 I²C 可能偶发丢包
（F407 侧发送失败会做 9 个 SCL 脉冲 + STOP 的总线恢复后重试）。

## 3. 电源

- 麦克风模块按其标称供电；SD、触摸屏均 3.3 V。
- SD 卡供电注意：若模块自带 AMS1117 且输入 5 V，则不要再同时接板子 3.3 V，二选一。
- SD 卡上电顺序：先给 3.3 V，再拉低 CS，发 74 个以上时钟完成初始化。

## 4. SD 卡

- 用 **SPI 模式挂载裸扇区**，不建 FAT（长期循环写 FAT 会被元数据更新拖慢并放大写放大）；
  固件从 LBA 2048 起把整卡当环形缓冲区，保留前 1 MB 不碰。
- 建议 8 GB 以上、Class 10 / U1 的卡；杂牌卡在 SPI 模式初始化经常失败。
- 用于录音的卡不要再存放其他数据（裸扇区写入会覆盖原文件系统区域）。

### 4.1 v2 音效区

- LBA **1024–2047**（512 KB）保留给 4 个情绪音效；首次启动由固件从
  `.rodata` 写入，之后启动只校验 CRC。
- 录音环形区仍从 LBA **2048** 开始，两者永不重叠。
- 音效是 16 kHz/16-bit/单声道 PCM，每段从 512 字节边界开始；
  blob 格式见 [07-v2-gesture-sound.md](07-v2-gesture-sound.md) 第 5 节。
- 更换音效后必须重新运行 `ecw6700/tools/build_sound_blob.py` 并重新烧录；
  固件会检测 CRC 变化并覆盖旧资产。

## 5. 接线检查清单（上电前逐条打勾）

- [ ] 两块板 GND 连通（万用表蜂鸣档量过）
- [ ] 板间 I²C：F407 PB8/PB9 ↔ ECW IO24/IO23，交叉不接错
- [ ] SD 卡 3.3 V 供电且 CS 空闲时为高
- [ ] MIC 连接器插到位（PDM 麦），喇叭接独立 SPK-OUT 口
- [ ] F407 烧录后串口 PB10 有启动日志（会打印触摸 chip id）
- [ ] ECW 启动日志出现 `[link] i2c slave ready: addr=0x42`
