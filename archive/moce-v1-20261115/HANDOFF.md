# MOCE「记忆时刻」设备 — v1 可工作版本交接文档

- 存档日期：2026-11-15
- 存档目的：在继续开发新功能前，保留一套**已在实物上跑通主链路**的代码、镜像和排障结论，便于回溯。
- 当前状态：触摸 → 录音 → SD 环形存储 → Wi-Fi → HTTP 上传 的软件链路已全部打通；**唯一未闭环项是服务器 `YOUR_SERVER_IP:8080` 当时不可达**（从 PC 测试 80/8080 均超时）。

---

## 1. 硬件拓扑

```
2.0TP 触摸屏 ──(2.0TP 12P 排线)── STM32F407_SHIELD1 ── STM32F407VET6 主板
                                                          │
                                              板间 I²C（PB8/PB9 ↔ ECW IO24/IO23）
                                                          │
ECW6700F 主板（YL6700B）: MIC 接口 + 背面 SPI SD 卡槽 + Wi-Fi 天线
```

### 1.1 已确认的 STM32 侧引脚

| 信号 | 引脚 | 说明 |
|---|---|---|
| 触摸 I2C_SCL | PA8 | I2C3，7 位地址 0x15（HAL 8 位 0x2A） |
| 触摸 I2C_SDA | PC9 | I2C3 |
| T_RES（触摸复位） | PD15 | 低有效，低 10ms → 高 200ms |
| T_INT（触摸中断） | PD10 | 输入上拉，当前轮询不使用 |
| BLK（背光/模组使能） | PD14 | 高有效，启动时拉高 |
| S_RES（LCD 复位） | PA11 | 高有效 |
| LCD CS | PC11 | 输出高 |
| LCD DC/WR | PC13 | 输出高 |
| LCD SPI_SCK | PC10 | AF6 SPI3（当前只上电，不驱动画面） |
| LCD SPI_MOSI | PC12 | AF6 SPI3 |
| 板间 I²C | PB8/PB9 | I2C1 主机，目标 7 位 0x42（HAL 8 位 0x84） |
| 调试串口 | PB10/PB11 | USART3，115200 8N1 |

### 1.2 已确认的 ECW6700 侧引脚

| 信号 | IO | 说明 |
|---|---|---|
| MIC WS | IO6 | I2S RXWS |
| MIC SCK | IO7 | I2S RXSCK（PDM 时钟，约 2.048MHz） |
| MIC SD | IO8 | I2S RXD（PDM 数据） |
| MIC 第 4 脚 | IO3 | 连接器未接信号，配置为 MCLK |
| SD CS | IO11 | GPIO 软件片选 |
| SD SCK | IO14 | 软件 SPI |
| SD MISO | IO12 | 软件 SPI |
| SD MOSI | IO13 | 软件 SPI |
| 板间 I2C SCL | IO24 | I2C0 从机，7 位地址 0x42 |
| 板间 I2C SDA | IO23 | I2C0 从机 |

> 注意：SD 的四根线是**软件 SPI**（GPIO 位翻转），不是硬件 SPI 外设。硬件 SPI1 在本板实测 CPU 全双工 RX 晚 1 bit、DMA 短传输无响应，故弃用。

---

## 2. 软件架构

### 2.1 ECW6700F（`ecw6700-memory-node/`）

| 文件 | 职责 |
|---|---|
| `main.c` | 任务编排：采集 / 触发 / 上传 / 网络；上传队列深度 4 |
| `app_config.h` | Wi-Fi、服务器、N 秒、全部引脚宏 |
| `app_audio.c` | I2S PDM 采集；三级 64 抽头滑动和 + 64:1 抽取 + DC blocker；32-bit slot 按 MSB-first 解码 |
| `app_sdspi.c` | 软件 SPI 模式 SD：CMD0/8/55/41/58/16/9/17/24 + CRC7/CRC16；上电自动尝试 4 种引脚映射 |
| `app_store.c` | 从 LBA 2048 起的裸扇区环形区（上限 512MB），4KB 写、跨环读取 |
| `app_link.c` | I2C0 从机 0x42；中断回调只置标志，任务上下文读命令 |
| `app_net.c` | Wi-Fi STA（接管 SDK 自动连接）+ AP 扫描诊断 + lwip socket + HTTP POST |

**数据流**

```
PDM 麦克风 → I2S → PCM16LE 32kHz 单声道 → SD 环形区（持续 64KB/s）
触摸（I2C 0x42 收到 0x01）→ 快照最近 30 秒 → 入上传队列
上传任务：POST /api/record（JSON）→ POST /api/audio（WAV 流式）
```

**板间命令**：STM32 主机写 1 字节 `0x01` = 触摸；`0x02` = 上电 PING（预留）。

