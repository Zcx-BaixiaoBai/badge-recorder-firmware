// main/rec_mode.c —— 熄屏录音模式实现（M1）。
//
// 链路：audio_pipe 采集 24kHz PCM（每 60ms 取 2880B = 1440 样本）
//   → resample_block() 3:2 到 16kHz（960 样本，28 抽 FIR 抗混叠 + 线性插值）
//   → esp_opus_enc（16kbps/60ms/单声道/VOIP/complexity 0）
//   → frec_seg_frame() 写 FREC v3；每 5000 帧（= 5 分钟）换段。
//
// 屏幕时序（design §4.1，ST7789 无硬件复位脚，只能走命令）：
//   熄：backlight(0) → DISPOFF(0x28) → SLPIN(0x10) → lvgl_port_stop()
//   亮：SLPOUT(0x11) → 等 120ms（ST7789 规格）→ DISPON(0x29)
//        → lvgl_port_resume() → backlight(100)
//   （亮屏时 LVGL 在 DISPON 之后才恢复，避免往睡着的面板刷 SPI。）
//
// 致命错误处理（麦克风超时 / 编码失败 / 写盘失败）：任务记录错误并自行退出，
// 屏幕保持熄灭直到用户长按 OK——rec_mode_stop() 幂等地做亮屏并带回错误文案。
#include "rec_mode.h"

#include "audio_pipe.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "frec_store.h"
#include "rec_upload.h"
#include "time_sync.h"
#include "wifi_sta.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_opus_enc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "recmode";

// ---- 参数 ----
#define CAP_RATE        24000       // ES8311 ADC 时钟要求的采集率（勿改，见 audio_pipe.c）
#define ENC_RATE        16000
#define FRAME_MS        60
#define SAMPLES_IN      (CAP_RATE * FRAME_MS / 1000)     // 1440
#define SAMPLES_OUT     (ENC_RATE * FRAME_MS / 1000)     // 960
#define BYTES_IN        (SAMPLES_IN * 2)                 // 2880
#define BYTES_OUT       (SAMPLES_OUT * 2)                // 1920
#define FRAMES_PER_SEG  (300000 / FRAME_MS)              // 5000 = 5 分钟一段
#define ST7789_SLPIN    0x10
#define ST7789_SLPOUT   0x11

// 28 抽汉明窗低通（截止 7.2kHz@24k，阻带 8.2k 起 -26dB → 9k -60dB），Q14。
// 生成方式：windowed-sinc，fc=7200/24000，汉明窗，归一化后量化。
static const int16_t s_fir[28] = {
    10, -39, 18, 75, -118, -68, 320, -141, -516, 722, 394, -1929, 1045, 8417,
    8417, 1045, -1929, 394, 722, -516, -141, 320, -68, -118, 75, 18, -39, 10,
};
#define RS_HIST 27                  // 28-1：跨块滤波历史

// ---- 状态 ----
static volatile bool s_active;
static volatile bool s_run;
static SemaphoreHandle_t s_done;
static char s_err[96];
static void *s_enc;
static uint8_t *s_opus_buf;         // 编码输出缓冲
static int s_opus_cap;

// 采集/重采样静态缓冲（不占任务栈）
static uint8_t s_stage[BYTES_IN];   // 攒满 60ms 的 24k PCM
static size_t s_staged;
static int16_t s_hist[RS_HIST];
static int16_t s_filt[SAMPLES_IN];
static int16_t s_pcm16k[SAMPLES_OUT];
static uint32_t s_seg_frames;       // 当前段已编码帧数

// ---- 屏幕时序 ----

static void screen_off(void)
{
    bsp_display_backlight(0);
    esp_lcd_panel_disp_on_off(bsp_display_panel(), false);
    esp_lcd_panel_io_tx_param(bsp_display_io(), ST7789_SLPIN, NULL, 0);
    lvgl_port_stop();               // 挂起 LVGL timer（esp_lvgl_port 2.9 公开 API）
}

static void screen_on(void)
{
    esp_lcd_panel_io_tx_param(bsp_display_io(), ST7789_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120)); // ST7789 规格：SLPOUT 后须等 120ms 再 DISPON
    esp_lcd_panel_disp_on_off(bsp_display_panel(), true);
    lvgl_port_resume();
    bsp_display_backlight(100);
}

// ---- 3:2 重采样（24k → 16k） ----

static void resample_reset(void)
{
    memset(s_hist, 0, sizeof(s_hist));
}

