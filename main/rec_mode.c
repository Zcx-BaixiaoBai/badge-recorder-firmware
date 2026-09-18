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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_opus_enc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "recmode";

// ---- 参数 ----
#define CAP_RATE        24000       // ES8311 ADC 时钟要求的采集率（勿改，见 audio_pipe.c）
#define ENC_RATE        16000
// ★ v0.4.35：帧长 20ms→10ms。栈深考古（真机符号化，ELF 已随 CI artifact）：
//   60ms CELT ~14.2KB、20ms SILK 12.4→14.4KB+（随内容浮动）——此端口的
//   opus 栈固有在 14-16KB 级，借还制洞上限 14848B 装不下。10ms 把帧相关
//   局部数组再砍半（固定开销 ~4-5KB + 缩放项 ≈8-9KB），稳进 14.3KB 借栈。
//   代价：帧头 100 帧/s×2B=200B/s（码率 2KB/s 的 10%）。FREC 头自带
//   frame_ms，服务端按头部解析，天然兼容。
#define FRAME_MS        10
#define SAMPLES_IN      (CAP_RATE * FRAME_MS / 1000)     // 240
#define SAMPLES_OUT     (ENC_RATE * FRAME_MS / 1000)     // 160
#define BYTES_IN        (SAMPLES_IN * 2)                 // 480
#define BYTES_OUT       (SAMPLES_OUT * 2)                // 320
#define FRAMES_PER_SEG  (300000 / FRAME_MS)              // 30000 = 5 分钟一段
#define SESSION_FRAMES  (1800 * 1000 / FRAME_MS)         // 180000 = 30 分钟一节
#define ST7789_SLPIN    0x10
#define ST7789_SLPOUT   0x11
#define SYNC_IP_WAIT_SEC  20                              // 同步窗口等 IP
#define SYNC_DRAIN_MS     (4 * 60000)                     // 同步窗口排空上限
#define PEND_GUARD_BYTES  (900 * 1024)                    // 遗留待传超过即拒开新节
                                                //（一节 3.66MB + 遗留 ≤0.9MB < 4.625MiB 环）
#define REC_STACK_BYTES   16384                           // 16KB 常驻 .bss 专栈：silk 链
                                                // 实测 15.9KB + 0.9KB 裕量（见 s_rec_stack 注释）

// 28 抽汉明窗低通（截止 7.2kHz@24k，阻带 8.2k 起 -26dB → 9k -60dB），Q14。
// 生成方式：windowed-sinc，fc=7200/24000，汉明窗，归一化后量化。
static const int16_t s_fir[28] = {
    10, -39, 18, 75, -118, -68, 320, -141, -516, 722, 394, -1929, 1045, 8417,
    8417, 1045, -1929, 394, 722, -516, -141, 320, -68, -118, 75, 18, -39, 10,
};
#define RS_HIST 27                  // 28-1：跨块滤波历史

// ---- 状态 ----
static volatile rec_phase_t s_phase = REC_PHASE_IDLE;
static volatile bool s_run;         // rec_task 会话许可（false=停车场）
static volatile bool s_rec_done;    // rec_task 已收尾（worker 轮询转段用）
static volatile bool s_stop_req;    // 用户请求结束（含同步期：本节后不续录）
static rec_start_mode_t s_mode = REC_MODE_MANUAL;
static bool s_last_had_err;         // 上一节录音期是否出错（续录判定用）
static char s_err[96];
static void *s_enc;
static uint8_t *s_opus_buf;         // 编码输出缓冲
static int s_opus_cap;

// 采集/重采样静态缓冲（不占任务栈）
static uint8_t s_stage[BYTES_IN];   // 攒满一帧 20ms 的 24k PCM
static size_t s_staged;
static int16_t s_hist[RS_HIST];
static int16_t s_filt[SAMPLES_IN];
static int16_t s_pcm16k[SAMPLES_OUT];
static uint32_t s_seg_frames;       // 当前段已编码帧数
static uint32_t s_sess_frames;      // 本节累计帧数（30 分钟上限）

// rec_task 常驻任务 + .bss 静态专栈（v0.4.37 终局方案）：esp_audio_codec
// 的 libopus 是预编译 .a（-O2 无效），silk 链栈深固定 ~15.9KB，deinit 后
// 堆最大洞 14.95KB 结构性装不下（借还制 5 版真机验证均差 ~1KB）。
// -O2 根因修复后开机预算翻出空间（pre-wifi 52.7KB - 16KB 栈 = 36.3KB >
// wifi init 29.5KB，margin 6.8KB；-Og 时代只有 30.9KB 所以 v0.4.32 失败）。
// 任务开机即建、停车场式待命（等 s_run）；会话间不删——栈永不归还，
// 同步窗口 wifi re-init 有 34.5KB 可用（原借还制腾的那 14KB 不再需要）。
static StaticTask_t s_rec_tcb;
static StackType_t s_rec_stack[REC_STACK_BYTES];

// ---- 屏幕时序 ----
// ★ v0.4.30：LVGL API 必须持锁调用（ui_badge 全部走 bsp_lvgl_lock，v0.4.29
//   的 rec_mode 无锁跨任务调 lvgl_port_stop → Load access fault 真机崩溃）。
//   本函数由 worker 在 phase 转 RECORDING 时调用（单一任务拥有屏幕）。

