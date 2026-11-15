# 09 · 开机 LOGO（SD → STM32）交接：已验证项、失败尝试与未解决问题

> 状态（2026-11-15）：**开机显示已可用**（内置图 + SSD1677 驱动实机确认）；
> **"从 SD 自动取图覆盖"未打通**，代码保留但默认关闭。本文记录全部尝试、
> 证据和剩余问题，供后续智能体继续。

---

## 1. 目标与最终取舍

原目标：LOGO 转成黑白灰位图存入 SD，STM32 每次开机能从 SD 取回并刷新。

最终落地方案（已实机验证）：

1. `LOGO.png` → 800×480 1bit "黑白灰"（Bayer 抖动）位图；
2. 同一份数据：编进 STM32 flash（开机立即全刷显示），并作为 48032 字节块
   写入 ECW 的 SD（LBA 1536），供将来取图；
3. STM32 开机只显示内置图；`stm32f407/main/main.c` 的
   `LOGO_FETCH_FROM_SD` 默认 **0**，SD 取图路径只保留代码，不参与启动。

原因：ECW6700 的 I2C 从机在"大块 TX"场景会把 SCL 拉死，连触摸/手势链路
一起失效（详见第 4、5 节）。继续调这个路径的投入产出比很低，先保证主链路。

---

## 2. 已确认可用（不要重复踩的坑）

### 2.1 墨水墨驱动：必须是 SSD1677 九位 SPI，不是 UC8176

- 现象：旧实现（`UC8176/400×300 + 独立 D/C=PC13/PC3`）能完成一次 3.7s 刷新，
  但画面内容不对；后续版本甚至 `0x20` 触发后 BUSY 永久为高（20s 超时）。
- 结论：面板是 **SSD1677 系、800×480、1bit、整帧 48000 字节**（48000×8 =
  800×480），与 `firmware-code-stm32-1115` 的窗口参数（X 到 0x031F=799、
  Y 到 479）一致。参考固件"整屏黑白闪烁"就是它在工作。
- 正确写法（已移植到 `stm32f407/main/app_epd.c`）：
  - SPI3 单线发送，8-bit 数据，SCK=PC10 / MOSI=PC12（AF6），CS=PA15，
    RES=PA13，BUSY=PD8（高=忙），**D/C=PC3 固定拉低**；
  - 命令/数据用 9-bit 字：bit8=0 为命令、bit8=1 为数据，每 9 bit 按 MSB-first
    拼进字节流；前缀补 0x7F 字对齐；
  - 初始化：0x12 → wait → 0x18(0x80) → 0x0C(AE C7 C3 C0 80) → 0x01(DF 01 02)
    → 0x11(0x01) → 0x3C(0x01) → 0x21(40 00) → 窗口 0x44(00 00 1F 03)
    / 0x45(DF 01 00 00) / 0x4E(00 00) / 0x4F(DF 01)；
  - 刷屏：0x24 帧 → 窗口 → 0x26 填充 0xFF 48000 字节 → 0x22(0xF7) → 0x20；
    实测 `refresh=3706ms busy=0`，画面正确。
- 相关 SDK 事实：`drv_i2s` 与本问题无关；EPD 参考代码在
  `firmware-code-stm32-1115/main/main.c`。

### 2.2 PNG 转位图的致命细节：透明背景

- 现象：旧 `tools/gen_epd_logo.py` 直接 `Image.open(png).convert("L")`，
  而 `LOGO.png` 是 RGBA、背景透明 → 透明区域变成 **黑色**，整屏发黑，
  只剩紫色圆环变成白色。
- 修复：先按 alpha 合成到白底（`Image.alpha_composite`），再转灰度；
  800×480 等比缩放居中；**8×8 Bayer 有序抖动**近似灰阶。
- 工具：`tools/build_logo_blob.py`（取代 `tools/gen_epd_logo.py`），输出：
  - `ecw6700/main/assets/logo.bin`（SD 资产块，48032 字节）
  - `stm32f407/main/epd_logo.c`（内置同款 48000 字节数组）
- 当前 CRC：`data_crc32 = 0x1198D38E`（800×480）。

### 2.3 SD 资产块写入（已实机验证）

- 布局：资产区 LBA 1024–2047（512 KB）。音效块在偏移 0（约 191 KB），
  LOGO 块固定偏移 **262144（LBA 1536）**，块头 32 字节 + 48000 数据。
- 块头（小端）：
  ```
  0  char[8] "MOCELGO1"   8 u16 version=1    10 u16 header_size=32
  12 u16 width=800        14 u16 height=480  16 u8 bpp=1  17 u8 flags(bit0=1: 1=白)
  18 u16 reserved         20 u32 data_bytes  24 u32 data_crc32  28 u32 reserved
  ```
- 实现：`ecw6700/main/app_assets.c` 的 `logo_install()` /
  `assets_logo_read()`；首次启动日志：
  `[asset] installing logo 48032 bytes to LBA 1536...` →
  `[asset] logo ready: 800x480 1bpp, 48032 bytes (installed)`。

