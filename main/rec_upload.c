// main/rec_upload.c —— 录音段上传实现（M2 设备侧；design §4.5 无线电占空比）。
//
// 契约（recorder-server ingest，design §6）：
//   POST {base}/rec/segment
//   X-Gateway-Token / X-Device-Id / X-Segment-Id / X-Start-Ts / X-End-Ts /
//   X-Seq / X-Bitrate；body = 整个 FREC v3 文件（头+帧流）。
//   服务端幂等：同 (Device-Id, Segment-Id) 返回 dedup:true——设备端据此可
//   安全重传（断网重试 / 崩溃遗留）。
//
// 结构说明（安全扫描友好）：上传主循环为单一函数，路径/段 ID 均在函数内
// 由【局部变量】按固定格式重建（字面量尺寸局部数组 + sizeof 定界），seq
// 来自本模块白名单解析（R+7位数字+.FRC），不跨函数边界传递路径或名称。
// 上传成功后 remove() 回收分区空间（不删则 4.625MiB 约 40 分钟写满）。
//
// 出站 URL：仅接受 http/https 前缀的网关基址（gw_base_ok()），其余拒绝上传。
//
// 上传用 Content-Length 定长流式（4KB 读块），不用 chunked：服务端 handler
// 按 Content-Length 读体（chunked 它不认），且文件大小本就已知（fseek/ftell）。
// esp_http_client_open(c, len) 后循环 esp_http_client_write；写失败即中止，
// 文件留在 /rec 等下一轮（补传队列）。
#include "rec_upload.h"

#include "frec_store.h"
#include "gw_client.h"
#include "rec_mode.h"
#include "time_sync.h"
#include "wifi_sta.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "recupload";

#define MAX_BATCH     16            // 单轮最多处理的段数（分区容量 ≈ 8 段，留余量）
#define READ_CHUNK    4096

static TaskHandle_t s_task;
static char s_device_id[24];        // "badge-<12 hex>"
static uint8_t s_rbuf[READ_CHUNK];

// ---- 设备 ID / 段名 / 出站校验 ----

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

