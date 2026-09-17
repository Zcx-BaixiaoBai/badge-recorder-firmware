// main/frec_store.h —— FREC v3 段存储：裸分区索引式（v2，替代 FAT 版）。
//
// 为什么弃用 FAT（设计变更，实测驱动）：esp_vfs_fat_spiflash_mount_rw_wl 挂载
// 需 >28KB 堆，而 WiFi 连通后空闲堆仅 ~28KB——FAT+Opus+WiFi 在 C3 上装不下。
// 裸分区方案 RAM 成本 ≈ 0（复用录音路径的 4KB 写缓冲），布局自管：
//
//   分区 recordings（0x360000，0x4A0000，4096B 扇区）
//     扇区0        索引：64 × 64B 条目（整扇区原子重写）
//     扇区1..end   数据区：段环形追加，段起始扇区对齐
//   每段布局：[64B 头区][[u16 len][opus] 帧流...]
//   崩溃语义：头区 0xFF = 未终结；FREC 头只在 seg_end 写一次（头区预留、
//   不会与帧区重叠），掉电后据此判定并恢复/丢弃。
//
// 索引条目 state：0=空闲 1=写中 2=完成（可上传）。上传成功置回空闲；
// 数据区游标在空闲空间上环绕；环满则 seg_begin 拒绝（录音中止——背压，
// 设计上 4.625MiB@16kbps ≈ 40 分钟缓冲足够等上传排空）。
//
// FREC 头格式（48B，与 FAT 版/服务端 frec.py 一致）：
//   magic"FREC" ver=3 hdr=48 rate=16000 ch=1 frame_ms=60 payload_size
//   frame_count bitrate seq start_ts_ms(0=未校时，服务端兜底) reserved
//   之后每帧：[u16 len][opus]
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// FREC v3 头（48B，小端，packed）。上传 body 的前 48 字节即此结构。
typedef struct __attribute__((packed)) {
    uint32_t magic;        // "FREC"
    uint16_t ver;
    uint16_t hdr_size;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t frame_ms;
    uint32_t payload_size;   // 终结时回填；上传/恢复按索引值为准
    uint32_t frame_count;
    uint32_t bitrate;
    uint32_t seq;
    uint64_t start_ts_ms;
    uint64_t reserved;
} frec_hdr_t;

// 段摘要（来自索引，上传/查询用；无需读 flash 头）。
typedef struct {
    uint32_t seq;
    uint64_t start_ts_ms;
    uint32_t payload_size;   // 帧数据字节（不含 u16 前缀）
    uint32_t frame_count;
    uint32_t bitrate;
    uint16_t sample_rate;
    uint16_t frame_ms;
} frec_seg_info_t;

// 挂载：找分区 → 读索引（CRC 坏则按数据区 FREC 魔数扫描重建）→ 掉电恢复
// （写中段查头区终结标记）。RAM 成本 ≈ 0。
esp_err_t frec_store_mount(void);

bool frec_store_is_mounted(void);

// fname（"R%07u.FRC" 命名，兼容遗留）是否为当前写中的段。
bool frec_store_is_active(const char *fname);

// ---- 段写入（录音路径，API 与 FAT 版一致） ----

// 开始新段：NVS 分配 seq，头区预留，索引置"写中"。start_ts_ms=0 表示未校时。
esp_err_t frec_seg_begin(uint64_t start_ts_ms, uint32_t *seq_out);

// 追加一帧（4KB 写缓冲攒批；写满扇区时先擦后写，擦在录音任务内完成，
// 8KB 音频流缓冲的 170ms 余量覆盖扇区擦除耗时）。
esp_err_t frec_seg_frame(const uint8_t *opus, uint16_t len);

// 终结当前段：flush 缓冲 → 头区写 FREC 头（一次）→ 索引置"完成"。
esp_err_t frec_seg_end(void);

// ---- 段读取/删除（上传路径，裸分区直读，无文件系统/无路径） ----

// 完成且未删除的段数（排除写中段）。
int frec_store_pending(void);

// 取第 i 个（0 起，按段序）完成段摘要。
bool frec_store_get(int i, frec_seg_info_t *out);

// 从段内偏移 off（0=含 FREC 头）读 len 字节。上传流式取数。
esp_err_t frec_store_read(const frec_seg_info_t *seg, uint32_t off,
                          uint8_t *buf, size_t len);

// 删除段（上传成功后回收环空间；索引置空闲）。
esp_err_t frec_store_delete(uint32_t seq);
