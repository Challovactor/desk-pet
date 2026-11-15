# AGENT.md — 给 AI / 自动化代理的项目工作指南

本文件面向在本仓库中修改、构建、烧录、排障的 AI 代理或自动化脚本。
人类开发者也可把它当作快速上下文。

---

## 1. 项目一句话

**MOCE Desk Pet**：两块板的桌面记忆助手。ECW6700F 负责 PDM 录音、SD 环形存储、
Wi-Fi 上传和喇叭回放；STM32F407 只负责 2.0TP 触摸事件，经板间 I²C 通知 ECW6700。

当前可工作版本的状态、引脚和排障记录：
[docs/06-ecw-bringup-2026-11-15.md](docs/06-ecw-bringup-2026-11-15.md)。

---

## 2. 动手前先读

1. `docs/06-ecw-bringup-2026-11-15.md`（v1 架构、引脚、构建烧录、9 条关键修复）
2. `docs/07-v2-gesture-sound.md`、`docs/08-v2-server-upload.md`（v2 手势/音效/上传）
2.5 `docs/09-logo-sd-i2c-handoff.md`（LOGO/EPD 驱动与 I2C 取图的失败记录，
   继续该方向前必读）
2.6 `docs/10-wifi-boot-sequencing.md`（Wi-Fi 扫描/关联时序与上电初始化顺序，
   改 `app_net.c`/`main.c` 启动流程前必读）
2.7 `docs/11-capture-sd-load-analysis.md`（SD 位翻转写入的 CPU 负载、
   网络任务优先级、"15 秒分段"方案为何不降低负载）
3. `ecw6700/README.md`、`stm32f407/README.md`（子工程职责与配置）
4. `ecw6700/main/app_config.h`（所有运行配置）
5. `ecw6700/main/app_sdspi.c`、`app_net.c`、`app_playback.c`、`app_assets.c`、
   `app_https.c`、`app_upload.c`（最容易改坏的文件）

---

## 3. 项目事实（不要假设）

- **当前主分支**：`main`。当前版本于 2026-11-15 在实物上验证。
- **已验证**：CST816D 触摸、板间 I²C、PDM/I2S 初始化、软件 SPI SD、30 秒片段、
  Wi-Fi 2.4GHz 关联与 IP。
- **已上板**：STM32 手势/板间链路/墨水屏 LOGO（SSD1677 800×480，开机 3.7s 全刷）；ECW 录音/SD/资产/I2S TX
  DMA、Wi-Fi + MQTT/TLS。
- **未验证**：音频内容、SPK 模块出声（SPK pin 1=NC，模块需外部供电）、
  正式上传闭环（在线握手刚修正，待复测）。
- **音频格式**：PDM 采集 32kHz；落盘 PCM16LE **8kHz 单声道**（4 阶 Butterworth
  3.4kHz 低通 + 4:1 抽取）；上传/回放统一 **16kHz**（8→16 线性插值）。
  依据见 `docs/11-capture-sd-load-analysis.md`。
- **服务器协议**：v2 默认 `APP_UPLOAD_FORMAL=1`，走 `desk-recorder/v1`
  （HTTPS 443 + MQTT/TLS 8883，16kHz WAV + SHA256，分钟级）。
  v1 明文 HTTP `/api/record` + `/api/audio` 保留为回退（`APP_UPLOAD_FORMAL=0`）。
- **板间 I²C**：STM32 主机，ECW 从机；7 位地址 0x42（HAL 8 位 0x84）；只传 1 字节命令。
- **Wi-Fi**：这个 SDK 在 `sta_stop` 之后、没有主动扫描时 supplicant 不会自行
  扫描（100 秒零日志），所以**连接前必须扫描一次**（`APP_WIFI_SCAN_DEBUG 1`）；
  关联/握手期间绝不能 `sta_stop/sta_start`（会得到假的 reason=15）。
  启动顺序：先让 Wi-Fi 关联（最多 25 秒），再做 SD 资产校验。
  详见 `docs/10-wifi-boot-sequencing.md`。
