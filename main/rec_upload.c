// main/rec_upload.c —— 录音段上传实现（裸分区版，配合 frec_store v2）。
//
// 契约（recorder-server ingest，design §6）：
//   POST {base}/rec/segment
//   X-Gateway-Token / X-Device-Id / X-Segment-Id / X-Start-Ts / X-End-Ts /
//   X-Seq / X-Bitrate；body = FREC v3 段（48B 头 + 帧流）。
//   服务端幂等：同 (Device-Id, Segment-Id) 返回 dedup:true——重传安全。
//
// 段数据经 frec_store_read 流式读出（无文件系统、无路径、无 fopen）：
// 1KB 读块（静态 s_rbuf 也缩到 1KB，给同步窗口的紧张堆让路）→
// Content-Length 定长上传（服务端 handler 只认 Content-Length）。
// 上传成功即 frec_store_delete（环空间回收，4.625MiB 不回收 40 分钟写满）。
// 失败即停整轮，段留队列下轮补传。
#include "rec_upload.h"

#include "frec_store.h"
#include "gw_client.h"
#include "time_sync.h"
#include "wifi_sta.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "recupload";

#define MAX_BATCH     16            // 单轮最多处理的段数（>80 分钟积压，足够）
// ★ v0.4.44：写块 1460→512。真机双端抓包铁证：POST 头（343B）能发、服务
//   器已确认，第一个整 MSS（1460B）写永远滞留在驱动 TX 队列（无报错、无
//   线上帧）。史上所有成功传输 ≤343B（84/142/343/48B），所有 ≥MSS 的写
//   （含 v0.4.26 的 2048）全部卡死。512B 属于已验证可流出的尺寸类；
//   Nagle 自节流下 ~20-40s/178KB 段（120s timeout 内）。
#define READ_CHUNK    512

static TaskHandle_t s_task;
static char s_device_id[24];        // "badge-<12 hex>"
static uint8_t s_rbuf[READ_CHUNK];
static bool s_was_connected;        // 未连→已连 跃迁：触发一次校时

// ---- 设备 ID / 出站校验 ----

