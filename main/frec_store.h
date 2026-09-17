// main/frec_store.h —— FREC v3 段容器：recordings FAT 分区写入 + 掉电恢复。
//
// 容器格式（48 字节头 + 帧流，小端）：
//   off  size  字段
//   0    4     magic "FREC"
//   4    2     ver = 3
//   6    2     hdr_size = 48
//   8    4     sample_rate = 16000
//   12   2     channels = 1
//   14   2     frame_ms = 60          （Opus 帧时长，服务端拼 Ogg 粒度要用）
//   16   4     payload_size           （终结时回填；0=未终结，掉电恢复时按帧扫描）
//   20   4     frame_count            （终结时回填）
//   24   4     bitrate = 16000
//   28   4     seq                    （段序号，NVS 计数器分配，跨重启不重复）
//   32   8     start_ts_ms            （段起始墙钟毫秒；0=未校时，服务端按到达时间兜底）
//   40   8     reserved = 0
//   之后每帧：[u16 len][opus 数据]
//
// 官方录音笔（folo_recorder_c3 v0.6.2）的 FREC v2 头是 32B、无时间戳（FAT 时间
// 1980-01-01 证明它没校时）。v3 补上 start_ts_ms/seq/bitrate/frame_ms 使段自描述、
// 可拼接、可标注缺口（design §4.3），服务端 frec.py 按本格式解出 Opus 帧再拼 Ogg。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// FREC v3 头（48B，小端，packed）。rec_upload 上传时按此解析段文件取元数据。
typedef struct __attribute__((packed)) {
    uint32_t magic;        // "FREC"
    uint16_t ver;
    uint16_t hdr_size;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t frame_ms;
    uint32_t payload_size;
    uint32_t frame_count;
    uint32_t bitrate;
    uint32_t seq;
    uint64_t start_ts_ms;
    uint64_t reserved;
} frec_hdr_t;

// 挂载 recordings 分区（FAT + wear levelling，首次自动格式化），并做掉电恢复
// （扫描未终结的段文件，按帧重算 payload_size/frame_count 回填头部）。
esp_err_t frec_store_mount(void);

bool frec_store_is_mounted(void);

// fname（形如 "R0000001.FRC"）是否是当前正在写的段——上传扫描时排除。
bool frec_store_is_active(const char *fname);

// 开始一个新段：分配 seq（NVS 计数器），写占位头。start_ts_ms=0 表示未校时。
// 成功后可通过 *seq_out 拿到段序号（日志/调试用，可为 NULL）。
esp_err_t frec_seg_begin(uint64_t start_ts_ms, uint32_t *seq_out);

// 追加一帧 Opus（内部 4KB 写缓冲，攒满才落盘，减少 Flash 小写次数）。
esp_err_t frec_seg_frame(const uint8_t *opus, uint16_t len);

// 终结当前段：flush 缓冲、回填头部统计、关闭并 sync。之后可再 begin 下一段。
esp_err_t frec_seg_end(void);
