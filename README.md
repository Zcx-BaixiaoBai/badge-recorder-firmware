# 秒忆卡 MemoSnap · 工牌静默录音固件（badge-recorder-firmware）

AI Passport 工牌（ESP32-C3 / ES8311 / 8MB Flash 无 PSRAM / 520mAh）的**录音产品线**固件：
会话制熄屏录音（30 分钟一节、单次/自动续录双模式）、Opus 16kbps 编码、FREC v3
段存储（裸分区环形）、无线电占空比上传、脱网可用（离线录音+联网补传）。

服务端（转写/分离/声纹/纪要/日报/WebUI）：
<https://github.com/Zcx-BaixiaoBai/memosnap-server>（设计文档在该仓库 `docs/design.md`）。

> 本仓库与 ZCode 工牌仓库（zcode-badge）**分离**：录音产品独立演进。
> 两者共享 BSP / UI 框架 / 配网等平台代码（同硬件），ZCode 远控功能随平台携带、
> 与熄屏录音共存互不干扰；后续如需纯录音精简版可再剥离。

## 功能（M1+M2 设备侧）

- **熄屏录音模式**（设置菜单 → 熄屏录音模式，长按 OK 结束）：
  背光 0 → DISPOFF → SLPIN → 挂起 LVGL；亮屏 SLPOUT → 120ms → DISPON → 恢复。
- **音频链路**：ES8311 24kHz 采集（时钟硬约束）→ 28 抽头 FIR 3:2 重采样 16kHz
  → `esp_opus_enc` 16kbps/60ms 单声道（esp_audio_codec 组件）。
- **FREC v3 分段存储**：recordings 分区（FAT16+磨损均衡，cardid 之后 0x360000/0x4A0000），
  48B 头（含 start_ts_ms/seq/bitrate/frame_ms）+ `[u16 len][opus]` 帧；5 分钟一段；
  NVS 段序号跨重启不重号；掉电恢复（按帧扫描回填统计）。
- **无线电占空比上传**：独立任务（优先级低于录音，音频永不因上传让路）；
  每段终结 kick → 开 WiFi → 等连 → 批量上传（Content-Length 流式）→ 仍录音才关无线电；
  上传成功即删段（分区仅 4.625MiB）；失败留队列，服务端按 (Device-Id, Segment-Id) 幂等去重。
- **/health 校时**：连网即从网关取 server_time_ms 设墙钟；未校时段头写 0 由服务端兜底。
- **功耗数据源**：每次换段打 `电量%/mV/空闲堆` 到日志（COM10 USB 控制台采集）。

## 构建

CI（tag push 或 workflow_dispatch 触发）：ESP-IDF 5.5.3 / esp32c3，
产物 full.bin（bootloader+分区表+app 合并镜像），门禁 `./tools/validate.sh --firmware`
（保护 cardid 分区、3MB app 上限、recordings 分区不得与其重叠）。

本地：ESP-IDF v5.5.3 → `idf.py build` → `idf.py merge-bin`。

## 刷写

用 badgeflash（工牌插 USB，Windows 下 COM 口自动识别）：

```bash
python ../tools/badgeflash/badgeflash.py flash-file build/badge-recorder-full.bin \
    --offset 0x0 --name v0.4.0
```

合并镜像不含 recordings 分区数据（首次挂载自动格式化），且字节范围在 cardid
之前结束——已写设备身份的机器可安全直刷整包。

## 模块

```
main/rec_mode.c/h     熄屏录音状态机 + Opus 编码 + 3:2 重采样
main/frec_store.c/h   FREC v3 容器 + FAT/WL 挂载 + 掉电恢复
main/rec_upload.c/h   上传任务（无线电占空比 + 补传队列）
main/time_sync.c/h    /health 校时
main/wifi_sta.c       加 stop/resume（无线电占空比；STA_START 事件自动重连）
main/gw_client.c      复用其网关基址/令牌（/rec/segment 上传目标）
```

服务端契约见 recorder-server 的 README（`/rec/segment` 头字段与幂等语义）。
