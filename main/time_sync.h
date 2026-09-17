// main/time_sync.h —— /health 校时（设备无 RTC / 无 SNTP 的最简方案）。
//
// 背景与依据：官方录音笔固件的 FAT 文件时间是 1980-01-01（从未校过时）；
// 固件与网关里也没有任何 SNTP。没有墙钟则"按天汇总 / 跨重启排序 / 会议对
// 日历"全失效（design §4.4，M1 必修）。方案：连上网关后 GET /health 拿
// server_time_ms（recorder-server 已提供），settimeofday 设墙钟基准，之后
// 靠系统单调时钟推进；每次重启 / 断网重连都重校。段文件的 start_ts_ms 即取
// 自此处；未校时（年份 < 2020）写 0，服务端按到达时间兜底并记录 arrived_at。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// GET {gw_base}/health → 解析 "server_time_ms" → settimeofday。
// gw_base 允许不带协议头（自动补 http://，与 gw_client 同一容错）。
// 成功返回 true；网络失败/解析失败返回 false（不致命，之后可再试）。
bool time_sync_from_gateway(const char *gw_base);

// 当前墙钟毫秒。从未校时（年份 < 2020）返回 0。
uint64_t time_now_ms(void);