### 2.4 触摸/手势链路（已恢复并实测）

- 上电瞬间（约 0.1s）访问 ECW 会撞上它还在启动的 I2C 引脚，**能把总线
  永久拉死**，此后 STM32 复位也解不开，只能给 ECW 断电。
- 修复：STM32 首次链路 ping 与取图都推迟到启动后 8 秒
  （`main.c` 的 `s_ping_at` / `s_logo_next_try`）。
- 实测（整机断电再上电、复位、单击一次屏幕）：
  `stat touch=8 click=1 up=2 down=2 link_ok=9 link_fail=0 recover=0`。

---

## 3. 未解决问题：ECW I2C 从机长块读取

**一句话**：STM32 作为 I2C 主机先写入读请求，再发起读事务；ECW 作为从机
返回 `[seq, data...]`。头 32 字节（33 字节事务）能成功，**紧接着的第一次
写请求就返回 `HAL_BUSY`（tx=2）**，总线被拉死。

关键诊断证据（STM32 侧，`app_logo.c` 记录）：

```
[stm32] logo fetch start 800x480                 ← 头 33 字节读成功
[stm32] logo chunk error @32 retry 1 (tx=2 rx=1 resp=0xB9)
```

- `tx=2` = `HAL_I2C_Master_Transmit` 返回 `HAL_BUSY`；
- `rx=1` = 上一次 `HAL_I2C_Master_Receive` 返回 `HAL_ERROR`；
- `resp` 是残留数据（0xB9/0x4C/0xFF 都出现过）。
- STM32 侧的 GPIO 总线恢复（9 个 SCL 脉冲 + STOP + 重新 Init）**无效**，
  说明 SCL 被 ECW 从机持续拉低（wired-AND，主机无法拉高）。

---

## 4. 尝试记录（按时间顺序，均已上机）

| # | 方案 | 结果 / 证据 |
|---|---|---|
| v1 | 请求 `[0x20, off_lo, off_hi, len_lo, len_hi]`，触发任务读 SD 后 `drv_i2c_slave_write` 预装，主机 15ms 后读 | 主机读到 0xFF/0x00；随后总线死锁 |
| v2 | 增加 seq 回显（从机回 `[seq, data]`） | 仍失败；读到 0xFF（预装未发生） |
| v3 | 未就绪也填满（seq^0xFF + 0xFF 填充），避免欠载 | 未解决；日志显示请求已解析、预装已发生 |
| v4 | 帧格式加 magic 0x5A + XOR 校验 + 重同步；ECW 打印每次请求 | ECW 打印 `logo req seq=17 off=0 len=32`，**证明请求帧解析正常**；主机仍失败 |
| v5 | `request_tx` 回调里直接 `drv_i2c_slave_write` | 不符合 SDK 官方示例；改为信号量+任务后仍 `tx=2` |
| v6 | 把触发任务优先级 4→6（高于采集 5），确保 40ms 内完成 SD 读+预装 | **头 32 字节首次成功**（`logo fetch start 800x480`），但第一个数据块立刻 `tx=2` |
| v7 | 从机每次预装**整个 320 字节 TX 缓冲**（数据后补 0xFF），主机 NACK/STOP 时仍有数据可发 | 已烧录，但当时总线已被"ECW 启动窗口"碰撞拉死，未得到有效结论；随后按用户意见关闭取图 |

补充现象：

- 失败总是发生在**一次成功的读事务之后的下一次写**；
- 只要发生过一次死锁，STM32 复位无法恢复，**必须给 ECW 断电**；
- 给 STM32 加入 8 秒延时后，正常的开机/触摸链路稳定，不再死锁（取图关闭）。

---

## 5. 根因假设（按可能性排序）

### H1（最可能）：ECW I2C 从机 TX 在事务边界时钟拉伸，主机发不出 STOP

- 驱动：`code/drivers/i2c/i2c_slave.c`。
- `drv_i2c_slave_isr_tx()` 在 FIFO 满时自旋，等待 FIFO 空/STOP/AddrHit；
  TX FIFO 空时控制器会拉低 SCL 等数据（时钟拉伸）。
- 若在主机读最后一个字节时从机 FIFO 恰好为空，控制器进入拉伸；主机此时
  发 NACK+STOP，但 SCL 被从机拉低，STOP 无法完成 → 主机 `HAL_BUSY`，
  且从机持续拉低 SCL。
- v7（全缓冲）正是针对该假设：让从机在事务末尾始终"还有数据"，理论上
  不应再拉伸；**尚未在健康总线上验证**。

### H2：ISR 自旋卡死

- `drv_i2c_slave_isr_tx()` 内层 `while (FIFOFull) { if (Stop|AddrHit) return; }`
  在主机异常中止时可能一直自旋（ISR 上下文），CPU 卡在中断里，SCL 保持低。
- 需读手册确认控制器在 STOP/NAK 后是否会置标志退出该循环。

### H3：驱动状态机在 RX→TX 或 TX→RX 切换时的边界处理

