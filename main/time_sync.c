// main/time_sync.c —— /health 校时实现。
//
// 刻意不用 cJSON：/health 响应就一行小 JSON，定点找 "server_time_ms": 子串
// 再 strtoull 足够，还省一个解析缓冲。超时给 5s（与网关探活同级）。
#include "time_sync.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "timesync";

// 曾成功校时的标志（进程内）：重启后首次 getppid 前置 false。
static bool s_synced;

bool time_sync_from_gateway(const char *gw_base)
{
    if (!gw_base || !gw_base[0]) return false;
    char url[112];
    if (strncmp(gw_base, "http://", 7) == 0 || strncmp(gw_base, "https://", 8) == 0) {
        snprintf(url, sizeof(url), "%s/health", gw_base);
    } else {
        snprintf(url, sizeof(url), "http://%s/health", gw_base);
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            char body[256];
            int total = 0;
            while (total < (int)sizeof(body) - 1) {
                int r = esp_http_client_read(c, body + total, sizeof(body) - 1 - total);
                if (r <= 0) break;
                total += r;
            }
            body[total] = '\0';
            const char *p = strstr(body, "\"server_time_ms\"");
            if (p) {
                p = strchr(p + strlen("\"server_time_ms\""), ':');
                if (p) {
                    uint64_t ms = strtoull(p + 1, NULL, 10);
                    if (ms > 1000000000000ULL) {       // > 2001-09：像真的毫秒时间戳
                        struct timeval tv = {
                            .tv_sec = (time_t)(ms / 1000),
                            .tv_usec = (suseconds_t)((ms % 1000) * 1000),
                        };
                        settimeofday(&tv, NULL);
                        s_synced = true;
                        ok = true;
                        ESP_LOGI(TAG, "已校时: %llu ms", (unsigned long long)ms);
                    }
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "GET /health open 失败");
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

uint64_t time_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    // 未校时判断：系统 epoch 落在 2020 之前视为无效（ESP 默认 epoch=1970）。
    if (tv.tv_sec < 1577836800) return 0;
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}
