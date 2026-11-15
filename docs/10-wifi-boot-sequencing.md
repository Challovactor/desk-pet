# 10 · Wi-Fi 连接与上电初始化顺序（2026-11-15）

> 本文记录"ECW 烧录后 30–40 秒内连上手机热点"这个目标下的两处反复：
> 为什么删过连接前的主动扫描、为什么后来又要加回来；以及上电时各模块的
> 初始化顺序与理由。改 Wi-Fi 相关代码前先读本文。

---

## 1. 现象与结论

**现象**：某版固件上电后日志只有

```
[net] wifi stack ready
[net] connecting to Redmi K50 Ultra ...
```

之后 100 秒内**没有任何 wpa 扫描/关联日志**，`wifi=0` 一直不变。

**结论**：这个 SDK 的 `wpa_supplicant_select_network` 路径在"刚被
`sys_wifi_sta_stop()` 停过、且没有预热扫描"时不会自行发起扫描。连接前
必须主动扫描一次（更新 BSS 表并唤醒 supplicant），否则永远不会关联。

**证据**：
- 成功版（06:58，含主动扫描）：`scanning APs...` → `Selected BSS ...` →
  `tx eapol done:0` → `[net] got ip`（总计约 27 秒）。
- 失败版（10:38，删除扫描后）：`connecting ...` 之后 100 秒无任何 wpa 日志。

---

## 2. 反复的来龙去脉（避免第三次）

### 2.1 第一版：扫描 + 30 秒看门狗

- 流程：`auto_connect(false)` → `sta_stop()` → 扫描 → `sta_start()`；
  主循环里"30 秒无 IP 就 sta_stop/sta_start"。
- 问题：冷启动首次关联要 20–50 秒（扫描 + 四次握手 + DHCP），**30 秒看门狗
  正好在握手过程中重启 STA**，日志表现为：
  ```
  sta associated
  tx eapol done:-12        ← -12 = ERR_IF，链路被我们自己停掉
  sta disconnected reason=15
  no ip for 30s, restart sta
  ```
  AP 侧把这解释成 PSK 错误（reason=15），看起来像密码错，实际是时序问题。
  这个 bug 也解释了"为什么只有烧录后连不上"：烧录后的首次关联是冷启动，
  超过 30 秒；平时热重连 10 秒内完成，不会触发看门狗。

### 2.2 第二版：状态感知的 90 秒恢复 + 删掉扫描

- 修复：只在 `STA_STATUS_DISCON/STOP` 且 90 秒无 IP 时才介入；关联中不打断；
  3 分钟硬上限。
- **同时的错误决定**：当时看到 `Selected BSS` 从 ~5 秒退化到 ~46 秒，把延迟
  归因于"扫描和 sta_start 内部扫描抢 supplicant"，于是把连接前扫描删了
  （`APP_WIFI_SCAN_DEBUG 0`）。
- 真实原因：46 秒延迟是 30 秒看门狗反复打断关联造成的；扫描只是背锅。
- 后果：supplicant 完全不扫描，100 秒零 wpa 日志，`wifi=0`，彻底连不上。

### 2.3 第三版（当前）：扫描 + 状态感知恢复 + 启动顺序优化

- 恢复 `APP_WIFI_SCAN_DEBUG 1`（连接前扫描，等待上限 10 秒）；
- 保留 90 秒防误断逻辑（不在关联/握手期间重启 STA）；
- 启动顺序调整：SD 资产校验**让位**给首次 Wi-Fi 关联（见第 3 节）；
- 资产校验改为"只比头部 + 安装后完整校验"（见第 4 节）。

---

## 3. 上电初始化顺序（当前设计）

```
SDK 启动
  ├─ RF 校准 / wifi_ctrl 事件 / WPA 任务创建
  └─ moce_app_main()
       1. net_init()                 事件订阅（调度器前）
       2. sound_start()              打开 UART0 TTS（9600）+ codec 任务
       3. 创建任务：capture / trigger / upload
       4. net_start()                Wi-Fi 任务
       5. app_mqtt_start()           MQTT 任务（先等 Wi-Fi）

net_task（优先级 3）
  ├─ 等 sys_wifi_is_ready()
  ├─ auto_connect(false) → sta_stop() → 等 STA_STATUS_STOP
  ├─ 主动扫描（≤10 秒，打印 AP 列表）        ← 必需，见第 1 节
  ├─ pmk_cache_clean()                       清 NV 里的过期 PMKSA
  ├─ sys_wifi_sta_start()                    发起关联
  └─ 监控：DISCON/STOP 且 90 秒无 IP 才重启；180 秒硬上限

capture_task（优先级 5）
  ├─ 等 codec 任务就绪
  ├─ audio_init() / store_init()
  ├─ 等 net_connected()，最多 25 秒          ← 让位给扫描/握手
  ├─ assets_init()（SD 音效 + LOGO 入库/校验）
  └─ 采集循环（PDM → SD 环形区）

trigger_task（优先级 6）
  ├─ sound_init() → app_link_init()
  └─ 命令循环（20ms 读超时；LOGO 读请求现场读 SD 并预装）

upload / mqtt 任务（优先级 3）
  └─ 等 IP → HTTPS 校时 → MQTT presence/telemetry
```

### 为什么"等 Wi-Fi 再校验 SD 资产"

- SD 是 GPIO 位翻转软件 SPI，读写都是 CPU 忙等；`assets_init` 旧版每次启动
  要流式复算 191 KB（12–14 秒），完全可能把 WPA 任务饿死，导致扫描/握手超时。