- **SD**：裸扇区环形区（从 LBA 2048 起），不是 FATFS；卡上原有数据会被覆盖。
  音效块从 LBA 1024 起，LOGO 块固定在 LBA 1536（48032 字节，`tools/build_logo_blob.py` 生成）。
- **喇叭/音效**：模块接在 STM32 SHIELD（PB12=I2S_WS/PB13=I2S_SCK/PB15=I2S_SD，
  I2S2 主发送），音效数据 embed 在 STM32 flash（`app_sounds_data.c`，
  `aligned(4)`），由 STM32 本地播放；I2S2 时钟必须配 PLLI2S（192/2）。
  ECW 的 U2（IO6/7/8）属于芯片的 I2S **RX 引脚组**，无法发送，不要再去调。
- **2.0" 彩屏**：ST7789 240×320 RGB565，SPI3 共享（CS=PC11/DC=PC13/RST=PA11/BL=PD14），
  开机显示 LOGO.png（RLE 17.7KB）。必须在 EPD 全刷之后再初始化/显示，否则
  EPD 升压泵的电流冲击会让 ST7789 复位、画面消失。
- **墨水屏**：SSD1677 系 800×480 1bit，SPI 9-bit 字（D/C=PC3 固定低），开机全刷
  内置 48000 字节 LOGO 约 3.7s。SD 取图代码在但默认关闭（`LOGO_FETCH_FROM_SD 0`）：
  ECW 的 I2C 从机长块 TX 在事务边界会时钟拉伸并拉死总线，连同触摸链路一起失效。
- **临时热点**：联调时曾用 `moce/12345678`；正式热点在 `app_config.h` 中配置。

---

## 4. 代码地图

### ECW6700F（`ecw6700/main/`）

| 文件 | 职责 | 修改风险 |
|---|---|---|
| `main.c` | 任务编排：采集 / 触发 / 上传 / 网络；上传队列 | 中 |
| `app_config.h` | 全部配置（Wi-Fi、服务器、N 秒、引脚） | 低 |
| `app_audio.c` | I2S PDM 采集 + 三级 64 抽头 + 64:1 抽取 + DC blocker | 高（位序/采样率） |
| `app_sdspi.c` | **GPIO 位翻转软件 SPI** 的 SD 协议实现 | 高（见第 6 节红线） |
| `app_store.c` | 裸扇区环形区、最近 N 秒读取 | 中 |
| `app_link.c` | I²C0 从机 0x42，接收 STM32 命令 | 中 |
| `app_playback.c` | 暂停采集 → I2S TX 16 kHz 回放 → 恢复；ES8311 启动配置 | 中 |
| `app_net.c` | Wi-Fi STA、AP 扫描诊断、lwip socket、v1 HTTP POST（回退） | 中 |
| `app_assets.c` | 音效 blob 校验、首次写入 SD LBA 1024、随机选择/读取 | 中 |
| `app_sound.c` | 手势命令 → 情绪类别；codec 启动任务 | 中 |
| `app_resample.c` | 32k→8k 四阶低通抽取（在 app_audio）+ 8k→16k 插值 | 高（系数/相位） |
| `app_sounds_blob.c` | `.incbin animal_sounds.bin` | 低（但依赖构建脚本复制） |
| `app_https.c` | mbedTLS HTTPS 客户端、流式 PUT | 高（TLS/内存） |
| `app_mqtt.c` | MQTT/TLS presence + commands/events | 高（schema 未确认） |
| `app_upload.c` | desk-recorder/v1 四步上传、WAV/SHA256 | 高（schema 未确认） |
| `app_sha256.c` / `app_json.c` / `app_ca_cert.c` | 哈希/JSON 取值/CA 占位 | 低 |

### STM32F407（`stm32f407/main/`）

