// main/frec_store.c —— FREC v3 裸分区存储实现（v2，无 FAT）。
//
// 索引（扇区0，4096B）：64 × 64B 条目。写索引 = 擦扇区0 + 整体重写（原子性
// 靠条目 CRC：中断重写导致校验失败 → 按数据区 FREC 魔数重建索引）。
// 数据区（扇区1..）：段起始扇区对齐；头区 64B 预留（seg_end 写 FREC 头，
// 只写一次）；帧流 [u16 len][opus] 经 4KB 写缓冲攒批；写新扇区前先擦。
// 环绕：数据游标循环推进，遇"完成"段占用则 seg_begin 失败（背压）。
#include "frec_store.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "frec";

#define SEC_SZ          4096
#define HDR_AREA        64            // 每段头区（FREC 头 48B 预留）
#define MAX_ENTRIES     64            // 64 × 64B = 索引扇区
#define WBUF_SIZE       SEC_SZ        // 写缓冲 = 一个扇区
#define NVS_NS          "frec"
#define NVS_KEY_SEQ     "seq"
#define FREC_MAGIC      0x43455246    // "FREC" 小端
#define IDX_MAGIC       0x58444946    // "FIDX"

typedef struct __attribute__((packed)) {
    uint32_t magic;         // IDX_MAGIC
    uint8_t  state;         // 0=空闲 1=写中 2=完成
    uint8_t  pad[3];
    uint32_t crc;           // 本条目（除 crc 字段）的 CRC32
    uint32_t seq;
    uint64_t start_ts_ms;
    uint32_t addr;          // 数据区内偏移（扇区对齐，不含索引扇区）
    uint32_t sectors;       // 占用扇区数（含头区）
    uint32_t payload_size;
    uint32_t frame_count;
    uint32_t bitrate;
    uint16_t sample_rate;
    uint16_t frame_ms;
    uint32_t reserved[4];
} idx_entry_t;               // 56B → 编译期补到 64

_Static_assert(sizeof(idx_entry_t) == 64, "索引条目 64B×64=扇区0");

static const esp_partition_t *s_part;
static bool s_mounted;
static idx_entry_t s_idx[MAX_ENTRIES];
static uint32_t s_cursor;          // 数据游标（字节，数据区内偏移）
static uint32_t s_data_size;       // 数据区字节容量

// 写中段状态
static int      s_wslot = -1;      // 索引槽位
static uint32_t s_waddr;           // 段起始（数据区偏移）
static uint32_t s_wpos;            // 段内下一写入字节（从 HDR_AREA 起）
static uint32_t s_werased;         // 已擦到的段内扇区数（不含）
static uint32_t s_seq, s_payload, s_frames;
static uint64_t s_start_ts;
static uint8_t  s_wbuf[WBUF_SIZE];
static size_t   s_wlen;
static uint8_t  s_scan[SEC_SZ];    // 索引重建扫描缓冲（静态，不占任务栈）

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return crc;
}

static uint32_t entry_crc(const idx_entry_t *e)
{
    idx_entry_t t = *e;
    t.crc = 0;
    return crc32_ieee((const uint8_t *)&t, sizeof(t));
}

// ---- 索引读写 ----

static esp_err_t idx_flush(void)
{
    esp_err_t e = esp_partition_erase_range(s_part, 0, SEC_SZ);
    if (e != ESP_OK) return e;
    return esp_partition_write(s_part, 0, s_idx, sizeof(s_idx));
}

// 数据区从 addr 起的扇区是否全部空闲（无"完成/写中"段占用）
static bool range_free(uint32_t addr, uint32_t sectors)
{
    uint32_t end = addr + sectors * SEC_SZ;
    if (end > s_data_size) return false;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_idx[i].state == 0) continue;
        uint32_t a = s_idx[i].addr, b = a + s_idx[i].sectors * SEC_SZ;
        if (a < end && addr < b) return false;   // 区间重叠
    }
    return true;
}

// ---- 掉电恢复 ----

static void recover(void)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        idx_entry_t *e = &s_idx[i];
        if (e->magic != IDX_MAGIC || e->crc != entry_crc(e)) {
            memset(e, 0, sizeof(*e));            // 坏条目按空闲
            continue;
        }
        if (e->state == 1) {                     // 写中：查数据区头区终结标记
            frec_hdr_t h;
            if (esp_partition_read(s_part, SEC_SZ + e->addr, &h, sizeof(h)) == ESP_OK
                && h.magic == FREC_MAGIC && h.payload_size != 0 && h.payload_size != 0xFFFFFFFF) {
                e->state = 2;
                e->payload_size = h.payload_size;
                e->frame_count = h.frame_count;
                ESP_LOGW(TAG, "掉电恢复：段 R%07u 补终结（%u 帧）",
                         (unsigned)e->seq, (unsigned)e->frame_count);
            } else {
                ESP_LOGW(TAG, "掉电恢复：段 R%07u 未终结，丢弃",
                         (unsigned)e->seq);
                memset(e, 0, sizeof(*e));
            }
        }
    }
}

