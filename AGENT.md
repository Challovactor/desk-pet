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

1. `docs/06-ecw-bringup-2026-11-15.md`（当前架构、引脚、构建烧录、9 条关键修复）
2. `ecw6700/README.md`、`stm32f407/README.md`（子工程职责与配置）
3. `ecw6700/main/app_config.h`（所有运行配置）
4. `ecw6700/main/app_sdspi.c`、`app_net.c`、`app_playback.c`（三个最容易改坏的文件）

---

## 3. 项目事实（不要假设）

- **当前主分支**：`main`。当前版本于 2026-11-15 在实物上验证。
- **已验证**：CST816D 触摸、板间 I²C、PDM/I2S 初始化、软件 SPI SD、30 秒片段、
  Wi-Fi 2.4GHz 关联与 IP。
- **未验证**：音频实际内容（PCM 是否静音/噪声）、HTTP 上传（服务器当时不可达）、
  喇叭回放（仅编译通过）。
- **音频格式**：当前 PCM16LE、**32kHz 单声道**、回溯 **30 秒**。
- **服务器协议**：当前是明文 HTTP `POST /api/record` + `POST /api/audio`。
  正式 `desk-recorder/v1`（MQTT+TLS/HTTPS、16kHz、分钟级、App 发起任务）**未接入**。
- **板间 I²C**：STM32 主机，ECW 从机；7 位地址 0x42（HAL 8 位 0x84）；只传 1 字节命令。
- **SD**：裸扇区环形区（从 LBA 2048 起），不是 FATFS；卡上原有数据会被覆盖。
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
| `app_playback.c` | 暂停采集 → I2S TX 回放 → 恢复采集 | 中 |
| `app_net.c` | Wi-Fi STA、AP 扫描诊断、lwip socket、HTTP POST | 中 |

### STM32F407（`stm32f407/main/`）

| 文件 | 职责 |
|---|---|
| `main.c` | CST816D 触摸轮询、去抖、I²C1 主机写事件、USART3 日志 |
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
7. **不要擅自切换音频格式或服务器协议。** 当前是 32kHz / 30 秒 / 明文 HTTP；
   正式协议是 16kHz / 分钟级 / MQTT+TLS+HTTPS，且**设备不能自行创建保存任务**
   （只能 App/管理网页创建）。切换前必须先定产品流程和服务端接口。
8. **`HAL_I2C_DeInit()` 会关 I2C 时钟，`HAL_I2C_Init()` 不会重新开。**
   任何总线恢复后必须 `__HAL_RCC_I2Cx_CLK_ENABLE()`。
9. **不要用“每个地址都 ACK”判断 I2C 器件存在。** 这通常是 SDA/SCL 被拉死的总线假象；
   结合正常/交换 SCL/SDA 两个方向、以及示波器/万用表判断。

---

## 7. 修改后的验证清单

改动固件后至少完成：

- [ ] ECW 构建成功（无 `error:`），STM32 构建成功
- [ ] ECW 启动日志出现 `[audio] PDM armed`、`[sd] ready: ... sectors`、
      `[link] i2c slave ready: addr=0x42`
- [ ] STM32 启动日志出现 `touch chip id=0xB6`、`touch ready`
- [ ] 触摸一次，ECW 打印 `clip N queued: ... bytes, ...s`
- [ ] Wi-Fi 日志出现 `sta associated`、`got ip`
- [ ] 若改了上传逻辑：`[net] record event sent` / `clip N uploaded`，
      或明确的 HTTP 错误码
- [ ] 若改了回放：STM32 发 `0x03`，ECW 打印 `[play] clip ...`，播完恢复采集

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

- 当前代码：明文 HTTP，`POST /api/record` + `POST /api/audio`，32kHz/30 秒。
- 正式协议：`desk-recorder/v1`（服务端文档 `HARDWARE_INTEGRATION.md`）：
  MQTT 3.1.1+TLS 8883、HTTPS 443、Bearer 设备令牌、私有 CA、16kHz 单声道 16-bit WAV、
  UTC 校时、SHA256、`GET/POST/PUT/result` 四步、任务持久化与重启恢复。
- **硬冲突**：正式协议只允许 App/管理网页创建保存任务，设备不能自建。
  “触摸屏拍一下保存”需要服务端扩展或 App 中转，接入前必须先确认产品流程。
- 当前决定：服务器通信改造暂缓，保持现有软硬件逻辑不变。

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
2. HTTP 上传闭环（服务器可达性 + 服务端接口）。
3. 喇叭回放实机试听与音量/时序调整。
4. 正式服务器协议接入（16kHz、分钟级、MQTT+TLS/HTTPS、UTC、SHA256、任务持久化）。
5. 触摸触发保存与服务端“App 发起任务”的产品流程对齐。
6. 凭据轮换与公开仓库历史中的 Wi-Fi 密码清理。
7. `叫声音效/` 情绪音效的触发方式与存放位置（当前未被引用）。
