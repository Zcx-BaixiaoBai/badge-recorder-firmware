// main/wifi_sta.h —— STA 模式连网（凭据经 NVS 配置写入，见 badge_cfg）。
#pragma once

#include <stdbool.h>

// 启动 WiFi STA 并发起连接（非阻塞；断线自动重连）。幂等。
//   ssid/pass 非空 = 保存凭据并连接；NULL/空 = 沿用上次凭据（deinit 后重建）。
//   生命周期分层（v0.4.27 会话制）：
//     netif + 事件循环 + 事件回调   开机一次，永不重建
//     esp_wifi_init 驱动           wifi_sta_deinit() 释放 ~34KB，start 可重建
//     esp_wifi_start 无线电        stop/resume 占空比（配置保留）
void wifi_sta_start(const char *ssid, const char *pass);

// 无线电占空比：只停 esp_wifi_start，驱动/netif/配置全保留；
// wifi_sta_resume() 触发 STA_START 事件 → 回调自动重连。从未 start 过时空操作。
void wifi_sta_stop(void);

// 无线电重新上电（配置保留）。从未初始化返回 false（调用方应走 wifi_sta_start）。
bool wifi_sta_resume(void);

// 彻底卸载 WiFi 驱动（esp_wifi_stop + esp_wifi_deinit）：释放 ~34KB 堆，
// 录音会话开始前调用。netif/事件/凭据全保留，wifi_sta_start(NULL,NULL)
// 即可重建（v0.5.5 文档：deinit 无 netif 前置约束；驱动句柄是每接口轻包装）。
void wifi_sta_deinit(void);

// 是否已配置过凭据（未配网设备可直接判失败，省 20s 连接超时）。
bool wifi_sta_configured(void);

// 阻塞等待拿到 IP。超时返回 false。
bool wifi_wait_connected(int timeout_sec);

bool wifi_is_connected(void);
