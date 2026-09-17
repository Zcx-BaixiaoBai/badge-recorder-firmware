// main/rec_upload.h —— 录音段上传（M2 设备侧；design §4.5 无线电占空比）。
//
// 策略：
// - 独立任务 + 通知（rec_upload_kick）。绝不在录音任务里同步上传——WiFi 关联
//   要数秒，而音频流缓冲只约 0.67s，阻塞即丢音频。
// - 无线电占空比：熄屏录音中收到 kick → 开无线电 → 校时 → 批量传 → 传完关掉
//   （录音已结束时留着给 UI 用）。
// - 断网补传 = 文件留在 /rec（分区可兜约 40 分钟），连通后按名字序（=段序）
//   排空；服务端按 (Device-Id, Segment-Id) 幂等去重，重传安全。
// - 上传成功即删除段文件：分区只有 4.625MiB，不删必然写满。
// - 闲时兜底：界面空闲（非录音、WiFi 已连）时由 main 心跳周期性 kick，
//   排空崩溃遗留 / 录音结束时的最后一段。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 起上传任务（幂等）。device id 取自 WiFi MAC（badge-<12hex>），网关地址/令牌
// 经 gw_client_base()/gw_client_token() 在上传时现取。
void rec_upload_init(void);

// 通知上传任务干活（可在任何任务上下文调用，只投递不等待）。
void rec_upload_kick(void);

// /rec 下已完成、待上传的段数（排除当前正在写的段）。快速扫描，心跳轮询用。
int rec_upload_pending(void);
