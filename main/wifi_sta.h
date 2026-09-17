// main/wifi_sta.h —— STA 模式连网（凭据来自 menuconfig，见 Kconfig.projbuild）
#pragma once

#include <stdbool.h>

// 启动 WiFi STA 并发起连接（非阻塞；断线自动重连）。幂等：重复调用无副作用。
void wifi_sta_start(const char *ssid, const char *pass);

// 无线电占空比（rec_upload 用）：只停 WiFi 协议栈，netif/事件/配置全保留；
// 再 wifi_sta_resume() 触发 STA_START 事件 → 回调自动重连。从未 start 过时空操作。
void wifi_sta_stop(void);

// 无线电重新上电（配置保留）。从未初始化返回 false（调用方应走 wifi_sta_start）。
bool wifi_sta_resume(void);

// 阻塞等待拿到 IP。超时返回 false。
bool wifi_wait_connected(int timeout_sec);

bool wifi_is_connected(void);