// 输入 1440 个 24k 样本 → 输出 960 个 16k 样本。
static void resample_block(const int16_t *x, int16_t *out)
{
    for (int i = 0; i < SAMPLES_IN; i++) {
        int64_t acc = 0;
        for (int k = 0; k < 28; k++) {
            int idx = i + k;
            int16_t s = (idx < RS_HIST) ? s_hist[idx] : x[idx - RS_HIST];
            acc += (int64_t)s_fir[k] * s;
        }
        s_filt[i] = (int16_t)(acc >> 14);
    }
    memcpy(s_hist, x + SAMPLES_IN - RS_HIST, RS_HIST * sizeof(int16_t));
    // 3:2 线性插值：t = 1.5j。t2 = 3j（定点），i = t2/2，半差在 t2 为奇时取。
    for (int j = 0; j < SAMPLES_OUT; j++) {
        int t2 = j * 3;
        int i = t2 >> 1;
        int32_t v = s_filt[i];
        if (t2 & 1) v += (s_filt[i + 1] - s_filt[i]) / 2;
        out[j] = (int16_t)v;
    }
}

// ---- 录音任务 ----

static void fatal(const char *why)
{
    snprintf(s_err, sizeof(s_err), "%s", why);
    ESP_LOGE(TAG, "录音中止：%s", why);
    s_run = false;
}