### 2.2 STM32F407（`stm32-touch-bridge/`）

- `main.c`：CST816D 触摸轮询（I2C3）、按下沿 + 800ms 去抖、I2C1 主机写 `0x01` 给 ECW、NACK 不触发总线恢复、失败重试 3 次。
- 启动时先给 2.0TP 模组上电/释放复位（PA11/PC11/PC13/PD14），再复位触摸（PD15）。
- 触摸初始化失败会自动扫描 I2C3 并打印所有应答地址；状态行每 10 秒输出 `stat touch=... ready=... link_ok=...`。
- `syscalls.c`：最小 newlib 桩，堆上限 `_estack - 8KB`，给栈留足空间。

### 2.3 服务器 HTTP 契约（当前实现，可替换）

1) 记录事件：

```http
POST /api/record HTTP/1.1
Content-Type: application/json
X-Device-Id: moce-01
X-Clip-Id: 1

{"device":"moce-01","event":"record","clip_id":1,"seconds":30,
 "bytes":1920000,"sample_rate":32000,"format":"pcm_s16le"}
```

2) 音频：

```http
POST /api/audio HTTP/1.1
Content-Type: audio/wav
Content-Length: <44 + bytes>
X-Device-Id: moce-01
X-Clip-Id: 1
X-Clip-Seconds: 30

<WAV: 44 字节头 + PCM16LE 单声道 32kHz>
```

只有 HTTP 2xx 才算成功；失败重试 2 次。`X-Clip-Id` 可用于服务端幂等去重。

---

## 3. 实机验证结果

| 项目 | 状态 | 证据/说明 |
|---|---|---|
| CST816D 触摸 | ✅ | `touch chip id=0xB6`、`ready=1`、坐标正常 |
| STM32 → ECW I²C 0x42 | ✅ | ECW `clip N queued` |
| PDM/I2S 初始化 | ✅ | `[audio] PDM armed: 32000 Hz, 8192 B/read -> 1024 samples` |
| **音频内容正确性** | ⚠️ 未验证 | 只验证了初始化与写卡；未回放 WAV 确认不是静音/噪声 |
| SD 卡（软件 SPI） | ✅ | `storage ready, 524288 KB ring`；`total` 持续增长 |
| 30 秒回溯片段 | ✅ | 连续运行 >30s 后触摸得到 `1920000 bytes, 30s` |
| Wi-Fi STA | ✅ | `scan found 11 AP(s)`、`sta associated`、`got ip` |
| HTTP 上传 | ⏳ 服务器不可达 | `[net] connect YOUR_SERVER_IP:8080 failed`；PC 测试 80/8080 均超时 |
| 墨水屏 / 蓝牙 | ➖ 已移除 | 本次需求不做 |

---

## 4. 构建与烧录

### 4.1 ECW6700F

**前置**：工程路径含中文，SDK make 的 `realpath` 会失败。需要 ASCII 目录联接：

```powershell
New-Item -ItemType Junction -Path 'C:\moce' -Target 'E:\博一 资料\黑客松\逆向开发'
```

**构建**（在 `C:\moce` 下执行）：

```bash
cd /c/moce
bash ecw-build-output/build_firmware.sh
# 产物：ecw-build-output/firmware_allinone.bin / firmware_cpu.bin / firmware.elf / firmware.map
```

构建脚本会：
1. 复用/解包固定版本 SDK（B02P12）；
2. 应用 Windows makefile 补丁；
3. 在 SDK 启动文件中插入 `moce_app_main()`，并**注释掉 `local_ota_main()`**（关键，见第 5 节）；
4. 把 `main/*.c` + `board_support` 接入 `Boards/ecw6700f/standalone`；
5. 用 ESWIN GCC 14.1.1 构建并导出镜像。

**烧录**（板上开关拨到 `ECW6700/ESP32` 一侧）：

```bash
cd C:/moce/MOCEWorkbench-data/moce-cache-ca0b65ff70a4/flash-jobs
C:/moce/MOCEWorkbench-data/tools/ecw6700-windows/flash/eswin_tool.exe \
  flash --device COM7 --allinoneFile C:/moce/ecw-build-output/firmware_allinone.bin
```

ECW 只有 RST 键，没有 BOOT 键；`eswin_tool` 通过 CH340 的 RTS 脉冲进入 ROM 下载模式。烧录完成后 `chip start_up` 自动重启，串口 115200 8N1 看日志。

### 4.2 STM32F407

**构建**（Git Bash）：

```bash
cd /c/moce
bash firmware-code-5a2b43165fb8-stm32f407/build_firmware.sh
# 产物：firmware-code-.../build/firmware.bin/.hex/.elf/.map
```

脚本会在 `C:\temp\armgcc` 建立指向工具链根目录的目录联接（GCC 用相对路径找 cc1/libgcc，必须联接根目录而不是 bin），构建目录放在 `C:\temp\moce_stm32_build`。