| 文件 | 职责 |
|---|---|
| `main.c` | CST816D 触摸轮询、硬件 GestureID + 软件兜底、I²C1 主机写事件、USART3 日志 |
| `syscalls.c` | 最小 newlib 桩，堆上限 `_estack - 8KB` |

### 构建脚本（工作区，不在本仓库）

- `ecw-build-output/build_firmware.sh`：ECW6700 构建（支持 `MOCE_PROJECT` 指向本仓库）
- `stm32f407/build_firmware.sh`：STM32 构建（本仓库内）

---

## 5. 构建 / 烧录 / 串口

### 5.1 一次性前置

```powershell
# ECW：工程路径含中文，SDK make 的 realpath 会失败
New-Item -ItemType Junction -Path 'C:\moce' -Target '<工作区绝对路径>'
```

STM32 构建脚本会自行创建 `C:\temp\armgcc` 工具链联接。

### 5.2 构建

```bash
cd /c/moce
MOCE_PROJECT="/c/moce/desk-pet/ecw6700" bash ecw-build-output/build_firmware.sh
bash desk-pet/stm32f407/build_firmware.sh
```

两个构建都必须成功才算改完。ECW 构建日志在 `ecw-build-work/build.log`。

### 5.3 烧录

```bash
# ECW（开关拨到 ECW6700/ESP32 一侧，COM7 按实际）
C:/moce/MOCEWorkbench-data/tools/ecw6700-windows/flash/eswin_tool.exe \
  flash --device COM7 --allinoneFile C:/moce/ecw-build-output/firmware_allinone.bin
```

STM32：115200 **8E1**，**BOOT0=1 且保持高电平期间复位/上电**，写 `0x08000000`；
烧完 BOOT0=0 复位。可用 `MOCEWorkbench/backend/stm32_uart.py` 做身份校验烧录。

### 5.4 串口日志

- ECW：115200 8N1，日志前缀 `[app]`、`[sd]`、`[net]`、`[link]`、`[audio]`、`[play]`。
- STM32：115200 8N1，日志前缀 `[stm32]`；状态行每 10 秒输出
  `stat touch=... ready=... link_ok=... link_fail=... recover=...`。

---

## 6. 修改红线（最容易改坏的地方）

1. **不要把 `app_sdspi.c` 改回硬件 SPI/DMA。**
   SDK 的 SPI1 CPU 全双工路径在本板实测 RX 晚 1 bit（R7 由 `00 00 01 AA` 变成
   `00 00 00 D5`）；mode 1/2/3 全部 0xFF；DMA 短传输无响应；`RX_TX` 调度会卡死。
   除非有新的硬件/驱动证据，否则保留 GPIO 软件 SPI。
2. **保留构建脚本对 `local_ota_main()` 的注释补丁。**
   SDK 的 local OTA 启动扫描会把 WPA supplicant 卡在 “Pending scan scheduled”，
   之后所有 Wi-Fi 扫描/连接失败。补丁必须幂等（只匹配行首原始调用）。
3. **不要覆盖 `main.c` 和 `app_config.h` 的回放集成。**
   仓库版本比 v1 多 `APP_LINK_CMD_PLAY`、`APP_PLAYBACK_VOLUME_DIV`、
   `#include "app_playback.h"` 和主循环里的播放处理。合并旧代码时只替换
   `app_sdspi.c` / `app_net.c` 这类真正修改过的文件，不要整目录覆盖。
4. **不要删除 `app_playback.c/h`。**
5. **不要提交真实 Wi-Fi 凭据、服务器地址、设备令牌或 CA 私钥。**
   公开仓库的 `app_config.h` 应使用 `YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD` /
   `YOUR_SERVER_IP` 等占位符。
6. **不要提交编译产物**：`.bin/.elf/.hex/.map`、`build/`。ECW 镜像的 `.rodata`
   会包含编译时的 Wi-Fi 凭据。
