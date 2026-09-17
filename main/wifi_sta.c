// main/wifi_sta.c —— 最小 STA 实现：连接 + 断线退避重连 + 驱动级 deinit。
//
// 三层生命周期（v0.4.27 会话制，RAM 实测驱动）：
//   栈层（一次）：esp_netif_init + 事件循环 + 默认 STA netif + 事件回调
//   驱动层：esp_wifi_init/deinit —— deinit 释放 ~34KB（esp_wifi_stop 不释放，
//           v0.4.x 实测）；录音会话开始前 deinit，同步窗口重建
//   无线电层：esp_wifi_start/stop —— 占空比（配置保留，重连由事件驱动）
// netif 跨 deinit/init 保留：v5.5 无 esp_netif_destroy_default_wifi（已移除），
// esp_wifi_deinit 文档无 netif 约束，wifi-netif 驱动句柄只是每接口轻包装。
#include "wifi_sta.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string.h>

static const char *TAG = "wifi_sta";
static EventGroupHandle_t s_eg;
static const int BIT_IP = BIT0;

static bool s_stack_up;     // netif_init/事件循环/netif/回调（开机一次）
static bool s_drv_up;       // esp_wifi_init 驱动存活
static bool s_radio_up;     // esp_wifi_start 无线电存活
static bool s_wifi_init_failed;   // esp_wifi_init NO_MEM：允许之后重试
// 尺寸与 wifi_sta_config_t 字段一致（32/64）：避免 format-truncation 告警
static char s_ssid[32];
static char s_pass[64];

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_eg, BIT_IP);
        if (!s_drv_up) return;            // deinit 竞态：驱动已卸，勿重连
        ESP_LOGW(TAG, "断线，2s 后重连");
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (s_drv_up) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "已连接, IP=" IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_eg, BIT_IP);
    }
}

void wifi_sta_start(const char *ssid, const char *pass)
{
    if (s_radio_up) return;               // 无线电已在：幂等
    if (ssid && ssid[0] && pass) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", pass);
    }
    if (!s_eg) s_eg = xEventGroupCreate();

    if (!s_stack_up) {                    // 栈层：开机一次（netif 必须在 esp_wifi_init 前建）
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
        esp_netif_create_default_wifi_sta();
        s_stack_up = true;
    }

    if (!s_drv_up) {                      // 驱动层：deinit 后重建（netif 已在，勿重复建）
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_LOGI(TAG, "esp_wifi_init 前空闲堆 %u B", (unsigned)esp_get_free_heap_size());
        esp_err_t e = esp_wifi_init(&init);
        if (e != ESP_OK) {
            // NO_MEM 不 abort（boot loop 会锁死调试路径）：记状态，可重试
            ESP_LOGE(TAG, "esp_wifi_init 失败 %s——本轮不可用，稍后重试", esp_err_to_name(e));
            s_wifi_init_failed = true;
            return;
        }
        s_wifi_init_failed = false;
        s_drv_up = true;
    }

    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", s_ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", s_pass);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_radio_up = true;
    // ★ 禁用 WiFi 省电：CPU 不进轻度睡眠，I2S DMA 持续运行。
    //   默认 WIFI_PS_MIN_MODEM 会让 CPU 在 beacon 间隔睡眠 → I2S DMA 停转 →
    //   麦克风录几块（陈旧 DMA 数据）后就再也没有新数据 → 读阻塞/超时。
    //   飞书固件 feishu_network.c:105 同样禁用省电，录音正常。
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "STA 启动, SSID=%s（驱动 %s）", s_ssid,
             s_wifi_init_failed ? "异常" : "就绪");
}

void wifi_sta_stop(void)
{
    if (!s_drv_up || !s_radio_up) return;
    esp_wifi_stop();                      // 驱动/事件/配置全保留；STA_START 事件驱动重连
    s_radio_up = false;
    xEventGroupClearBits(s_eg, BIT_IP);
    ESP_LOGI(TAG, "无线电已关（占空比）");
}

bool wifi_sta_resume(void)
{
    if (!s_drv_up) {
        // 驱动被 deinit 过（会话制录音）：完整重建后连接
        wifi_sta_start(NULL, NULL);
        return s_drv_up;
    }
    if (s_radio_up) return true;
    if (esp_wifi_start() != ESP_OK) return false;
    s_radio_up = true;
    esp_wifi_set_ps(WIFI_PS_NONE);        // I2S DMA 常转（并发采集的场景）
    ESP_LOGI(TAG, "无线电已开（占空比恢复）");
    return true;                          // 连接由 STA_START → 回调 esp_wifi_connect 自动发起
}

void wifi_sta_deinit(void)
{
    if (!s_stack_up) return;              // 从未 start 过：无事可做
    if (s_radio_up) wifi_sta_stop();
    if (!s_drv_up) return;
    uint32_t before = esp_get_free_heap_size();
    esp_wifi_deinit();                    // 释放 ~34KB；netif/回调/凭据保留
    s_drv_up = false;
    xEventGroupClearBits(s_eg, BIT_IP);
    ESP_LOGI(TAG, "WiFi 驱动已卸载（释放后空闲堆 %u → %u B）",
             (unsigned)before, (unsigned)esp_get_free_heap_size());
}

bool wifi_sta_configured(void)
{
    return s_ssid[0] != '\0';
}

bool wifi_wait_connected(int timeout_sec)
{
    if (!s_eg) return false;
    return xEventGroupWaitBits(s_eg, BIT_IP, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_sec * 1000)) & BIT_IP;
}

bool wifi_is_connected(void)
{
    return s_eg && (xEventGroupGetBits(s_eg) & BIT_IP);
}
