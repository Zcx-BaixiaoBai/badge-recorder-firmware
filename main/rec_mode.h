// main/rec_mode.h —— 熄屏录音模式（M1；design §4.1–§4.3）。
//
// 行为：进入后熄屏（背光 0 → DISPOFF → SLPIN → 挂起 LVGL timer），
// 24kHz 采集（ES8311 时钟约束）→ 3:2 重采样 16kHz（28 抽头抗混叠 FIR）
// → esp_opus_enc 16kbps/60ms 单声道 → FREC v3 分段写 recordings FAT 分区
// （每 5 分钟一段）。长按 OK 结束：终结段 → SLPOUT(120ms) → DISPON →
// 恢复 LVGL → 亮屏。本阶段只落盘不上传（无线电占空比上传是 M2 设备侧）。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 进入录音模式（在 worker 上下文调用；调用方 main.c 会先 best-effort 校时）：
// 挂载存储 → 起编码器/采集/录音任务 → 熄屏。成功后长按 OK 退出。
esp_err_t rec_mode_start(void);

// 退出录音模式：置停 → 等任务终结段（最长 8s）→ 亮屏。
// 返回 ESP_OK；若录音中发生致命错误，错误文案可由 rec_mode_last_error() 取。
esp_err_t rec_mode_stop(void);

bool rec_mode_active(void);

// 最近一次录音会话的错误文案（无错为空串）。
const char *rec_mode_last_error(void);