static void device_id_init(void)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id), "badge-unknown");
        return;
    }
    snprintf(s_device_id, sizeof(s_device_id), "badge-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 网关基址白名单：仅 http/https（来自操作者配网写入 NVS，出站前再验一次）
static bool gw_base_ok(void)
{
    const char *base = gw_client_base();
    return base[0] != '\0' &&
           (strncmp(base, "http://", 7) == 0 || strncmp(base, "https://", 8) == 0);
}

// ---- 单段上传（数据全部来自索引与裸分区读） ----

static esp_err_t upload_one(const frec_seg_info_t *seg)
{
    char url[112];
    snprintf(url, sizeof(url), "%s/rec/segment", gw_client_base());
    char seg_id[16];
    snprintf(seg_id, sizeof(seg_id), "R%07lu", (unsigned long)seg->seq);
    // 上传总长 = FREC 头 48B + Σ(2+帧)
    uint32_t total = 48 + seg->payload_size + seg->frame_count * 2;
    uint64_t end_ms = seg->start_ts_ms + (uint64_t)seg->frame_count * seg->frame_ms;
    char ts[24], te[24], sq[16], br[16];
    snprintf(ts, sizeof(ts), "%llu", (unsigned long long)seg->start_ts_ms);
    snprintf(te, sizeof(te), "%llu", (unsigned long long)end_ms);
    snprintf(sq, sizeof(sq), "%lu", (unsigned long)seg->seq);
    snprintf(br, sizeof(br), "%lu", (unsigned long)seg->bitrate);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,       // 大段兜底（MSS 写正常 ~1-2s/段）
        .buffer_size = 512,
        .buffer_size_tx = READ_CHUNK,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(c, "X-Gateway-Token", gw_client_token());
    esp_http_client_set_header(c, "X-Device-Id", s_device_id);
    esp_http_client_set_header(c, "X-Segment-Id", seg_id);
    esp_http_client_set_header(c, "X-Start-Ts", ts);
    esp_http_client_set_header(c, "X-End-Ts", te);
    esp_http_client_set_header(c, "X-Seq", sq);
    esp_http_client_set_header(c, "X-Bitrate", br);

    esp_err_t ret = ESP_FAIL;
    if (esp_http_client_open(c, (int)total) == ESP_OK) {
        bool aborted = false;
        uint32_t off = 0;
        while (off < total) {
            size_t want = total - off > READ_CHUNK ? READ_CHUNK : (size_t)(total - off);
            if (frec_store_read(seg, off, s_rbuf, want) != ESP_OK) {
                aborted = true;                              // 读分区失败（不该发生）
                break;
            }
            int w = esp_http_client_write(c, (const char *)s_rbuf, (int)want);
            if (w < (int)want) { aborted = true; break; }    // 网络中断
            off += want;
        }
        if (!aborted) {
            esp_http_client_fetch_headers(c);
            int status = esp_http_client_get_status_code(c);
            if (status == 200) {                              // 含 dedup:true：服务端已有
                char body[128];
                int n = 0, r;
                while (n < (int)sizeof(body) - 1
                       && (r = esp_http_client_read(c, body + n, sizeof(body) - 1 - n)) > 0) {
                    n += r;
                }
                body[n] = '\0';
                ret = ESP_OK;
                ESP_LOGI(TAG, "OK %s %uB", seg_id, (unsigned)total);
            } else {
                ESP_LOGW(TAG, "%s HTTP %d", seg_id, status);
            }
        } else {
            ESP_LOGW(TAG, "%s 上传中断（网络）", seg_id);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (ret == ESP_OK && frec_store_delete(seg->seq) != ESP_OK) {
        ESP_LOGW(TAG, "%s 删除失败，留待重传（服务端幂等兜底）", seg_id);
    }
    return ret;
}

// ---- 批量 ----

static int flush_pending(void)
{
    int n = frec_store_pending();
    if (n == 0) return 0;
    if (!gw_base_ok() || !gw_client_token()[0]) {
        ESP_LOGW(TAG, "网关未配置或协议非法，%d 段留在本机", n);
        return 0;
    }
    int sent = 0;
    for (int i = 0; i < n && i < MAX_BATCH; i++) {
        frec_seg_info_t seg;
        if (!frec_store_get(i, &seg)) break;
        if (upload_one(&seg) != ESP_OK) break;               // 失败即停，整轮留下轮
        sent++;
    }
    return sent;
}

// ---- 任务 ----

static void upload_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);              // 等 kick
        // v2 被动模型：任务不碰无线电。同步窗口/闲时路径都已把网弄好才 kick。
        if (!wifi_is_connected()) {
            s_was_connected = false;
            continue;
        }
        // 未连→已连 跃迁：重校时（同步窗口重建驱动后、闲时断线重连后都覆盖）
        if (!s_was_connected) {
            s_was_connected = true;
            time_sync_from_gateway(gw_client_base());
        }
        int sent = flush_pending();
        if (sent > 0 || frec_store_pending() > 0) {
            ESP_LOGI(TAG, "本轮上传 %d 段（剩 %d 待传）", sent, frec_store_pending());
        }
    }
}

// ---- 对外 ----

void rec_upload_init(void)
{
    device_id_init();
    if (s_task) return;
    // 4KB：esp_http_client 全流程在本任务跑（v2 同步窗口堆紧张，本任务栈
    // 换来 HTTP 可靠性；优先级 4 低于 rec_task(5)/采集(6)——录音优先）
    if (xTaskCreate(upload_task, "rec_upload", 4096, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "上传任务创建失败");
        s_task = NULL;
    }
}

void rec_upload_kick(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

int rec_upload_pending(void)
{
    return frec_store_pending();
}