// 文件名白名单解析：R + 7位数字 + .FRC（长度 12 定长）→ seq。
// 不合规格一律拒绝，其内容永不进入任何路径/请求。
static bool parse_seg_name(const char *s, uint32_t *seq_out)
{
    if (!s || strlen(s) != 12 || s[0] != 'R') return false;
    for (int i = 1; i <= 7; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    if (strcmp(s + 8, ".FRC") != 0) return false;
    *seq_out = (uint32_t)strtoul(s + 1, NULL, 10);
    return true;
}

static bool seg_is_active(uint32_t seq)
{
    char cur[16];
    snprintf(cur, sizeof(cur), "R%07u.FRC", seq);
    return frec_store_is_active(cur);
}

// ---- 批量上传（单一函数：路径/ID 全部由局部变量重建） ----

static int cmp_seq(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int collect_seqs(uint32_t *seqs, int cap)
{
    DIR *d = opendir("/rec");
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < cap) {
        uint32_t seq;
        if (!parse_seg_name(ent->d_name, &seq)) continue;     // 非白名单：忽略
        if (seg_is_active(seq)) continue;                     // 正在写的段不上传
        seqs[n++] = seq;
    }
    closedir(d);
    qsort(seqs, n, sizeof(seqs[0]), cmp_seq);                 // 段序 = 时间序
    return n;
}

static int flush_pending(void)
{
    static uint32_t seqs[MAX_BATCH];
    int n = collect_seqs(seqs, MAX_BATCH);
    if (n == 0) return 0;
    if (!gw_base_ok() || !gw_client_token()[0]) {
        ESP_LOGW(TAG, "网关未配置或协议非法，%d 段留在本机", n);
        return 0;
    }

    char url[112];
    snprintf(url, sizeof(url), "%s/rec/segment", gw_client_base());
    int sent = 0;

    for (int i = 0; i < n; i++) {
        uint32_t seq = seqs[i];                // 局部变量：路径只由它派生
        char path[64];
        snprintf(path, sizeof(path), "/rec/R%07u.FRC", seq);
        char seg_id[16];
        snprintf(seg_id, sizeof(seg_id), "R%07u", seq);

        FILE *f = fopen(path, "rb");
        if (!f) continue;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long fsize = ftell(f);
        if (fseek(f, 0, SEEK_SET) != 0 || fsize < (long)sizeof(frec_hdr_t)) {
            fclose(f);
            continue;
        }
        frec_hdr_t h;
        bool okhdr = (fread(&h, 1, sizeof(h), f) == sizeof(h)) && h.magic == 0x43455246;
        if (!okhdr) { fclose(f); continue; }

        char ts[24], te[24], sq[16], br[16];
        uint64_t end_ms = h.start_ts_ms + (uint64_t)h.frame_count * h.frame_ms;
        snprintf(ts, sizeof(ts), "%llu", (unsigned long long)h.start_ts_ms);
        snprintf(te, sizeof(te), "%llu", (unsigned long long)end_ms);
        snprintf(sq, sizeof(sq), "%lu", (unsigned long)h.seq);
        snprintf(br, sizeof(br), "%lu", (unsigned long)h.bitrate);

        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 60000,
            .buffer_size = 1024,
            .buffer_size_tx = READ_CHUNK,
            .disable_auto_redirect = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) { fclose(f); break; }
        esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
        esp_http_client_set_header(c, "X-Gateway-Token", gw_client_token());
        esp_http_client_set_header(c, "X-Device-Id", s_device_id);
        esp_http_client_set_header(c, "X-Segment-Id", seg_id);
        esp_http_client_set_header(c, "X-Start-Ts", ts);
        esp_http_client_set_header(c, "X-End-Ts", te);
        esp_http_client_set_header(c, "X-Seq", sq);
        esp_http_client_set_header(c, "X-Bitrate", br);

        bool uploaded = false;
        if (esp_http_client_open(c, (int)fsize) == ESP_OK) {
            bool aborted = false;
            long left = fsize;
            while (left > 0) {
                size_t want = left > READ_CHUNK ? READ_CHUNK : (size_t)left;
                size_t got = fread(s_rbuf, 1, want, f);
                if (got == 0) { aborted = true; break; }
                int w = esp_http_client_write(c, (const char *)s_rbuf, (int)got);
                if (w < (int)got) { aborted = true; break; }
                left -= got;
            }
            if (!aborted) {
                esp_http_client_fetch_headers(c);
                if (esp_http_client_get_status_code(c) == 200) {
                    uploaded = true;                // 200 含 dedup:true：服务端已有
                    ESP_LOGI(TAG, "OK %s %ldB", seg_id, fsize);
                } else {
                    ESP_LOGW(TAG, "%s HTTP %d", seg_id,
                             esp_http_client_get_status_code(c));
                }
            } else {
                ESP_LOGW(TAG, "%s 上传中断（网络）", seg_id);
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        fclose(f);

        if (uploaded) {
            sent++;
            if (remove(path) != 0) {
                // 删失败（极少见）：段保留，下轮重传 → 服务端 dedup → 再删
                ESP_LOGW(TAG, "%s 删除失败，留待重传", seg_id);
            }
        } else {
            break;                                  // 失败即停，整轮留下轮补传
        }
    }
    return sent;
}

// ---- 任务 ----

static void upload_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);              // 等 kick
        bool recording = rec_mode_active();
        bool we_started_radio = false;

        if (!wifi_is_connected()) {
            if (!recording) continue;                         // 闲时无网：不动无线电（UI 场景自己管）
            if (!wifi_sta_resume()) {                         // 占空比：开无线电
                ESP_LOGW(TAG, "无线电恢复失败，本轮放弃");
                continue;
            }
            we_started_radio = true;
            if (!wifi_wait_connected(20)) {                   // 20s 连不上就放弃，文件留着
                ESP_LOGW(TAG, "WiFi 未连上，本轮放弃");
                wifi_sta_stop();
                continue;
            }
            time_sync_from_gateway(gw_client_base());         // 每次重连都重校（design §4.4）
        }

        int sent = flush_pending();
        ESP_LOGI(TAG, "本轮上传 %d 段（剩 %d 待传）", sent, rec_upload_pending());

        // 占空比收尾：仍处录音模式才关无线电；录音已结束（退回 UI）就留着给界面用
        if (we_started_radio && rec_mode_active()) wifi_sta_stop();
    }
}

// ---- 对外 ----

void rec_upload_init(void)
{
    device_id_init();
    if (s_task) return;
    // 优先级 4：低于 rec_task(5)/采集(6)——音频永不因上传让路
    if (xTaskCreate(upload_task, "rec_upload", 6144, NULL, 4, &s_task) != pdPASS) {
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
    static uint32_t seqs[MAX_BATCH];
    return collect_seqs(seqs, MAX_BATCH);
}
