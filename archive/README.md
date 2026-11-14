# 存档索引

## moce-v1-20261115 — v1 可工作版本

- 目录：`moce-v1-20261115/`
- 交接文档：`moce-v1-20261115/HANDOFF.md`
- 校验清单：`SHA256SUMS.txt`（49 个文件）

### 内容

- `stm32-touch-bridge/`：STM32F407 触摸桥源码、`build_firmware.sh`、`build/` 镜像
- `ecw6700-memory-node/`：ECW6700F 录音/存储/上传源码（`main/*.c/h` + `board_support/`）
- `ecw-build-output/`：ECW 可复现构建脚本 + `firmware_allinone.bin` 等产物
- `ecw-reverse-engineering/`：厂商 SDK / 烧录链路的逆向记录与脚本
- `hardware/`：2.0TP、SHIELD1/2、MIC、SD 引脚图 （厂商数据手册未随公开仓库分发）

### 状态

- 已实机验证：CST816D 触摸、板间 I²C、PDM/I2S 初始化、软件 SPI SD 卡、30 秒环形片段、Wi-Fi STA 连接。
- 待验证：音频实际内容（回放 WAV）、HTTP 上传（服务器 `YOUR_SERVER_IP:8080` 当时不可达）。

### 注意

- 存档中的 ECW 镜像是用**正式热点 `YOUR_WIFI_SSID`** 构建的；板上当前运行的可能是临时热点 `moce` 的旧镜像，需重新烧录才与存档一致。
- 工程路径含中文，ECW 构建需 `C:\moce` 目录联接；STM32 构建需 `C:\temp\armgcc` 工具链联接。详见 `HANDOFF.md` 第 4 节。

> 公开仓库不包含编译产物（`.bin/.elf/.hex/.map` 与 `build/`），因为 ECW 镜像中嵌有真实 Wi-Fi 凭据；请用随附的 `build_firmware.sh` 重新构建。
