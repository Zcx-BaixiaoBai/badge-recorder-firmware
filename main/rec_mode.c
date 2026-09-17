// main/rec_mode.c —— 会话制熄屏录音实现（v2 架构）。
//
// 会话宿主任务（rec_sess）持有完整循环（rec_mode.h 的状态机）：
//   录音（≤30 分钟，WiFi 已 deinit，堆独占）→ 同步（WiFi 重建→排空）→
//   [续录模式且排空成功且无错 → 回到录音]。
// 每节 30000 帧 @60ms = 3.66MB@16kbps（分区 4.625MiB 的 78%），6 段 × 5 分钟。
//
// 链路（与 v1 相同）：audio_pipe 采集 24kHz PCM（每 60ms 取 2880B = 1440 样本）
//   → resample_block() 3:2 到 16kHz（960 样本，28 抽 FIR 抗混叠 + 线性插值）
//   → esp_opus_enc（16kbps/60ms/单声道/VOIP/complexity 0，开机持有）
//   → frec_seg_frame() 写 FREC v3；每 5000 帧（= 5 分钟）换段。
//
// 屏幕时序（design §4.1，ST7789 无硬件复位脚，只能走命令）：
//   熄：backlight(0) → DISPOFF(0x28) → SLPIN(0x10) → lvgl_port_stop()
//   亮：SLPOUT(0x11) → 等 120ms（ST7789 规格）→ DISPON(0x29)
//        → lvgl_port_resume() → backlight(100)
//   续录循环期间保持熄屏；会话彻底结束时由宿主任务亮屏，worker 随后刷 UI。
//
// 错误处理：致命错误（麦克风超时/编码失败/写盘失败）置 s_err 并停止录音，
// 同步照常尝试（已录数据优先保住）；错误会话绝不自动续录。同步排空失败
// 也停（Flash 环保护：不清空不续录，遗留段由闲时心跳补传）。
#include "rec_mode.h"

#include "audio_pipe.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "frec_store.h"
#include "gw_client.h"
#include "rec_upload.h"
#include "time_sync.h"
#include "wifi_sta.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_opus_enc.h"
#include "esp_system.h"
#include "esp_timer.h"
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
#define SESSION_FRAMES  (1800 * 1000 / FRAME_MS)         // 30000 = 30 分钟一节
#define ST7789_SLPIN    0x10
#define ST7789_SLPOUT   0x11
#define SYNC_IP_WAIT_SEC  20                              // 同步窗口等 IP
#define SYNC_DRAIN_MS     (4 * 60000)                     // 同步窗口排空上限
#define PEND_GUARD_BYTES  (900 * 1024)                    // 遗留待传超过即拒开新节
                                                //（一节 3.66MB + 遗留 ≤0.9MB < 4.625MiB 环）

// 28 抽汉明窗低通（截止 7.2kHz@24k，阻带 8.2k 起 -26dB → 9k -60dB），Q14。
// 生成方式：windowed-sinc，fc=7200/24000，汉明窗，归一化后量化。
static const int16_t s_fir[28] = {
    10, -39, 18, 75, -118, -68, 320, -141, -516, 722, 394, -1929, 1045, 8417,
    8417, 1045, -1929, 394, 722, -516, -141, 320, -68, -118, 75, 18, -39, 10,
};
#define RS_HIST 27                  // 28-1：跨块滤波历史

// ---- 状态 ----
static volatile rec_phase_t s_phase = REC_PHASE_IDLE;
static volatile bool s_run;         // rec_task 循环许可
static volatile bool s_stop_req;    // 用户请求结束（含同步期：本节后不续录）
static rec_start_mode_t s_mode = REC_MODE_MANUAL;
static bool s_last_had_err;         // 上一节录音期是否出错（续录判定用）
static TaskHandle_t s_session;      // 会话宿主任务
static SemaphoreHandle_t s_done;    // rec_task 收尾信号
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
static uint32_t s_sess_frames;      // 本节累计帧数（30 分钟上限）

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
    s_sess_frames = 0;
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
            s_sess_frames += 1;
        }

        if (s_sess_frames >= SESSION_FRAMES) {    // 30 分钟到点：收尾当前段
            ESP_LOGI(TAG, "30 分钟到点（%u 帧，电量 %d%%），自动收尾",
                     (unsigned)s_sess_frames, bsp_battery_soc());
            s_run = false;
            continue;                             // → 循环顶 break → 收尾
        }

        if (s_seg_frames >= FRAMES_PER_SEG) {     // 5 分钟换段
            if (frec_seg_end() == ESP_OK) {
                // 功耗实测数据源（M0）：换段时打电量/电压/空闲堆，COM10 日志采集
                ESP_LOGI(TAG, "段完成：电量 %d%% (%dmV)，空闲堆 %u B",
                         bsp_battery_soc(), bsp_battery_mv(),
                         (unsigned)esp_get_free_heap_size());
                frec_seg_begin(time_now_ms(), NULL);
                s_seg_frames = 0;
                rec_upload_kick();                // 无线电关闭期是空操作，留给同步窗口
            } else {
                fatal("换段失败");
                break;
            }
        }
    }

    // 收尾：终结当前段。采集停止由 stop_capture() / rec_mode_stop() 触发。
    if (s_seg_frames > 0) {
        if (frec_seg_end() != ESP_OK) {
            ESP_LOGE(TAG, "终结段失败（掉电恢复会按帧扫描兜底）");
        }
    } else {
        frec_seg_end();             // 空段也正常关闭（头部 payload=0，服务端忽略）
    }
    // 编码器与输出缓冲永久持有（开机分配防碎片化），会话结束只 reset
    esp_opus_enc_reset(s_enc);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// ---- 会话宿主任务（录音 → 同步 → 续录判定） ----