static void rec_task(void *arg)
{
    (void)arg;
    s_seg_frames = 0;
    for (;;) {
        if (!s_run) break;          // 正常停止：排空后退出
        size_t got = 0;
        esp_err_t e = audio_rec_read(s_stage + s_staged, BYTES_IN - s_staged,
                                     &got, 2000);
        if (e == ESP_ERR_TIMEOUT) { fatal("麦克风无数据（超时）"); break; }
        if (e != ESP_OK)            { fatal("采集失败"); break; }
        if (got == 0) break;        // 已停止且排空
        s_staged += got;
        if (s_staged < BYTES_IN) continue;
        s_staged = 0;               // 攒满一帧 60ms

        resample_block((const int16_t *)s_stage, s_pcm16k);

        esp_audio_enc_in_frame_t in = {
            .buffer = (uint8_t *)s_pcm16k, .len = BYTES_OUT,
        };
        esp_audio_enc_out_frame_t outf = {
            .buffer = s_opus_buf, .len = (uint32_t)s_opus_cap,
        };
        esp_audio_err_t ae = esp_opus_enc_process(s_enc, &in, &outf);
        if (ae != ESP_AUDIO_ERR_OK) { fatal("Opus 编码失败"); break; }
        if (outf.encoded_bytes > 0) {
            if (outf.encoded_bytes > 0xFFFF) { fatal("帧超长"); break; }
            if (frec_seg_frame(s_opus_buf, (uint16_t)outf.encoded_bytes) != ESP_OK) {
                fatal("写存储失败（分区满？）");
                break;
            }
            s_seg_frames += 1;
        }
        if (s_seg_frames >= FRAMES_PER_SEG) {     // 5 分钟换段
            if (frec_seg_end() == ESP_OK) {
                // 功耗实测数据源（M0）：换段时打电量/电压/空闲堆，COM10 日志采集
                ESP_LOGI(TAG, "段完成：电量 %d%% (%dmV)，空闲堆 %u B",
                         bsp_battery_soc(), bsp_battery_mv(),
                         (unsigned)esp_get_free_heap_size());
                frec_seg_begin(time_now_ms(), NULL);
                s_seg_frames = 0;
                rec_upload_kick();                // 段已终结：通知上传任务（无线电占空比）
            } else {
                fatal("换段失败");
                break;
            }
        }
    }

    // 收尾：终结当前段、通知上传排空。采集停止由 rec_mode_stop() 触发（见上）。
    if (s_seg_frames > 0) {
        if (frec_seg_end() != ESP_OK) {
            ESP_LOGE(TAG, "终结段失败（掉电恢复会按帧扫描兜底）");
        }
    } else {
        frec_seg_end();             // 空段也正常关闭（头部 payload=0，服务端忽略）
    }
    rec_upload_kick();              // 最后一段：上传任务会在无线电可用时排空
    // 编码器与输出缓冲永久持有（开机分配防碎片化），会话结束只 reset
    esp_opus_enc_reset(s_enc);
    s_active = false;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// ---- 对外 API ----

bool rec_mode_active(void) { return s_active; }

const char *rec_mode_last_error(void) { return s_err; }

esp_err_t rec_mode_init(void)
{
    if (s_enc) return ESP_OK;                 // 幂等

    esp_opus_enc_config_t cfg = {
        .sample_rate      = ENC_RATE,
        .channel          = 1,
        .bits_per_sample  = 16,
        .bitrate          = 16000,
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity       = 0,
        .enable_fec       = false,
        .enable_dtx       = false,
        .enable_vbr       = false,
    };
    esp_opus_enc_register();
    esp_audio_err_t oe = esp_opus_enc_open(&cfg, sizeof(cfg), &s_enc);
    if (oe != ESP_AUDIO_ERR_OK || !s_enc) {
        s_enc = NULL;
        ESP_LOGE(TAG, "开机预开 Opus 编码器失败 ret=%d（录音不可用）", (int)oe);
        return ESP_FAIL;
    }
    int in_size = 0, out_size = 0;
    esp_opus_enc_get_frame_size(s_enc, &in_size, &out_size);
    if (in_size != BYTES_OUT) {
        ESP_LOGW(TAG, "编码输入帧 %dB ≠ 预期 %dB", in_size, (int)BYTES_OUT);
    }
    s_opus_cap = out_size > 2048 ? out_size : 2048;
    // 输出缓冲也开机分配：避免运行期碎片化下 malloc 失败
    s_opus_buf = malloc((size_t)s_opus_cap);
    if (!s_opus_buf) {
        esp_opus_enc_close(s_enc);
        s_enc = NULL;
        ESP_LOGE(TAG, "开机分配编码输出缓冲失败（%dB）", s_opus_cap);
        return ESP_ERR_NO_MEM;
    }
    if (!s_done) s_done = xSemaphoreCreateBinary();
    ESP_LOGI(TAG, "Opus 编码器开机就绪（状态 ~25KB 永久持有；余堆 %u B）",
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t rec_mode_start(void)
{
    if (s_active) return ESP_ERR_INVALID_STATE;
    s_err[0] = '\0';
    // 校时由调用方（main.c）在进录音前完成：有网时 time_sync_from_gateway()
    // best-effort 校一次；无网也允许录（段头 start_ts_ms=0，服务端按到达时间兜底）。

    ESP_LOGI(TAG, "录音模式启动（空闲堆 %u B）", (unsigned)esp_get_free_heap_size());
    if (frec_store_mount() != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "recordings 分区挂载失败");
        return ESP_FAIL;
    }

    if (!s_enc || !s_opus_buf) {
        // 编码器未在开机预开（理论上 main 已调 rec_mode_init）：此时堆碎片化，
        // 尝试一次但大概率失败——真正的修复是开机持有（v0.4.17 实测破案）
        if (rec_mode_init() != ESP_OK) {
            snprintf(s_err, sizeof(s_err), "Opus 编码器不可用（须开机预开）");
            return ESP_FAIL;
        }
    }
    esp_opus_enc_reset(s_enc);               // 复用编码器：状态清零

    if (frec_seg_begin(time_now_ms(), NULL) != ESP_OK) {
        // 编码器与输出缓冲永久持有，失败只回滚段，不释放它们
        snprintf(s_err, sizeof(s_err), "段文件创建失败");
        return ESP_FAIL;
    }

    resample_reset();
    s_staged = 0;
    if (audio_rec_start() != ESP_OK) {
        frec_seg_end();
        snprintf(s_err, sizeof(s_err), "采集启动失败");
        return ESP_FAIL;
    }

    s_active = true;
    s_run = true;
    if (xTaskCreate(rec_task, "rec_task", 4096, NULL, 5, NULL) != pdPASS) {
        s_active = false;
        s_run = false;
        audio_rec_cancel();
        frec_seg_end();
        snprintf(s_err, sizeof(s_err), "录音任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    screen_off();
    ESP_LOGI(TAG, "熄屏录音中（长按 OK 结束）");
    return ESP_OK;
}

esp_err_t rec_mode_stop(void)
{
    if (!s_active) {
        screen_on();                // 任务已自行退出（致命错误）：只需亮屏
        wifi_sta_resume();          // 回 UI：恢复无线电（占空比期间是关的）
        rec_upload_kick();
        return ESP_OK;
    }
    s_run = false;
    audio_rec_stop();               // 采集排空后 audio_rec_read 返回 0 → 任务收尾
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGW(TAG, "录音任务 8s 未退出（仍在写盘？）");
    }
    screen_on();
    // 录音结束回 UI：恢复无线电（界面需要网），等连接后排空最后一段；
    // 等不到也无妨——main 心跳的闲时兜底会在连通后重试。
    wifi_sta_resume();
    (void)wifi_wait_connected(15);
    rec_upload_kick();
    ESP_LOGI(TAG, "录音结束%s", s_err[0] ? "（有错误）" : "");
    return ESP_OK;
}