**进入 Bootloader（关键）**：
- BOOT0 = 1、BOOT1 = 0；
- **在 BOOT0 保持高电平期间**按复位或断电重上电（常见坑：按一下 BOOT0 就松手再复位，芯片仍从 Flash 启动）；
- 串口 115200 **8E1**（偶校验），写入 `firmware.bin` 到 `0x08000000`；
- 烧完 BOOT0 = 0，复位运行。

可用 MOCE Workbench 的 `backend/stm32_uart.py` 做带身份校验的烧录（芯片 ID 0x413、512KB、UID 校验、逐字节回读）。

### 4.3 串口开关

板上拨动开关只切换 CH340 接到哪块板的 UART：
- `ECW6700/ESP32` 一侧：COM7 是 ECW 日志（115200 8N1）；
- `STM32` 一侧：COM7 是 STM32 USART3 日志（115200 8N1）。
板间 I²C 不受该开关影响。

---

## 5. 关键修复与踩坑记录（后续调试必看）

1. **SDK SPI1 的 CPU 全双工 RX 晚 1 bit**
   - 现象：CMD0 返回 0x00（应为 0x01），CMD8 的 R7 从期望 `00 00 01 AA` 变成 `00 00 00 D5`（整段右移 1 bit）。
   - 尝试：mode 0 有响应但偏移；mode 1/2/3 全部 0xFF；DMA 全双工对短传输 0xFF；`RX_TX` 调度会卡死。
   - 解决：改用 **GPIO 位翻转软件 SPI**（`app_sdspi.c` 的 `bb_byte`），时序完全可控。当前速率约 0.5–1MHz，64KB/s 录音够用；1.9MB 上传约数秒。

2. **SDK local OTA 把 WPA supplicant 卡死**
   - 现象：`[wpa-W] Pending scan scheduled, reject new request`、`[wifi_ctrl-E] can not scan fac wifi`，之后所有扫描/连接失败。
   - 原因：`CONFIG_OTA_LOCAL` 的 `local_ota_main()` 在启动时扫描 `local_ota_test`。
   - 解决：构建脚本在启动文件里注释掉 `local_ota_main()`（幂等替换）。关闭后 Wi-Fi 正常。

3. **SD 引脚顺序**
   - 板级表只给了 `SPI_0={14,12,13}` + `GPIO_4=11`，没有信号级映射。
   - 实测可用映射：**CS=IO11、SCK=IO14、MOSI=IO13、MISO=IO12**。
   - `app_sdspi.c` 仍保留 4 种映射自动尝试，日志会打印哪种命中。

4. **SD 卡没插紧 / 触摸屏损坏**
   - 两者都表现为“无 ACK/全 0xFF”。更换触摸屏、插紧 SD 后恢复。
   - 诊断经验：I²C 扫描“每个地址都 ACK”通常是 SDA/SCL 被拉死的总线假象，不是真的有 128 个器件。

5. **Wi-Fi 频段被热点切换**
   - 手机热点接入新设备后自动切到 5GHz，ECW 断连；调回 2.4GHz 后恢复。
   - 建议固定热点为 2.4GHz，或确认 ECW 支持的信道。

6. **`HAL_I2C_DeInit()` 会关 I2C 时钟，`HAL_I2C_Init()` 不会重新开**
   - 总线恢复后必须 `__HAL_RCC_I2Cx_CLK_ENABLE()` 再 `HAL_I2C_Init()`，否则扫描会卡住。

7. **STM32 进入 Bootloader 的姿势**
   - 必须“BOOT0 高电平期间复位/上电”；按一下 BOOT0 再复位是无效的。

8. **2.0TP 模组上电**
   - 只复位触摸 PD15 不够；需要先把 PA11（LCD 复位）拉高、PD14（背光/使能）拉高，模组才完全上电。

9. **构建脚本幂等性**
   - 启动文件补丁必须只匹配“行首空白 + 原始调用”的行，否则会把已注释的行再次替换成嵌套注释（`/* /* ... */ ... */`），编译报 `"/*" within comment`。

---

## 6. 配置项（`ecw6700-memory-node/main/app_config.h`）