// 索引扇区校验失败 → 扫数据区 FREC 魔数重建（段按扇区对齐猜界）
static void rebuild_by_scan(void)
{
    ESP_LOGW(TAG, "索引损坏，按数据区 FREC 魔数重建");
    memset(s_idx, 0, sizeof(s_idx));
    uint32_t off = 0;
    int slot = 0;
    while (off + SEC_SZ <= s_data_size && slot < MAX_ENTRIES) {
        if (esp_partition_read(s_part, SEC_SZ + off, s_scan, HDR_AREA) != ESP_OK) break;
        const frec_hdr_t *h = (const frec_hdr_t *)s_scan;
        if (h->magic == FREC_MAGIC && h->ver == 3 && h->payload_size != 0
            && h->payload_size != 0xFFFFFFFF && h->hdr_size == 48) {
            idx_entry_t *e = &s_idx[slot++];
            e->magic = IDX_MAGIC;
            e->state = 2;
            e->seq = h->seq;
            e->start_ts_ms = h->start_ts_ms;
            e->payload_size = h->payload_size;
            e->frame_count = h->frame_count;
            e->bitrate = h->bitrate;
            e->sample_rate = (uint16_t)h->sample_rate;
            e->frame_ms = h->frame_ms;
            e->addr = off;
            // 占用 = 头区 + 帧流（每帧 2B 前缀）
            uint32_t bytes = HDR_AREA + e->payload_size + e->frame_count * 2;
            e->sectors = (bytes + SEC_SZ - 1) / SEC_SZ;
            off += e->sectors * SEC_SZ;
            ESP_LOGI(TAG, "重建段 R%07u @+%#x（%uB）",
                     (unsigned)e->seq, (unsigned)e->addr, (unsigned)e->payload_size);
        } else {
            off += SEC_SZ;
        }
    }
    idx_flush();
}

// ---- 对外：挂载 ----

esp_err_t frec_store_mount(void)
{
    if (s_mounted) return ESP_OK;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      "recordings");
    if (!s_part) {
        ESP_LOGE(TAG, "recordings 分区不存在（分区表？）");
        return ESP_ERR_NOT_FOUND;
    }
    s_data_size = s_part->size - SEC_SZ;

    // 读索引
    if (esp_partition_read(s_part, 0, s_idx, sizeof(s_idx)) != ESP_OK) {
        return ESP_FAIL;
    }
    bool ok = true;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_idx[i].magic == IDX_MAGIC && s_idx[i].crc != entry_crc(&s_idx[i])) {
            ok = false;
            break;
        }
        if (s_idx[i].magic != IDX_MAGIC && s_idx[i].magic != 0) {
            ok = false;                           // 非 FIDX/非空 → 坏
            break;
        }
    }
    if (!ok) {
        rebuild_by_scan();
    } else {
        recover();
    }

    // 游标：最后一个占用段之后
    s_cursor = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_idx[i].state != 0) {
            uint32_t end = s_idx[i].addr + s_idx[i].sectors * SEC_SZ;
            if (end > s_cursor) s_cursor = end;
        }
    }
    s_mounted = true;
    ESP_LOGI(TAG, "recordings 裸分区挂载：索引 %d 段，游标 +%#x（RAM 0）",
             frec_store_pending(), (unsigned)s_cursor);
    return ESP_OK;
}

bool frec_store_is_mounted(void) { return s_mounted; }

// ---- 段写入 ----

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

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_idx[i].state == 0) return i;
    }
    return -1;
}