7. **不要擅自切换音频格式或服务器协议。** 当前是 8kHz 落盘 / 16kHz 上传 / 30 秒；
   正式协议是 16kHz / 分钟级 / MQTT+TLS+HTTPS，且**设备不能自行创建保存任务**
   （只能 App/管理网页创建）。切换前必须先定产品流程和服务端接口。
8. **`HAL_I2C_DeInit()` 会关 I2C 时钟，`HAL_I2C_Init()` 不会重新开。**
   任何总线恢复后必须 `__HAL_RCC_I2Cx_CLK_ENABLE()`。
9. **不要用“每个地址都 ACK”判断 I2C 器件存在。** 这通常是 SDA/SCL 被拉死的总线假象；
   结合正常/交换 SCL/SDA 两个方向、以及示波器/万用表判断。
10. **不要移动音效区/环形区边界。** 音效区 LBA 1024–2047，录音环形区从
    LBA 2048 开始。两者重叠会静默破坏音效或录音。
11. **本板 SPK 是纯 I2S，不是 ES8311 控制口。** U2 GH1.25-5P 的引脚是
    NC/LRCK/BCLK/DIN/GND，pin 1 不供电；`APP_CODEC_CONFIG_ENABLE=0`。
    只有在其它带 ES8311 的硬件上才启用 codec 任务（它必须在 I2S 16 kHz
    MCLK 稳定后、板间 I2C 从机打开前配置）。
12. **不要关闭 TLS 证书校验。** desk-recorder/v1 要求 CA 校验；`app_ca_cert.c`
    是占位符时 HTTPS/MQTT 会直接拒绝连接，这是预期行为。
13. **构建脚本必须继续复制 `main/assets/*.bin`。** `app_sounds_blob.c` 的
    `.incbin` 以 SDK 板级目录为基准；漏掉这一步会导致编译失败。

---

## 7. 修改后的验证清单

改动固件后至少完成：

- [ ] ECW 构建成功（无 `error:`），STM32 构建成功
- [ ] ECW 启动日志出现 `[audio] PDM armed`、`[sd] ready: ... sectors`、
      `[link] i2c slave ready: addr=0x42`
- [ ] STM32 启动日志出现 `touch chip id=0xB6`、`touch ready`
- [ ] 触摸一次，ECW 打印 `clip N queued: ... bytes, ...s`
- [ ] Wi-Fi 日志出现 `sta associated`、`got ip`
- [ ] 若改了上传逻辑：`[net] record event sent` / `clip N uploaded`（v1），
      或 `[up] job=...` / `[up] PUT blob: http 2xx`（v2）
- [ ] 若改了回放：STM32 发 `0x03`，ECW 打印 `[play] clip ...`，播完恢复采集
- [ ] 若改了手势/音效：ECW 打印 `[asset] ready: 4 sounds`、
      `[sound] codec task done, ok=1`、`[sound] gesture 0x11 -> positive class`、
      `[play] sound ...` / `[play] done`
- [ ] STM32 日志：单击/双击/上滑/下滑分别出现 `cmd 0x11/0x12/0x13/0x14`
- [ ] 双击只产生一条 `0x12`；单击延迟约 300 ms

无法上板时，至少说明“仅编译通过/未上板验证”，不要写成已验证。

---

## 8. 常见故障定位

