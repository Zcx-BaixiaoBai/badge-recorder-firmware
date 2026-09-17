// main/rec_mode.h —— 会话制熄屏录音（v2 架构，RAM 实测驱动的设计变更）。
//
// 背景：C3 无 PSRAM 实测（v0.4.x 系列 13 版）"WiFi+录音常驻"不成立（WiFi
// init 一次性 ~34KB 且 esp_wifi_stop 不释放；Opus 25KB 必须开机持有）。改
// 会话制：录音中 WiFi 完全 deinit（RAM+功耗双赢），同步窗口（停录→上传→
// 清空）独占堆。两种模式（用户 2026-09-18 定）：
//   手动模式（长按上键进入）：录满 30 分钟 → 同步 → 提示"可再次录音"。
//   续录模式（长按下键进入）：录满 30 分钟 → 同步 → 排空成功后自动续录。
//
// 行为：进入后熄屏；会话上限 30 分钟（30000 帧 @60ms = 3.6MB@16kbps，Flash
// 分区 78%）；同步失败停在待同步态（Flash 保护：不清空不续录，绝不写穿环，
// 遗留段由闲时心跳补传）。会话宿主任务持有整个"录音→同步→（续录）"循环，
// worker 只监视阶段变化刷 UI，所有网络都在 upload/worker 已有任务里。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    REC_MODE_MANUAL = 0,     // 30 分钟一节，同步后提示，用户手动再录
    REC_MODE_AUTO,           // 同步排空成功后自动续录
} rec_start_mode_t;

// 会话阶段（worker 轮询刷新 UI 用；IDLE 之外按键全部忽略，OK 长按除外）。
typedef enum {
    REC_PHASE_IDLE = 0,      // 无会话
    REC_PHASE_RECORDING,     // 录音中（WiFi 已 deinit，屏幕已熄）
    REC_PHASE_SYNC,          // 同步窗口（WiFi re-init，上传排空中）
} rec_phase_t;

// 开机预开 Opus 编码器 + 输出缓冲（必须在 WiFi 启动前：WiFi 之后堆碎片化，
// 编码器状态 ~25KB 的连续块分不出来）。幂等。
esp_err_t rec_mode_init(void);

// 进入一次录音会话（worker 上下文调用，立即返回；流程由会话任务异步推进）：
// 熄屏 → WiFi deinit → 起采集/录音任务。会话由 30 分钟上限或 rec_mode_stop 结束。
esp_err_t rec_mode_start(rec_start_mode_t mode);

// 请求结束（幂等、立即返回）：录音期=停止采集排空收尾；同步期=本节完成后
// 不再续录。真正的收尾/亮屏由会话任务完成后 worker 监视 phase 刷新。
esp_err_t rec_mode_stop(void);

bool rec_mode_active(void);            // 会话进行中（录音或同步，任一）
rec_phase_t rec_mode_phase(void);      // 当前阶段
rec_start_mode_t rec_mode_cur_mode(void);

// 最近一次会话的错误文案（无错为空串）。
const char *rec_mode_last_error(void);

// 同步流程（会话任务内部调用；也暴露给控制台手动触发）：WiFi re-init →
// 校时（upload 任务内）→ 上传排空。返回 true=排空成功。失败段留本机，
// 由闲时心跳补传；调用方据此决定是否续录（不清空不续录）。
bool rec_mode_sync_and_flush(void);