esp_err_t frec_seg_begin(uint64_t start_ts_ms, uint32_t *seq_out)
{
    if (!s_mounted || s_wslot >= 0) return ESP_ERR_INVALID_STATE;
    if (nvs_next_seq(&s_seq) != ESP_OK) {
        static uint32_t s_fallback;
        s_fallback += 1;
        s_seq = s_fallback;
        ESP_LOGW(TAG, "NVS seq 失败，退化为内存计数 %u", (unsigned)s_seq);
    }

    // 环上找一块连续空闲区（最多容纳 5 分钟 16kbps ≈ 700KB；先按 176 扇区申请，
    // 不够就环回；段实际增长按需，环满在写路径报）
    uint32_t want = 176 * SEC_SZ;                // ≈ 700KB 预留判定
    uint32_t try_addr = s_cursor;
    for (int wrap = 0; wrap < 2; wrap++) {
        if (try_addr + want <= s_data_size && range_free(try_addr, 176)) {
            goto found;
        }
        try_addr = 0;                             // 环回重试
    }
    // 退化：任何能放下头区的空位也接受（短段优于拒绝）
    for (uint32_t a = 0; a + SEC_SZ <= s_data_size; a += SEC_SZ) {
        if (range_free(a, 1)) { try_addr = a; goto found; }
    }
    ESP_LOGE(TAG, "存储环满（段未上传排空？）");
    return ESP_ERR_NO_MEM;

found:
    s_wslot = find_free_slot();
    if (s_wslot < 0) {
        ESP_LOGE(TAG, "索引槽位耗尽");
        return ESP_ERR_NO_MEM;
    }
    s_waddr = try_addr;
    s_wpos = HDR_AREA;               // 头区预留：帧数据从 64B 之后写
    s_werased = 0;
    s_payload = 0;
    s_frames = 0;
    s_wlen = 0;
    s_start_ts = start_ts_ms;

    idx_entry_t *e = &s_idx[s_wslot];
    memset(e, 0, sizeof(*e));
    e->magic = IDX_MAGIC;
    e->state = 1;
    e->seq = s_seq;
    e->start_ts_ms = start_ts_ms;
    e->addr = s_waddr;
    e->bitrate = 16000;
    e->sample_rate = 16000;
    e->frame_ms = 60;
    e->crc = entry_crc(e);
    idx_flush();

    if (seq_out) *seq_out = s_seq;
    ESP_LOGI(TAG, "段 R%07u 开始 @+%#x（start_ts=%llu ms）",
             (unsigned)s_seq, (unsigned)s_waddr, (unsigned long long)start_ts_ms);
    return ESP_OK;
}

// 写缓冲落盘（先擦后写；按"已擦到的段内扇区数"水印推进擦除）
static esp_err_t wbuf_flush(void)
{
    while (s_wlen > 0) {
        uint32_t sec_idx = s_wpos / SEC_SZ;       // 当前写入位置所在段内扇区
        if (sec_idx >= s_werased) {               // 新扇区：擦
            if (s_waddr + (sec_idx + 1) * SEC_SZ > s_data_size) {
                ESP_LOGE(TAG, "段写越界（环尾），段中止");
                return ESP_ERR_NO_MEM;
            }
            if (esp_partition_erase_range(s_part,
                    SEC_SZ + s_waddr + sec_idx * SEC_SZ, SEC_SZ) != ESP_OK) {
                return ESP_FAIL;
            }
            s_werased = sec_idx + 1;
        }
        size_t chunk = SEC_SZ - (s_wpos % SEC_SZ);
        if (chunk > s_wlen) chunk = s_wlen;
        if (esp_partition_write(s_part, SEC_SZ + s_waddr + s_wpos, s_wbuf, chunk) != ESP_OK) {
            return ESP_FAIL;
        }
        s_wpos += chunk;
        memmove(s_wbuf, s_wbuf + chunk, s_wlen - chunk);
        s_wlen -= chunk;
    }
    return ESP_OK;
}

esp_err_t frec_seg_frame(const uint8_t *opus, uint16_t len)
{
    if (s_wslot < 0 || len == 0) return ESP_ERR_INVALID_STATE;
    uint8_t pre[2] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    if (s_wlen + 2 + len > WBUF_SIZE) {
        if (wbuf_flush() != ESP_OK) return ESP_FAIL;
    }
    if (2 + len <= WBUF_SIZE) {
        memcpy(s_wbuf + s_wlen, pre, 2);
        s_wlen += 2;
        memcpy(s_wbuf + s_wlen, opus, len);
        s_wlen += len;
    } else {
        // 单帧超缓冲（60ms@16kbps≈200B，理论不至）：直接写
        if (wbuf_flush() != ESP_OK) return ESP_FAIL;
        if (esp_partition_write(s_part, SEC_SZ + s_waddr + s_wpos, pre, 2) != ESP_OK
            || esp_partition_write(s_part, SEC_SZ + s_waddr + s_wpos + 2, opus, len) != ESP_OK) {
            return ESP_FAIL;
        }
        s_wpos += 2 + len;
    }
    s_payload += len;
    s_frames += 1;
    return ESP_OK;
}