// 本节启动（会话任务上下文）。失败时 s_err 已填，调用方据此退出。
static esp_err_t start_capture(void)
{
    ESP_LOGI(TAG, "一节开始（%s，空闲堆 %u B）",
             s_mode == REC_MODE_AUTO ? "续录" : "手动",
             (unsigned)esp_get_free_heap_size());

    // ★ 会话制核心：录音前彻底卸载 WiFi 驱动，堆（~34KB）还给录音流水线
    wifi_sta_deinit();

    esp_opus_enc_reset(s_enc);               // 复用编码器：状态清零
    resample_reset();
    s_staged = 0;

    if (frec_seg_begin(time_now_ms(), NULL) != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "段创建失败（分区满？先同步）");
        return ESP_FAIL;
    }
    if (audio_rec_start() != ESP_OK) {
        frec_seg_end();
        snprintf(s_err, sizeof(s_err), "采集启动失败");
        return ESP_FAIL;
    }

    s_run = true;
    // 16KB：libopus 编码实测栈深 ~14KB（v0.4.28 真机 Guru Meditation：Stack
    // protection fault，SP 越过 6KB 栈底再深 0x1FA0）。旧 6KB 从未真正跑过
    // 帧处理——"rec_start OK"只是启动成功。
    if (xTaskCreate(rec_task, "rec_task", 16384, NULL, 5, NULL) != pdPASS) {
        s_run = false;
        audio_rec_cancel();
        frec_seg_end();
        snprintf(s_err, sizeof(s_err), "录音任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "熄屏录音中（30 分钟上限，空闲堆 %u B）",
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

// 本节采集收尾（无论何种结束路径）：确保 cap_task 退出。
static void stop_capture(void)
{
    audio_rec_stop();                        // cap_task 排空退出（一块 64ms）
    vTaskDelay(pdMS_TO_TICKS(150));
    audio_rec_cancel();                      // 兜底（挂死场景由下次 start 解卡）
}

// 遗留待传总量（索引 RAM 数据，无 flash 读）。
static uint32_t pending_bytes(void)
{
    uint32_t total = 0;
    int n = frec_store_pending();
    for (int i = 0; i < n; i++) {
        frec_seg_info_t si;
        if (!frec_store_get(i, &si)) break;
        total += si.payload_size + si.frame_count * 2 + 64;
    }
    return total;
}

// 会话宿主：只负责录音节本身。录完即退出（★ v0.4.29：先释放自身 4KB 栈，
// 同步窗口的堆预算才能到 ~8KB——实测 4.5KB 水位下 lwIP 发包异常、上传断流；
// 8KB≈空闲态水位，上传正常）。同步+续录驱动由 worker 的
// rec_mode_finish_session() 接手（观察 phase==SYNC 且无会话任务时调用）。
static void session_task(void *arg)
{
    (void)arg;
    s_phase = REC_PHASE_RECORDING;
    esp_err_t st = start_capture();
    if (st == ESP_OK) {
        xSemaphoreTake(s_done, portMAX_DELAY);   // rec_task 收尾给出
        s_last_had_err = (s_err[0] != '\0');
    } else {
        s_last_had_err = true;
    }
    stop_capture();
    // 移交：phase=SYNC + 任务自删。worker 看到即驱动同步与续录判定。
    s_phase = REC_PHASE_SYNC;
    s_session = NULL;
    vTaskDelete(NULL);
}

// ---- 对外 API ----

bool rec_mode_active(void) { return s_phase != REC_PHASE_IDLE; }

rec_phase_t rec_mode_phase(void) { return s_phase; }

rec_start_mode_t rec_mode_cur_mode(void) { return s_mode; }

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

esp_err_t rec_mode_start(rec_start_mode_t mode)
{
    if (s_phase != REC_PHASE_IDLE) return ESP_ERR_INVALID_STATE;   // 会话/同步进行中
    s_err[0] = '\0';
    if (!s_enc || !s_opus_buf) {
        snprintf(s_err, sizeof(s_err), "Opus 编码器不可用（须开机预开）");
        return ESP_FAIL;
    }
    if (!frec_store_is_mounted()) {
        snprintf(s_err, sizeof(s_err), "recordings 分区未挂载");
        return ESP_FAIL;
    }
    uint32_t pend = pending_bytes();
    if (pend > PEND_GUARD_BYTES) {
        // Flash 环保护：遗留 + 新一节 3.66MB 可能写穿 4.625MiB 环
        snprintf(s_err, sizeof(s_err), "本机遗留 %uKB 未上传，先连网同步",
                 (unsigned)(pend / 1024));
        return ESP_FAIL;
    }

    s_mode = mode;
    s_stop_req = false;
    s_last_had_err = false;
    s_phase = REC_PHASE_IDLE;                 // 宿主任务起跑后置 RECORDING
    // ★ 顺序关键（v0.4.27 真机教训）：必须先卸载 WiFi 驱动（堆 +~34KB）再创建
    //   会话任务——否则在 WiFi 常开的空闲堆（~10KB）上 4KB 任务栈分配失败，
    //   "会话任务创建失败" 死循环。start_capture 里的 deinit 幂等兜底。
    wifi_sta_deinit();
    // 4KB：本任务不做网络（HTTP 都在 upload 任务），只编排 + 等信号
    if (xTaskCreate(session_task, "rec_sess", 4096, NULL, 4, &s_session) != pdPASS) {
        s_session = NULL;
        snprintf(s_err, sizeof(s_err), "会话任务创建失败");
        wifi_sta_resume();                    // 失败回滚：网络留给 UI
        return ESP_ERR_NO_MEM;
    }
    screen_off();
    return ESP_OK;
}

esp_err_t rec_mode_stop(void)
{
    if (s_phase == REC_PHASE_IDLE) return ESP_OK;   // 无会话：幂等
    s_stop_req = true;                        // 本节完成后不再续录
    if (s_phase == REC_PHASE_RECORDING) {
        s_run = false;                        // rec_task 排空后退出
        audio_rec_stop();                     // 采集排空 → read 返回 0 → 任务收尾
    }
    ESP_LOGI(TAG, "用户结束会话（%s）",
             s_phase == REC_PHASE_SYNC ? "同步后不续录" : "录音排空中");
    return ESP_OK;
}

// 会话收尾驱动（worker 上下文，观察 phase==SYNC 且无会话任务时调用一次）：
// 同步排空 → 续录判定。续录成功则内部已起新节（phase 回 RECORDING，屏幕
// 仍熄）；否则亮屏、恢复网络、回 IDLE（worker 随后刷收尾 UI）。
void rec_mode_finish_session(void)
{
    bool drained = rec_mode_sync_and_flush();
    bool cont = !s_stop_req && !s_last_had_err && s_mode == REC_MODE_AUTO && drained;
    if (cont) {
        ESP_LOGI(TAG, "续录：已排空，进入下一节");
        s_phase = REC_PHASE_IDLE;             // 归位以通过 start 的会话重入守卫
        if (rec_mode_start(s_mode) == ESP_OK) return;   // 起新节（内部置 RECORDING）
        ESP_LOGW(TAG, "续录起新节失败，落到收尾");
    } else if (s_mode == REC_MODE_AUTO && !s_stop_req && !s_last_had_err && !drained) {
        ESP_LOGW(TAG, "排空失败：停续录（Flash 环保护，遗留 %d 段待补传）",
                 frec_store_pending());
    }
    if (!wifi_is_connected() && wifi_sta_configured()) {
        wifi_sta_resume();                   // 启动失败路径：驱动被 deinit 过则内部重建
    }
    screen_on();
    s_phase = REC_PHASE_IDLE;
}

bool rec_mode_sync_and_flush(void)
{
    int pend = frec_store_pending();
    if (pend == 0) {
        ESP_LOGI(TAG, "同步窗口：无待传段");
        return true;
    }
    ESP_LOGI(TAG, "同步窗口：%d 段待传（空闲堆 %u B）",
             pend, (unsigned)esp_get_free_heap_size());
    if (!wifi_sta_configured()) {
        ESP_LOGW(TAG, "未配网，%d 段留本机（配网后闲时补传）", pend);
        return false;
    }
    wifi_sta_start(NULL, NULL);               // 驱动重建（幂等：已在则空操作）
    if (!wifi_wait_connected(SYNC_IP_WAIT_SEC)) {
        ESP_LOGW(TAG, "同步窗口：WiFi %ds 未连上，%d 段留本机",
                 SYNC_IP_WAIT_SEC, pend);
        return false;
    }
    // 校时在 upload 任务内完成（其栈更大；每次重连重校，design §4.4）
    int64_t t0 = esp_timer_get_time();
    int last_left = -1;
    int stall = 0;
    for (;;) {
        rec_upload_kick();
        vTaskDelay(pdMS_TO_TICKS(stall > 0 ? 2000 : 500));
        int left = frec_store_pending();
        if (left == 0) {
            ESP_LOGI(TAG, "同步完成：全部排空（%llus，空闲堆 %u B）",
                     (unsigned long long)((esp_timer_get_time() - t0) / 1000000),
                     (unsigned)esp_get_free_heap_size());
            return true;
        }
        if (left == last_left) stall++;
        else                   stall = 0;
        last_left = left;
        if ((esp_timer_get_time() - t0) / 1000 > SYNC_DRAIN_MS) {
            ESP_LOGW(TAG, "同步超时（%ds）：剩 %d 段留本机",
                     (int)(SYNC_DRAIN_MS / 1000), left);
            return false;
        }
    }
}
