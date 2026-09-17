// main/frec_store.c —— FREC v3 段容器实现。
//
// 存储路径：recordings 分区（FAT16 + wear levelling，挂载点 /rec）。
// 官方录音笔同款挂载方式（esp_vfs_fat_spiflash_mount_rw_wl），簇 4096B。
// 文件名 R%07u.FRC（FAT 8.3 合法，seq 来自 NVS 计数器保证跨重启不重复）。
// 掉电恢复：mount 后扫描 /rec 下 header.payload_size==0 的段，逐帧扫描重算
// 统计并回填——对应官方固件的 "Recovered interrupted recording R%07lu"。
#include "frec_store.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wear_levelling.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp

static const char *TAG = "frec";

#define MOUNT_POINT   "/rec"
#define PART_LABEL    "recordings"
#define NVS_NS        "frec"
#define NVS_KEY_SEQ   "seq"
#define WBUF_SIZE     4096

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_mounted;
static FILE *s_fp;
static uint32_t s_seq;
static uint32_t s_payload;        // 已写字节数（帧数据，不含长度前缀）
static uint32_t s_frames;
static uint8_t s_wbuf[WBUF_SIZE]; // 写缓冲
static size_t s_wlen;

_Static_assert(sizeof(frec_hdr_t) == 48, "FREC v3 头必须 48 字节");

static void hdr_fill(frec_hdr_t *h, uint64_t start_ts_ms, int finalized)
{
    memset(h, 0, sizeof(*h));
    h->magic = 0x43455246;                 // "FREC" 小端
    h->ver = 3;
    h->hdr_size = sizeof(frec_hdr_t);
    h->sample_rate = 16000;
    h->channels = 1;
    h->frame_ms = 60;
    h->payload_size = finalized ? s_payload : 0;
    h->frame_count = finalized ? s_frames : 0;
    h->bitrate = 16000;
    h->seq = s_seq;
    h->start_ts_ms = start_ts_ms;
}

static esp_err_t nvs_next_seq(uint32_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    uint32_t v = 0;
    nvs_get_u32(h, NVS_KEY_SEQ, &v);
    v += 1;
    esp_err_t e = nvs_set_u32(h, NVS_KEY_SEQ, v);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) *out = v;
    return e;
}

// 掉电恢复：payload_size==0 的段按帧扫描重算统计并回填。
static void recover_unfinalized(void)
{
    DIR *d = opendir(MOUNT_POINT);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcasecmp(dot, ".FRC") != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), MOUNT_POINT "/%s", ent->d_name);
        FILE *f = fopen(path, "r+b");
        if (!f) continue;
        frec_hdr_t h;
        if (fread(&h, 1, sizeof(h), f) != sizeof(h) || h.magic != 0x43455246) {
            fclose(f);
            continue;
        }
        if (h.payload_size != 0) { fclose(f); continue; }     // 已终结，正常
        // 逐帧扫描：[u16 len][data]，直到 EOF 或异常长度
        uint32_t payload = 0, frames = 0;
        for (;;) {
            uint16_t len = 0;
            if (fread(&len, 1, 2, f) != 2) break;
            if (len == 0 || len > 8192) break;                 // 长度异常：截到这里
            if (fseek(f, len, SEEK_CUR) != 0) break;
            payload += len;
            frames++;
        }
        h.payload_size = payload;
        h.frame_count = frames;
        if (fseek(f, 0, SEEK_SET) == 0) fwrite(&h, 1, sizeof(h), f);
        fclose(f);
        ESP_LOGW(TAG, "掉电恢复 %s：回填 %u 帧 %uB", ent->d_name, frames, payload);
    }
    closedir(d);
}

esp_err_t frec_store_mount(void)
{
    if (s_mounted) return ESP_OK;
    const esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = true,     // 首次上电：空分区自动格式化
        .max_files = 4,
        .allocation_unit_size = 4096,       // 与官方录音笔一致（FAT16 簇 4096B）
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, PART_LABEL, &mc, &s_wl);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "recordings 分区挂载失败: %s", esp_err_to_name(e));
        return e;
    }
    s_mounted = true;
    recover_unfinalized();
    ESP_LOGI(TAG, "recordings 挂载于 %s", MOUNT_POINT);
    return ESP_OK;
}