- 改造后：先让扫描和关联跑完（最多等 25 秒），再跑资产流程；热点不可用时
  25 秒后照常入库，音效晚一点可用，录音/上传不受影响。
- 注意：`assets_init()` 期间手势音效会打印 "assets not ready"，属预期。

### 为什么触发任务优先级是 6

- LOGO 读请求需要在主机 40ms 等待内完成 SD 读 + 预装；
- 它绝大多数时间阻塞在 20ms 的 I2C 读超时上，占用极小；
- 高于采集任务（5）才能不被 SD 位翻转写拖住。

---

## 4. 资产校验的快速路径

- 每次启动只读 SD 上的 32 字节头部（音效块 + LOGO 块），与内嵌 blob 头部
  逐字段比较（magic/version/size/crc32/count）；
- 完整流式 CRC 只在**安装之后**做一次（`sd_blob_verify()` / LOGO 安装后校验）；
- 理由：静态资产安装后不再被写，逐启动复算 191KB+48KB 的收益极低，
  代价是 10+ 秒的 CPU 占用和 Wi-Fi 干扰。
- 风险：若某次写入中断后头部恰好完整而数据残缺，本次启动不会发现；
  重新烧录或清资产区即可恢复。出现音效异常时先怀疑这一点。

---

## 5. 改 Wi-Fi 代码的红线（这次踩过的坑）

1. **不要删除连接前的主动扫描**。没有它 supplicant 不扫描（100% 复现）。
2. **不要在 `STA_STATUS_CONNECTING/CONNECTED` 期间 `sta_stop/sta_start`**；
   关联+握手+DHCP 需要 20–50 秒，任何中途重启都会得到 reason=15（假 PSK 错误）。
3. **扫描/关联窗口内不要做重 CPU 工作**（SD 位翻转、191KB CRC、48KB 安装）。
4. `reason=15` 先怀疑打断/时钟问题，再看热点密码；`tx eapol done:-12`
   是 lwIP `ERR_IF`，说明发送时链路已被我们自己断开。
5. 调试顺序：先看 `[net] connecting` 之后有没有 wpa 日志；没有 = 没扫描；
   有 `Selected BSS` 才进入握手问题。

---

## 6. 追加记录：AP 踢线后的恢复路径（2026-11-15 实测）

- 首次连接达标（扫描 0.76s → Selected BSS 3.7s → `got ip` 5.0s）；
- 约 60 秒后热点主动 deauth：`reason=32769`，SDK 触发 fast reconnect；
- fast reconnect 的 4 次握手超时（`reason=0x1000`，即 15），
  90 秒后被本应用的恢复逻辑 `sta_stop → sta_start`；
- **恢复路径当时没有预扫描，重启后同样连不上**（日志末尾 `wifi=0`）。

已修复：`net_sta_restart()` 与启动路径一致，stop → 预扫描 → 清 PMK →
`sta_start`。以后任何"重新连接"的代码路径都必须带预扫描（红线 1）。

另：踢线前出现过 HTTPS 的 `handshake send failed (-0x004E)` 与
`tcp connect (-0x0044)`，发生在链路降级窗口内；需在链路稳定后复测才能判断
是服务端/网络问题还是断链症状。

## 7. AP 不活跃踢线的心跳对策（2026-11-15）

- 现象：连接约 60 秒后 AP 主动 deauth（`reason=32769`），手机热点没有可调的
  "不活跃踢线"开关；随后 SDK fast reconnect 的四次握手超时。
- 对策：设备侧每 **15 秒**向网关发一个 **ARP 请求**（`etharp_request`），
  42 字节，开销可忽略；任何上行帧都会刷新 AP 的客户端活跃计时器。
- 实现：`app_net.c` 的 `net_task` 监控循环，`NET_KEEPALIVE_MS=15000`；
  每 4 次（60 秒）打印一次 `[net] keepalive N (arp gw)` 便于确认。
- 实测 ARP 心跳**不够**：手机热点仍会在约 60–90 秒后把设备踢掉，且设备侧
  supplicant 收不到 deauth（静默掉线，`wifi=1` 但热点列表为空）。原因推测是
  热点只按"发给网关的单播数据帧"计算活跃度，ARP 是广播帧。
- 追加对策（2026-11-15）：每次拿到 IP 后调用
  `hal_sntp_period_set(30000, HAL_SNTP_SYNC_NTP)`，让 SDK 的 SNTP 客户端每
  **30 秒**向 NTP 服务器发一次**单播 UDP** 校时请求——既刷新热点计时器，
  又持续校准 `sent_at_ms`。SNTP 周期下限是 15 秒。
- MQTT 命令主题同时订阅 `.../commands/events`（API.md）与 `.../commands`
  （运维口径），避免主题歧义。
- 手机锁屏/省电会关闭热点射频（用户实测：锁屏后热点列表无客户端）。
  设备侧的静默掉线检测已加入：`s_sta_ip` 仍为 1 但 MQTT 连续 150 秒未连上
  时，强制 `sta_stop → 预扫描 → sta_start`（见 `app_net.c`）。

## 8. 验证清单（烧录后 60 秒内）

- [ ] `[net] scanning APs...` → `scan found N AP(s)`（N>0）
- [ ] `Selected BSS ... ssid='Redmi K50 Ultra'`
- [ ] `tx eapol done:0`（不是 -12）
- [ ] `[net] sta associated` → `[net] got ip`（目标：上电后 30–40 秒）
- [ ] `[mqtt] connected, subscribing ...`
- [ ] `[asset] ready: 4 sounds` / `[asset] logo ready: 800x480`（在 Wi-Fi 之后）
- [ ] 单击屏幕：`link_ok` 增长、ECW 打印手势与 clip
