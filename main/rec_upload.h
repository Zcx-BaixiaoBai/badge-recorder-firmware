// main/rec_upload.h —— 录音段上传（v2 会话制：被动上传）。
//
// 策略（v0.4.27 起，无线电占空比逻辑移除——录音期 WiFi 已整体 deinit）：
// - 独立任务 + 通知（rec_upload_kick）。谁保证网络可用谁负责：
//   同步窗口（rec_mode_sync_and_flush）先连 WiFi 再 kick；闲时心跳只在
//   已连网时 kick。任务本身绝不碰无线电——无网即返回等待。
// - 每次从"未连 → 已连"跃迁时做一次 /health 校时（design §4.4）。
// - 断网补传 = 段留在裸分区环（4.625MiB ≈ 40 分钟），连通后按段序排空；
//   服务端按 (Device-Id, Segment-Id) 幂等去重，重传安全。
// - 上传成功即删除段：环空间回收，不删必然写满。
// - 闲时兜底：界面空闲（非会话、WiFi 已连）时由 main 心跳周期性 kick。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 起上传任务（幂等）。device id 取自 WiFi MAC（badge-<12hex>），网关地址/令牌
// 经 gw_client_base()/gw_client_token() 在上传时现取。
void rec_upload_init(void);

// 通知上传任务干活（可在任何任务上下文调用，只投递不等待；通知合并）。
void rec_upload_kick(void);

// 已完成、待上传的段数（排除当前正在写的段）。快速扫描，心跳轮询用。
int rec_upload_pending(void);