bool frec_store_is_mounted(void) { return s_mounted; }

bool frec_store_is_active(const char *fname)
{
    if (!s_fp || !fname) return false;
    char cur[16];
    snprintf(cur, sizeof(cur), "R%07u.FRC", s_seq);
    return strcmp(cur, fname) == 0;
}

static esp_err_t wbuf_flush(void)
{
    if (s_wlen && s_fp) {
        if (fwrite(s_wbuf, 1, s_wlen, s_fp) != s_wlen) return ESP_FAIL;
        s_wlen = 0;
    }
    return ESP_OK;
}

esp_err_t frec_seg_begin(uint64_t start_ts_ms, uint32_t *seq_out)
{
    if (!s_mounted || s_fp) return ESP_ERR_INVALID_STATE;
    if (nvs_next_seq(&s_seq) != ESP_OK) {
        // NVS 不可用时退化为内存自增（掉电可能重号，服务端按 start_ts 去重兜底）
        static uint32_t s_fallback;
        s_fallback += 1;
        s_seq = s_fallback;
        ESP_LOGW(TAG, "NVS seq 分配失败，退化为内存计数 %u", s_seq);
    }
    char path[64];
    snprintf(path, sizeof(path), MOUNT_POINT "/R%07u.FRC", s_seq);
    s_fp = fopen(path, "wb");
    if (!s_fp) {
        ESP_LOGE(TAG, "创建 %s 失败（分区满？）", path);
        return ESP_FAIL;
    }
    s_payload = 0;
    s_frames = 0;
    s_wlen = 0;
    frec_hdr_t h;
    hdr_fill(&h, start_ts_ms, 0);
    if (fwrite(&h, 1, sizeof(h), s_fp) != sizeof(h)) {
        fclose(s_fp);
        s_fp = NULL;
        return ESP_FAIL;
    }
    if (seq_out) *seq_out = s_seq;
    ESP_LOGI(TAG, "段 R%07u 开始（start_ts=%llu ms）", s_seq, start_ts_ms);
    return ESP_OK;
}

esp_err_t frec_seg_frame(const uint8_t *opus, uint16_t len)
{
    if (!s_fp || len == 0) return ESP_ERR_INVALID_STATE;
    uint8_t pre[2] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    if (s_wlen + 2 + len > WBUF_SIZE) {           // 攒不下：先落盘已有缓冲
        if (wbuf_flush() != ESP_OK) return ESP_FAIL;
    }
    if (2 + len <= WBUF_SIZE) {
        memcpy(s_wbuf + s_wlen, pre, 2);
        s_wlen += 2;
        memcpy(s_wbuf + s_wlen, opus, len);
        s_wlen += len;
    } else {                                      // 单帧超缓冲（60ms@16kbps≈200B，理论不至）：透写
        if (fwrite(pre, 1, 2, s_fp) != 2) return ESP_FAIL;
        if (fwrite(opus, 1, len, s_fp) != len) return ESP_FAIL;
    }
    s_payload += len;
    s_frames += 1;
    return ESP_OK;
}

esp_err_t frec_seg_end(void)
{
    if (!s_fp) return ESP_ERR_INVALID_STATE;
    if (wbuf_flush() != ESP_OK) {
        ESP_LOGE(TAG, "段 R%07u flush 失败", s_seq);
        fclose(s_fp);
        s_fp = NULL;
        return ESP_FAIL;
    }
    // 回填头部统计（占位头写在文件开头，rewind 覆写）
    frec_hdr_t h;
    uint64_t start_ts = 0;
    {   // 保留 begin 时写的 start_ts（从现有头部读回，避免再传参）
        frec_hdr_t old;
        rewind(s_fp);
        if (fread(&old, 1, sizeof(old), s_fp) == sizeof(old)) start_ts = old.start_ts_ms;
    }
    hdr_fill(&h, start_ts, 1);
    rewind(s_fp);
    if (fwrite(&h, 1, sizeof(h), s_fp) != sizeof(h)) {
        ESP_LOGE(TAG, "段 R%07u 回填头部失败", s_seq);
    }
    fclose(s_fp);
    s_fp = NULL;
    ESP_LOGI(TAG, "段 R%07u 完成：%u 帧 %uB", s_seq, s_frames, s_payload);
    return ESP_OK;
}
