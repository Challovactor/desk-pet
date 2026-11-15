# 文档索引

## 当前实机可工作版本（2026-11-15）

- [06-ecw-bringup-2026-11-15.md](06-ecw-bringup-2026-11-15.md)
  — 当前固件的架构、已确认引脚、构建/烧录、实机验证结果、关键修复与排障记录、
  服务器协议现状、安全注意和已知限制。
- [01-wiring.md](01-wiring.md)
  — 接线与硬件：总线分工、两板引脚表、电源与 SD 卡、上电前检查清单
  （引脚以 06 的实机确认表为准）。

## v2 新增功能

- [07-v2-gesture-sound.md](07-v2-gesture-sound.md)
  — 手势识别、STM32/ECW 命令、SD 情绪音效资产格式、首次启动入库、
  16 kHz 回放、ES8311 初始化顺序与未确认硬件点。
- [08-v2-server-upload.md](08-v2-server-upload.md)
  — desk-recorder/v1 四步上传、MQTT/TLS presence、16 kHz WAV + SHA256、
  配置项、服务端 schema 待确认项与验证清单。
- [09-logo-sd-i2c-handoff.md](09-logo-sd-i2c-handoff.md)
  — 开机 LOGO：SSD1677/800×480 驱动、PNG→黑白灰工具、SD 资产块、
  I2C 取图 7 次尝试与未解决的总线死锁（含后续排查建议）。
- [10-wifi-boot-sequencing.md](10-wifi-boot-sequencing.md)
  — Wi-Fi 连接与上电初始化顺序：为什么连接前必须主动扫描、为什么关联
  期间不能重启 STA、SD 资产校验为何让位给首次关联（含验证清单）。
- [11-capture-sd-load-analysis.md](11-capture-sd-load-analysis.md)
  — 录音 SD 写入的 CPU 定量分析；"15 秒分段"方案为何不降低负载（不采纳）；
  网络任务优先级与"采集直接存 16 kHz"两种缓解手段的取舍。

## 子工程

- [ecw6700/README.md](../ecw6700/README.md) — ECW6700F 录音 / SD 环形存储 / 上传 / 喇叭回放
- [stm32f407/README.md](../stm32f407/README.md) — STM32F407 触摸事件桥

## 说明

`docs/02-protocol.md` ~ `docs/05-test-plan.md` 属于早期长期架构规划，当前仓库尚未包含；
当前可工作的引脚、协议和测试步骤请以 `06-ecw-bringup-2026-11-15.md` 为准。