esp_err_t frec_seg_end(void)
{
    if (s_wslot < 0) return ESP_ERR_INVALID_STATE;
    if (wbuf_flush() != ESP_OK) {
        ESP_LOGE(TAG, "段 R%07u flush 失败", (unsigned)s_seq);
        memset(&s_idx[s_wslot], 0, sizeof(idx_entry_t));
        idx_flush();
        s_wslot = -1;
        return ESP_FAIL;
    }

    // 头区写 FREC 头（seg_begin 预留 64B、从未写过；首扇区已随首帧擦除）
    frec_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = FREC_MAGIC;
    h.ver = 3;
    h.hdr_size = 48;
    h.sample_rate = 16000;
    h.channels = 1;
    h.frame_ms = 60;
    h.payload_size = s_payload;
    h.frame_count = s_frames;
    h.bitrate = 16000;
    h.seq = s_seq;
    h.start_ts_ms = s_start_ts;
    if (s_werased == 0) {                          // 空段：至少擦首扇区再写头
        if (esp_partition_erase_range(s_part, SEC_SZ + s_waddr, SEC_SZ) != ESP_OK) {
            return ESP_FAIL;
        }
        s_werased = 1;
    }
    if (esp_partition_write(s_part, SEC_SZ + s_waddr, &h, sizeof(h)) != ESP_OK) {
        ESP_LOGE(TAG, "段 R%07u 头写入失败", (unsigned)s_seq);
        return ESP_FAIL;
    }

    // 索引置完成
    idx_entry_t *e = &s_idx[s_wslot];
    uint32_t bytes = s_wpos;                       // 已写到的位置（含头区）
    e->state = 2;
    e->payload_size = s_payload;
    e->frame_count = s_frames;
    e->sectors = (bytes + SEC_SZ - 1) / SEC_SZ;
    e->crc = entry_crc(e);
    idx_flush();

    s_cursor = s_waddr + e->sectors * SEC_SZ;
    if (s_cursor >= s_data_size) s_cursor = 0;
    ESP_LOGI(TAG, "段 R%07u 完成：%u 帧 %uB @+%#x",
             (unsigned)s_seq, (unsigned)s_frames, (unsigned)s_payload,
             (unsigned)s_waddr);
    s_wslot = -1;
    return ESP_OK;
}

// ---- 查询/读取/删除 ----

bool frec_store_is_active(const char *fname)
{
    if (s_wslot < 0 || !fname) return false;
    char cur[16];
    snprintf(cur, sizeof(cur), "R%07u.FRC", (unsigned)s_seq);
    return strcmp(cur, fname) == 0;
}

int frec_store_pending(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_idx[i].magic == IDX_MAGIC && s_idx[i].state == 2) n++;
    }
    return n;
}

static int cmp_seq(const void *a, const void *b)
{
    uint32_t x = (*(const idx_entry_t *const *)a)->seq;
    uint32_t y = (*(const idx_entry_t *const *)b)->seq;
    return x < y ? -1 : (x > y ? 1 : 0);
}

bool frec_store_get(int i, frec_seg_info_t *out)
{
    if (!s_mounted || i < 0) return false;
    const idx_entry_t *list[MAX_ENTRIES];
    int n = 0;
    for (int k = 0; k < MAX_ENTRIES; k++) {
        if (s_idx[k].magic == IDX_MAGIC && s_idx[k].state == 2) list[n++] = &s_idx[k];
    }
    if (i >= n) return false;
    qsort(list, n, sizeof(list[0]), cmp_seq);
    const idx_entry_t *e = list[i];
    out->seq = e->seq;
    out->start_ts_ms = e->start_ts_ms;
    out->payload_size = e->payload_size;
    out->frame_count = e->frame_count;
    out->bitrate = e->bitrate;
    out->sample_rate = e->sample_rate;
    out->frame_ms = e->frame_ms;
    return true;
}

esp_err_t frec_store_read(const frec_seg_info_t *seg, uint32_t off,
                          uint8_t *buf, size_t len)
{
    if (!s_mounted || !seg) return ESP_ERR_INVALID_ARG;
    // 段总长 = 头 48B + Σ(2+帧)；读按帧计近似上限保护
    uint32_t total = 48 + seg->payload_size + seg->frame_count * 2;
    if (off + len > total) return ESP_ERR_INVALID_SIZE;
    const idx_entry_t *e = NULL;
    for (int k = 0; k < MAX_ENTRIES; k++) {
        if (s_idx[k].magic == IDX_MAGIC && s_idx[k].state == 2 && s_idx[k].seq == seg->seq) {
            e = &s_idx[k];
            break;
        }
    }
    if (!e) return ESP_ERR_NOT_FOUND;
    return esp_partition_read(s_part, SEC_SZ + e->addr + off, buf, len);
}

esp_err_t frec_store_delete(uint32_t seq)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_idx[i].magic == IDX_MAGIC && s_idx[i].state == 2 && s_idx[i].seq == seq) {
            memset(&s_idx[i], 0, sizeof(idx_entry_t));
            idx_flush();
            ESP_LOGI(TAG, "段 R%07u 已删除（环空间回收）", (unsigned)seq);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