| 宏 | 当前值 | 说明 |
|---|---|---|
| `APP_WIFI_SSID` | `YOUR_WIFI_SSID` | 正式热点（联调时曾临时改为 `moce`） |
| `APP_WIFI_PASSWORD` | `YOUR_WIFI_PASSWORD` | |
| `APP_SERVER_HOST` | `YOUR_SERVER_IP` | 建议用公网 IP，避免 lwip DNS 阻塞 |
| `APP_SERVER_PORT` | `8080` | |
| `APP_SERVER_EVENT_PATH` | `/api/record` | |
| `APP_SERVER_AUDIO_PATH` | `/api/audio` | |
| `APP_CLIP_SECONDS` | `30` | 回溯时长 |
| `APP_PDM_SAMPLE_RATE` | `32000` | I2S 配置采样率；PDM 时钟 ≈ 2.048MHz |
| `APP_PDM_READ_BYTES` | `8192` | 每次 1024 个 PCM 样本（32ms） |
| `APP_SD_START_LBA` | `2048` | 裸扇区环起点（覆盖卡上 LBA 2048 之后的数据） |
| `APP_SD_CHUNK_BYTES` | `4096` | 环形写块大小 |
| `APP_UPLOAD_QUEUE_LEN` | `4` | 触摸上传队列深度 |

> 烧录前务必确认 `app_config.h` 与目标环境一致；改完需重新编译烧录。

---

## 7. 后续开发指引

### 7.1 建议优先补的验证

1. **音频内容**：把 SD 环形区导出一段 PCM 或让服务器保存 WAV，确认不是静音/噪声；若异常，优先检查 PDM 时钟（IO7）、数据（IO8）、WS/SCK 是否反。
2. **服务器可达性**：确认 YOUR_SERVER_IP 是公网 IP、8080 在安全组放行、服务监听 `0.0.0.0:8080`；手机浏览器先用同一热点访问。
3. **SD 吞吐**：软件 SPI 当前约 0.5–1MHz；若后续提高采样率/双声道，需要评估总线占用，或回头解决硬件 SPI 的 1-bit 采样问题（可向 ESWIN 确认 `SPI_TRAMODE`/mode 编码）。

### 7.2 代码扩展点

- **新触摸手势**：`app_link.c` 现在只透传 1 字节命令；可在 `APP_LINK_CMD_*` 增加枚举。
- **新上传协议**：全部隔离在 `app_net.c`；换成 MQTT/WebSocket/HTTPS 只改这一层。
- **片段元数据/多片段索引**：`app_store.c` 目前只有 `total` 与环偏移；如需列表/重传，增加片段表和持久化。
- **RTC/墙钟时间**：当前 `clip_id` 是自增计数，没有真实时间；需要 RTC/SNTP 时在 `app_net.c` 或新增模块。
- **显示**：STM32 侧已配好 ST7789 的 SPI/控制脚但未驱动画面；可在 `stm32-touch-bridge/main/main.c` 增加显示任务。
- **上传队列**：深度 4，连续快速触摸会丢；可按产品语义增大或改为“只保留最后一次”。

### 7.3 已知限制

- SD 使用裸扇区（非 FATFS），PC 不能直接读取；LBA 2048 之后的数据会被覆盖。
- 软件 SPI 速率有限，长时间连续上传会占用较多 CPU。
- Wi-Fi 断线由 SDK 自动重连；应用每 30 秒无 IP 会 stop/start STA。频繁重启可能让 supplicant 卡住，建议后续改为更温和的重连或加状态机。
- 没有看门狗；应用任务若卡死只能断电。
- HTTP 上传没有断点续传/服务端幂等（仅靠 `X-Clip-Id`）。

---

## 8. 存档文件清单

```
archive/moce-v1-20261115/
├─ HANDOFF.md                       ← 本文档
├─ SHA256SUMS.txt                   ← 全文件校验和
├─ stm32-touch-bridge/              ← STM32 源码 + build_firmware.sh + build/ 产物
├─ ecw6700-memory-node/             ← ECW 源码（main/*.c/h + board_support）
├─ ecw-build-output/                ← ECW 可复现构建脚本 + firmware_allinone.bin 等产物
├─ ecw-reverse-engineering/         ← 厂商 SDK/烧录链路的逆向记录与脚本
└─ hardware/                        ← 2.0TP/SHIELD/MIC/SD 引脚图（数据手册未随公开仓库分发）
```

> 公开仓库不包含编译产物（含真实 Wi-Fi 凭据的 ECW 镜像）；请用 `build_firmware.sh` 本地重新构建。以下哈希仅用于对照本地构建结果。

### 本次存档镜像

| 镜像 | 大小 | SHA-256 |
|---|---|---|
| `ecw-build-output/firmware_allinone.bin` | 1,564,215 B | `59afa9f4c8c862f688cf704381fc90d0953b521e7b9546d52a2790aad01dbe40` |
| `stm32-touch-bridge/build/firmware.bin` | 42,244 B | `64205b289c0268f54dd6ebd536e4460536bd8aede1765417ff041dc0878ee2dc` |

> 注：ECW 镜像是用**正式热点** `YOUR_WIFI_SSID` 构建的；板上当前运行的可能是临时热点 `moce` 的旧镜像，如需一致请重新烧录本存档镜像。