void rec_mode_screen_off(void)
{
    bsp_lvgl_lock(2000);
    bsp_display_backlight(0);
    esp_lcd_panel_disp_on_off(bsp_display_panel(), false);
    esp_lcd_panel_io_tx_param(bsp_display_io(), ST7789_SLPIN, NULL, 0);
    lvgl_port_stop();               // 挂起 LVGL timer（esp_lvgl_port 2.9 公开 API）
    bsp_lvgl_unlock();
}

void rec_mode_screen_wake(void)
{
    bsp_lvgl_lock(2000);
    esp_lcd_panel_io_tx_param(bsp_display_io(), ST7789_SLPOUT, NULL, 0);
    bsp_lvgl_unlock();
    vTaskDelay(pdMS_TO_TICKS(120)); // ST7789 规格：SLPOUT 后须等 120ms 再 DISPON
    bsp_lvgl_lock(2000);
    esp_lcd_panel_disp_on_off(bsp_display_panel(), true);
    lvgl_port_resume();
    bsp_lvgl_unlock();
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

// 常驻录音任务：停车场式待命（等 s_run）→ 录一节 → 收尾 → 回停车场。
// 不 vTaskDelete——.bss 专栈永不归还（v0.4.37，见 s_rec_stack 注释）。
static void rec_task(void *arg)
{
    (void)arg;
    for (;;) {
        // 停车场：等会话开始（start_capture 置 s_run 前已备好采集）
        while (!s_run) vTaskDelay(pdMS_TO_TICKS(50));

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
            s_staged = 0;               // 攒满一帧 10ms

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
        s_rec_done = true;              // worker 轮询发现后转 SYNC
        // 不删任务：回停车场等下一节
    }
}

// ---- 会话编排（v0.4.30：无独立会话任务，worker 轮询驱动）----
// v0.4.29 教训：会话任务本身吃 4KB 且其创建也撞碎片堆；16KB rec 栈在
// 33.7KB 碎片堆上分配失败。改由 worker 调 rec_mode_start / rec_mode_poll /
// rec_mode_finish_session 三个非重入入口推进状态机（worker 单任务天然互斥），
// 录音期堆多 4KB、少一个碎片源。

// 本节启动（rec_mode_start 内部，worker 或 console 上下文）。失败时 s_err 已填。
// rec_task 常驻（rec_mode_init 已建），这里只做段开启+采集启动+放行。
static esp_err_t start_capture(void)
{
    ESP_LOGI(TAG, "一节开始（%s，空闲堆 %u B）",
             s_mode == REC_MODE_AUTO ? "续录" : "手动",
             (unsigned)esp_get_free_heap_size());

    esp_opus_enc_reset(s_enc);               // 复用编码器：状态清零
    resample_reset();
    s_staged = 0;
    s_rec_done = false;

    if (frec_seg_begin(time_now_ms(), NULL) != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "段创建失败（分区满？先同步）");
        return ESP_FAIL;
    }

    if (audio_rec_start() != ESP_OK) {
        s_run = false;                       // 采集失败：任务留在停车场
        frec_seg_end();
        snprintf(s_err, sizeof(s_err), "采集启动失败");
        return ESP_FAIL;
    }
    s_run = true;                            // 放行常驻录音任务

    ESP_LOGI(TAG, "录音中（30 分钟上限，空闲堆 %u B）",
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
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_10_MS,
        // ★ v0.4.35 回 VOIP/SILK：10ms 帧栈深预算 ~8-9KB（见 FRAME_MS 注释），
        //   16kbps SILK 是语音质量正解；此前 CELT(AUDIO) 只是 20ms 时代的
        //   栈逃生门。
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
    // 常驻录音任务（.bss 专栈）：开机即建、停车场待命（-O2 预算内，见
    // s_rec_stack 注释）。会话开始只置 s_run 放行。
    if (!xTaskCreateStatic(rec_task, "rec_task", REC_STACK_BYTES, NULL, 5,
                           s_rec_stack, &s_rec_tcb)) {
        ESP_LOGE(TAG, "常驻录音任务创建失败（理论不可达）");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Opus 编码器开机就绪（状态 ~25KB 永久持有；余堆 %u B）",
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

// 进入一节录音（worker 或 console 上下文，阻塞 ~1s）。成功后 phase=RECORDING，
// worker 在转段时调 rec_mode_screen_off()；屏幕归 worker 单任务拥有。
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
    // ★ v0.4.46：无线电层关断（wifi_sta_stop），不再整体 deinit。原 deinit
    //   是为借 16KB 栈腾堆——栈已 .bss 静态化（v0.4.37），deinit 只剩省电
    //   一个理由；而真机实锤 deinit→reinit 循环会把驱动留在半死状态（deinit
    //   前空闲心跳上传成功、reinit 后 TCP 连接永久静默失败——v0.4.45 R26/
    //   R27 对照）。stop/start 占空比是 v0.4.26 验证过的路径。
    wifi_sta_stop();

    if (start_capture() != ESP_OK) {
        s_last_had_err = true;
        wifi_sta_resume();                    // 失败回滚：网络留给 UI
        return ESP_FAIL;
    }
    s_phase = REC_PHASE_RECORDING;
    return ESP_OK;
}

// worker 轮询（250ms 一次）：录音收尾检测 → 转 SYNC。幂等，IDLE 时无操作。
void rec_mode_poll(void)
{
    if (s_phase != REC_PHASE_RECORDING || !s_rec_done) return;
    s_last_had_err = (s_err[0] != '\0');
    stop_capture();
    s_phase = REC_PHASE_SYNC;
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
    rec_mode_screen_wake();
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