- 驱动在 `I2C_STATE_TX` 收到新 AddrHit 时才重置 `tx_trigger/index`；
  若主机在从机 TX 状态下发写请求，依赖该分支正确切回 RX。未逐行验证。

---

## 6. 代码与协议现状（继续调试所需）

### ECW 侧

- `main/app_link.c`
  - 从机初始化：`drv_i2c_slave_open` + rx_done + request_tx（**回调只 post
    信号量**，与 SDK 示例一致）+ `app-linktx` 任务（优先级 `TRIGGER+2=8`）；
  - `app_link_serve_logo(seq, off, len)`：读 SD → 填 `s_logo_tx`
    （整缓冲 320 字节，0xFF 填充）→ `drv_i2c_slave_write`；
  - `app_link_request_logo()`：读时服务模型的 pending（当前 main 走这条）。
- `main/main.c`
  - `case APP_LINK_CMD_LOGO_READ`：读 7 字节 `[magic 0x5A, seq, off_lo,
    off_hi, len_lo, len_hi, xor]`，校验后 `app_link_request_logo()`；
  - 每次请求可打印 `[link] logo req seq=... off=... len=...`。
- `main/app_config.h`：`APP_LINK_CMD_LOGO_READ 0x20`、
  `APP_LINK_LOGO_CHUNK 200`、`APP_LINK_TX_BUF 320`、
  `APP_TASK_TRIGGER_PRIO 6`。
- `main/app_assets.c`：`logo_install()` / `assets_logo_read()`。
- `main/app_logo_blob.c`：`.incbin "logo.bin"`。

### STM32 侧

- `main/app_epd.c`：SSD1677 九位驱动（第 2.1 节）。
- `main/app_logo.c`：
  - 请求帧 8 字节 `[0x20, 0x5A, seq, off_lo, off_hi, len_lo, len_hi, xor]`；
  - 读 `len+1` 字节，校验 `resp[0]==seq`；每块 5 次重试、40ms 等待；
  - 失败后调用 `logo_bus_recover()`（main.c 的 `i2c1_bus_recover`）；
  - 诊断：`logo_last_tx_status()/rx_status()/resp()`。
- `main/main.c`：
  - `LOGO_FETCH_FROM_SD 0`（默认关闭）；置 1 可重新实验；
  - 状态机分块取图（每次主循环 200 字节），CRC32 校验；
  - 首次 ping/取图延后 8 秒。

### 复现实验步骤

1. `stm32f407/main/main.c`：`#define LOGO_FETCH_FROM_SD 1`；重编 STM32；
2. 重编 ECW（`ecw-build-output/build_firmware.sh`），确保 `logo.bin` 已生成；
3. **先给 ECW 上电**，等 20 秒（SD 资产就绪：`[asset] logo ready`）；
4. 再复位 STM32（BOOT0=0），观察 8 秒后的 `logo fetch start`；
5. 判读：`logo from SD shown crc=1198D38E` 为成功。

**注意**：只要出现 `tx=2`，总线已死，必须给 ECW 断电才能恢复。

---

## 7. 给后续智能体的建议

1. **先读 ECW6700 手册/驱动章节**（`drivers/i2c/i2c_slave.c` 的 TX 状态机、
   FIFO 与 SCL 拉伸行为），确认从机长 TX 的官方用法；SDK 示例只回 16 字节。
2. 做**最小复现**：让 ECW 从机返回固定 16 / 33 / 64 / 201 字节，STM32
   连续读 100 次，统计第几次死锁。判断是否是"长度 > FIFO 深度"才出问题。
3. 若 I2C 长读不可靠，**改用 UART 传图**（与参考固件一致）：ECW UART1
   （IO9/IO10）↔ STM32 PB10/PB11；代价是放弃 STM32 的调试串口或改线。
4. 也可考虑 **ECW 侧周期性自恢复**：无命令 N 毫秒后关闭/重开 I2C 从机，
   让总线从拉伸状态恢复（未验证是否足够）。
5. 不要再用"上电瞬间就访问对端"的测试方式；STM32 首次访问必须 ≥8s，
   否则会把总线锁死，且复位无法恢复。
6. 不要重复的坑：UC8176/400×300 驱动、透明 PNG 直接转灰度、ISR 里直接
   `drv_i2c_slave_write`、`gen_epd_logo.py`（已被 `build_logo_blob.py` 取代）。

---

## 8. 关键日志摘录

```
[asset] installing logo 48032 bytes to LBA 1536...
[asset] logo ready: 800x480 1bpp, 48032 bytes (installed)
[link] logo req seq=17 off=0 len=32          ← ECW 收到并解析请求
[stm32] epd logo done refresh=3706ms busy=0  ← EPD 全刷正常
[stm32] logo fetch start 800x480             ← 头 32 字节读成功（v6）
[stm32] logo chunk error @32 retry 1 (tx=2 rx=1 resp=0xB9)
[stm32] logo fetch unavailable, keep built-in (tx=2 rx=1 resp=0xFF)
[stm32] stat touch=8 click=1 up=2 down=2 link_ok=9 link_fail=0 recover=0
```