| 现象 | 最可能原因 | 处理 |
|---|---|---|
| `[sd] CMD0 failed ... 0xff` | 卡没插紧/没插卡；引脚映射不对 | 插紧卡；看日志的 `CMD0 ok with ...` 用了哪组映射 |
| SD 首次初始化失败、2 秒后成功 | 卡上电未就绪 | 正常，应用会自动重试 |
| `touch init failed` / 扫描无设备 | 触摸屏损坏、模组未上电、I2C 引脚错 | 先确认 PA11/PD14 已拉高；看 I2C 扫描；换屏验证 |
| `link FAILED` | ECW 未运行/未上电/地址不对 | 看 ECW `i2c slave ready`；确认 0x42 |
| `[net] scan found 0 AP(s)` | 天线/射频/频段问题 | 确认天线、热点 2.4GHz、距离 |
| `[net] no ip for 30s` | 热点频段/密码/服务端 | 固定 2.4GHz；看 `Selected BSS` 日志 |
| `[net] connect <ip>:<port> failed` | 服务器不可达/端口未放行 | 从 PC 和手机分别测 TCP；查安全组/监听地址 |
| 回放无声音/采集不恢复 | I2S TX 引脚、音量、暂停 ACK 超时 | 看 `[play]` 日志；检查 `APP_PLAYBACK_VOLUME_DIV` |
| 启动无日志 | BOOT0 未回 0、串口开关方向、波特率 | 确认 115200 8N1 和开关位置 |

---

## 9. 服务器协议现状（不要误改）

- v2 默认 `APP_UPLOAD_FORMAL=1`：`desk-recorder/v1` 设备主动保存。
  - HTTPS 443：`POST /device/v1/recordings` → `GET /jobs/{id}` →
    `POST /output` → `PUT /output/blob` → `POST /result`。
  - MQTT 3.1.1+TLS 8883：`commands/events` QoS1、`telemetry` QoS0、
    `presence` QoS1 retained；90 秒无有效心跳视为离线。
  - 音频固定 16 kHz/单声道/16-bit WAV，上限 115,200,044 字节。
- v1 明文 HTTP（`/api/record` + `/api/audio`）保留，`APP_UPLOAD_FORMAL=0` 回退。
- **未决**：设备令牌、MQTT 账号、CA PEM，以及 presence/telemetry/命令/
  output/result 的完整 JSON schema。API.md 只给了语义，没给字段名；
  改 schema 时集中在 `app_mqtt.c` 的 publish/parse 和 `app_upload.c` 的
  `build_output_json()`/`build_result_json()`。
- 设备不能自行创建“任意已有文件上传”任务；`POST /recordings` 保存的是
  命令执行时最近 N 分钟。触摸只是触发一次设备来源的保存申请。

---

## 10. 提交规则

- 提交前：`git status` 确认没有真实凭据/二进制；`git diff` 只包含预期文件。
- 提交信息用英文短句，例如：
  - `ecw6700: fix ...`
  - `stm32f407: ...`
  - `docs: ...`
- 推送目标：`git@github.com:Challovactor/desk-pet.git` 的 `main`
  （需要仓库写权限；本机 SSH 身份为 `Liulj22`）。
- 不要把 `firmware-code-*` 工作副本、`archive/`、SDK、`ecw-build-work/` 提交进仓库。

---

## 11. 当前未完成项

1. 音频内容验证（导出/回放 WAV，确认不是静音/噪声）。
2. v2 手势/音效上板验证：CST816D GestureID、ES8311 I²C0/0x18、MCLK/WS/SCK、
   功放使能、半双工数据线（见 `docs/07` 第 8 节）。
3. v2 正式上传联调：设备令牌、MQTT 账号、CA PEM、presence/telemetry/
   output/result schema、服务器 443/8883 可达性（见 `docs/08` 第 7 节）。
4. `audio.configure` 任务：已把 `minutes` 更新到 RAM，但未回 `/result`，
   重启后恢复默认 `APP_RETENTION_MINUTES`。
5. 上传任务持久化：设备重启会丢失未完成的保存请求。
6. 内存/堆评估：MQTT/TLS + HTTPS/TLS 同时连接时的 mbedTLS 缓冲区。
7. 凭据轮换与公开仓库历史中的 Wi-Fi 密码清理。
8. 音效资产变更后重新生成 `animal_sounds.bin` 并提交。
9. 开机从 SD 取 LOGO 的 I2C 长块读取：头 32 字节能读，第一个数据块后
   总线死锁（`tx=HAL_BUSY`）。代码保留、默认关闭；尝试记录与排查建议见
   `docs/09-logo-sd-i2c-handoff.md`。
