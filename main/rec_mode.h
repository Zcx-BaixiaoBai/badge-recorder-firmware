// main/rec_mode.h —— 会话制熄屏录音（v2 架构，RAM 实测驱动的设计变更）。
//
// 背景：C3 无 PSRAM 实测（v0.4.x 系列）"WiFi+录音常驻"不成立（WiFi init
// 一次性 ~29KB 且 esp_wifi_stop 不释放；Opus 25KB 必须开机持有）。改会话制：
// 录音中 WiFi 完全 deinit（RAM+功耗双赢），同步窗口（停录→上传→清空）独占
// 堆。两种模式（用户 2026-09-17 定）：
//   手动模式（长按上键进入）：录满 30 分钟 → 同步 → 提示"可再次录音"。
//   续录模式（长按下键进入）：录满 30 分钟 → 同步 → 排空成功后自动续录。
//
// 行为：会话上限 30 分钟（30000 帧 @60ms = 3.66MB@16kbps，分区 78%）；
// 同步失败停在待同步态（Flash 保护：不清空不续录，绝不写穿环，遗留段由
// 闲时心跳补传）。
//
// 编排（v0.4.30：无独立会话任务，worker 轮询驱动，单任务天然互斥）：
//   worker/console: rec_mode_start() → [worker: 转 RECORDING 时 screen_off]
//   worker 每 250ms: rec_mode_poll()（录音收尾 → 转 SYNC）
//   worker: phase==SYNC 时 rec_mode_finish_session()（同步+续录判定，
//           续录成功内部起新节，否则亮屏回 IDLE）
//   屏幕/网络错误路径的教训见 rec_mode.c 注释（LVGL 锁、栈深、堆碎片）。
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

// 进入一次录音会话（worker 或 console 上下文，阻塞 ~1s）：WiFi deinit →
// 起 16KB rec_task + 采集。成功后 phase=RECORDING，由 30 分钟上限或
// rec_mode_stop 结束（rec_task 收尾后 worker 的 rec_mode_poll 转 SYNC）。
esp_err_t rec_mode_start(rec_start_mode_t mode);

// 请求结束（幂等、立即返回）：录音期=停止采集排空收尾；同步期=本节完成后
// 不再续录。
esp_err_t rec_mode_stop(void);

bool rec_mode_active(void);            // 会话进行中（录音或同步，任一）
rec_phase_t rec_mode_phase(void);      // 当前阶段
rec_start_mode_t rec_mode_cur_mode(void);

// 最近一次会话的错误文案（无错为空串）。
const char *rec_mode_last_error(void);

// worker 轮询入口（250ms）：检测录音收尾（30 分钟到点/用户停/致命错误）→
// 做采集收尾并把 phase 转 SYNC。幂等；IDLE 时无操作。
void rec_mode_poll(void);

// 会话收尾驱动（worker 上下文，phase==SYNC 时调用一次）：同步排空 → 续录
// 判定。续录成功则内部已起新节（phase 回 RECORDING，屏幕仍熄）；否则亮屏、
// 恢复网络、回 IDLE（worker 随后刷收尾 UI）。网络 HTTP 都在 upload 任务里。
void rec_mode_finish_session(void);

// 熄屏（持 bsp_lvgl_lock；worker 在 phase IDLE→RECORDING 转换时调用）。
void rec_mode_screen_off(void);

// 唤醒屏幕（持 bsp_lvgl_lock；录音中任意按键触发，10 秒无操作后由 worker 熄回）。
void rec_mode_screen_wake(void);

// 同步流程（worker 的 finish 内部调用；控制台手动触发需会话空闲）：WiFi
// re-init → 校时（upload 任务内）→ 上传排空。返回 true=排空成功。失败段留
// 本机，由闲时心跳补传；调用方据此决定是否续录（不清空不续录）。
bool rec_mode_sync_and_flush(void);
