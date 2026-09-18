// main/main.c —— 工牌录音固件
//
// 产品：静默录音工牌（会话制 30 分钟/节 + 同步上传 + 服务器分析出纪要/日报）。
// 主界面：录音仪表盘（状态/电量/待传段数）。
// 设置菜单：配网（二次确认）/ 测试网关 / 录音（手动/连续）/ 配网指导 / 关于。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "ui_badge.h"
#include "wifi_sta.h"
#include "gw_client.h"
#include "audio_pipe.h"
#include "badge_cfg.h"
#include "badge_console.h"
#include "frec_store.h"
#include "provision.h"
#include "rec_mode.h"
#include "rec_upload.h"
#include "time_sync.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "badge";

typedef enum { LVL_WS = 0, LVL_SESS, LVL_DETAIL, LVL_SETTINGS, LVL_SUBPAGE } level_t;
typedef enum { EV_UP = 0, EV_DOWN, EV_OK, EV_OKLONG, EV_UPLONG, EV_DOWNLONG } badge_ev_t;

#define REC_MAX_SEC   15
#define REC_MAX_BYTES (REC_MAX_SEC * 48000)   // 24kHz*16bit*mono = 48000 B/s
#define REC_MIN_BYTES 28800                   // 0.6s：录满前忽略停止（服务器要求≥rate字节=24000）

static QueueHandle_t s_evq;
static level_t s_level = LVL_WS;
static gw_ws_t   s_ws[GW_MAX_WS];
static int s_ws_n;
static gw_sess_t s_sess[32];
static int s_sess_n;
static char s_cur_task[GW_SESS_ID];
static char s_cur_title[GW_SESS_TITLE];
static int  s_ws_sel;                // 进入 L2 时记住的 L1 选中项（返回列表后 sel 会变）
static badge_cfg_t g_cfg;            // 运行期配置（NVS 持久化，发布 bin 不内嵌凭据）
static bool s_onboarding;            // 首次使用（无 WiFi 配置）引导态
static bool s_provision_confirm;    // 配网确认子页（防误触直接覆盖配置）

// PTT 状态机：0=空闲 1=录音中 2=已停止录音、等网关处理（此阶段按键不可打断）
static volatile int  s_ptt;
static volatile bool s_rec_stop;     // OK 再按：正常结束录音
static volatile bool s_rec_cancel;   // 长按 OK：取消
static int64_t s_last_oklong_us;     // 防 LONG 后松手的 CLICK 误触发

// ---------- PTT 音频源（gw_ask_voice 的回调，worker 任务内执行） ----------

typedef struct { uint32_t total; bool stopping; } ptt_ctx_t;

static esp_err_t ptt_src(uint8_t *buf, size_t want, size_t *got, void *ctx)
{
    ptt_ctx_t *c = (ptt_ctx_t *)ctx;
    if (s_rec_cancel) { audio_rec_cancel(); return ESP_ERR_INVALID_STATE; }
    if (!c->stopping &&
        ((s_rec_stop && c->total >= REC_MIN_BYTES) || c->total >= REC_MAX_BYTES)) {
        c->stopping = true;
        audio_rec_stop();                    // 采集任务收完当前块后退出，余量仍可排空
        if (s_ptt == 1) {
            s_ptt = 2;                       // 进入"等网关"阶段
            ui_set_state("思考中… 等待回复");
        }
    }
    esp_err_t err = audio_rec_read(buf, want, got, 2500);
    if (err == ESP_OK) {
        uint32_t prev = c->total;
        c->total += *got;
        if (c->total / 48000 != prev / 48000)   // 每满 1 秒报一次（24kHz=48000B/s）
            ESP_LOGI(TAG, "录音 %lus (%lu B)",
                     (unsigned long)(c->total / 48000), (unsigned long)c->total);
    } else if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "mic 2.5s 无数据，判定挂起");
        audio_rec_cancel();
    } else {
        ESP_LOGE(TAG, "采集失败: %s", esp_err_to_name(err));
        audio_rec_cancel();
    }
    return err;
}

static esp_err_t play_chunk(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    return audio_play_feed(data, len);
}

// ---------- 界面构建 ----------

static void build_ws_list(void)
{
    ui_row_t rows[GW_MAX_WS];
    char notes[GW_MAX_WS][24];
    for (int i = 0; i < s_ws_n; i++) {
        snprintf(notes[i], sizeof(notes[i]), "%d/%d", s_ws[i].running, s_ws[i].total);
        rows[i].main = s_ws[i].label;
        rows[i].right = notes[i];
    }
    ui_set_header("远控工作区");
    ui_show_list(rows, s_ws_n, ui_list_sel() < s_ws_n ? ui_list_sel() : 0);
    ui_set_hint("上下选择  OK进入");
}

static void build_sess_list(void)
{
    static ui_row_t rows[32];
    static char notes[32][24];
    int n = s_sess_n > 32 ? 32 : s_sess_n;
    for (int i = 0; i < n; i++) {
        const char *st = s_sess[i].status[0] ? s_sess[i].status : "-";
        snprintf(notes[i], sizeof(notes[i]), "%s",
                 strcmp(st, "running") == 0 ? "运行中" :
                 strncmp(st, "completed", 9) == 0 ? "已完成" : st);
        rows[i].main = s_sess[i].title;
        rows[i].right = notes[i];
    }
    char hdr[GW_WS_LABEL + 16];
    snprintf(hdr, sizeof(hdr), "会话：%s",
             (s_ws_sel >= 0 && s_ws_sel < s_ws_n) ? s_ws[s_ws_sel].label : "");
    ui_set_header(hdr);
    ui_show_list(rows, n, 0);
    ui_set_hint("上下选择  OK进入  长按OK返回");
}

static void enter_detail(int idx)
{
    snprintf(s_cur_task, sizeof(s_cur_task), "%s", s_sess[idx].task_id);
    snprintf(s_cur_title, sizeof(s_cur_title), "%s", s_sess[idx].title);
    ui_set_header("会话");
    ui_show_detail(s_cur_title);
    ui_set_hint("OK说话 上下滚动 长按OK返回");
    ui_set_state("加载历史记录…");
    static char hist[3072];
    if (gw_fetch_history(s_cur_task, hist, sizeof(hist)) == ESP_OK && hist[0]) {
        ui_set_answer(hist);
    }
    ui_set_state("就绪，短按 OK 说话");
}

// ---------- 录音问答 ----------

static void do_ask(void)
{
    if (!s_cur_task[0]) return;
    s_rec_stop = false;
    s_rec_cancel = false;
    s_ptt = 1;
    // 置位后再清队列：连按 OK 的第二次点击若已入队，会被清掉，
    // 否则本次问答结束后它会误触发第二次录音（用户视角="按 OK 结束不了"）。
    // s_ptt=1 之后的新按键走标志路径，不再入队，无竞态窗口。
    badge_ev_t junk;
    while (xQueueReceive(s_evq, &junk, 0) == pdTRUE) {}
    ESP_LOGI(TAG, "ptt 开始录音 task=%s", s_cur_task);
    ui_set_state("聆听中… 说完按 OK 发送");
    esp_err_t rerr = audio_rec_start();
    if (rerr != ESP_OK) {
        s_ptt = 0;
        ESP_LOGE(TAG, "麦克风启动失败: %s", esp_err_to_name(rerr));
        ui_set_error("麦克风启动失败");
        ui_set_hint("OK重试  长按OK返回");
        return;
    }

    ptt_ctx_t ctx = { 0 };
    static gw_ask_t res;                 // ~1.5KB，放静态区省任务栈
    esp_err_t err = gw_ask_voice(s_cur_task, ptt_src, &ctx, &res);
    s_ptt = 0;
    audio_rec_cancel();                  // 兜底：确保采集任务退出（正常路径已 stop 退出）
    ESP_LOGI(TAG, "ptt done: total=%u B err=%s cancel=%d ok=%d",
             (unsigned)ctx.total, esp_err_to_name(err),
             (int)s_rec_cancel, (int)res.ok);

    if (s_rec_cancel) {
        ui_set_state("已取消，短按 OK 重新说话");
        return;
    }
    if (err != ESP_OK || !res.ok) {
        const char *why = res.error[0] ? res.error :
                          (ctx.total < 3200 ? "录音太短，没听清" : "网关无响应");
        ui_set_error(why);
        ui_set_hint("OK重试  长按OK返回");
        return;
    }

    char qline[288];
    snprintf(qline, sizeof(qline), "你问：%s", res.question);
    ui_set_answer(res.answer[0] ? res.answer : qline);
    ESP_LOGI(TAG, "ask ok: Q=%s A=%.60s audio=%s", res.question, res.answer, res.audio_id);

    if (res.audio_id[0]) {
        if (g_cfg.mute) {
            ui_set_state("已静音，仅显示文本");
        } else {
            ui_set_state("播报中…");
            audio_play_begin();
            if (gw_fetch_audio(res.audio_id, play_chunk, NULL) != ESP_OK) {
                ESP_LOGW(TAG, "回复语音获取失败（文本已显示）");
            }
        }
    }
    ui_set_state("完成，短按 OK 继续提问");
}

// ---------- worker ----------

// 主界面：录音仪表盘（v0.4.51：无回复框——纯录音产品）
// 颜色值对齐 ui_badge.c 调色板
#define HOME_C_OK     0x4ADE80
#define HOME_C_WARN   0xFBBF24
#define HOME_C_ACCENT 0x2F81F7

static void refresh_home(void)
{
    s_level = LVL_WS;                   // 复用 LVL_WS 层级：上下滚动 + OK进设置
    int pend = rec_upload_pending();
    int soc = bsp_battery_soc();
    int mv = bsp_battery_mv();
    rec_phase_t ph = rec_mode_phase();
    ui_show_home();
    if (ph == REC_PHASE_SYNC) {
        ui_set_home("同步中…", HOME_C_ACCENT, soc, mv, pend);
        ui_set_hint("正在上传录音，请稍候…");
    } else if (ph == REC_PHASE_RECORDING) {
        ui_set_home("录音中", HOME_C_ACCENT, soc, mv, pend);
        ui_set_hint("长按OK 结束录音");
    } else if (pend > 0) {
        ui_set_home("待传", HOME_C_WARN, soc, mv, pend);
        ui_set_hint("连网后自动补传");
    } else {
        ui_set_home("就绪", HOME_C_OK, soc, mv, pend);
        ui_set_hint("长按上键单次录音 长按下键连续录音 长按OK设置");
    }
    ui_set_battery(soc);
}

static void refresh_lists(void)
{
    refresh_home();   // v0.4.49：录音产品不再拉工作区列表
}

static void enter_workspace(void)
{
    int sel = ui_list_sel();
    if (sel < 0 || sel >= s_ws_n) return;
    s_ws_sel = sel;
    char busy[96];
    snprintf(busy, sizeof(busy), "加载 %s 的会话…", s_ws[sel].label);
    ui_set_busy(busy);
    esp_err_t err = gw_fetch_sessions(s_ws[sel].path, s_sess, 32, &s_sess_n);
    ui_set_busy(NULL);
    if (err != ESP_OK) {
        ui_set_error("会话列表获取失败");
        return;
    }
    if (s_sess_n == 0) {
        ui_show_detail(s_ws[sel].label);
        ui_set_state("该工作区暂无会话");
        ui_set_hint("长按OK返回");
        s_level = LVL_SESS;   // 仍可按返回键回 L1
        return;
    }
    s_level = LVL_SESS;
    build_sess_list();
}

// ---------- 设置 / 首次引导 ----------

static const char *GUIDE_TEXT =
    "【这是什么】静默录音工牌：佩戴后录下你一天的\n"
    "对话和会议，自动上传服务器分析——每日生成会议\n"
    "纪要、工作日报、说话人识别、全文搜索。\n"
    "\n"
    "【怎么用】\n"
    "· 长按上键 = 单次录音（30 分钟一节）\n"
    "· 长按下键 = 自动连续录音（每 30 分钟一节，\n"
    "  同步后自动续录。注意：每节间有约 1 分钟\n"
    "  同步间隔，期间不录音）\n"
    "· 长按 OK = 结束录音 / 进设置\n"
    "· 录音中短按任意键 = 亮屏看状态\n"
    "\n"
    "【配网步骤】\n"
    "1 长按 OK → 设置 → 配网设置 → 长按 OK 确认。\n"
    "2 手机连热点 Badge-Recorder-Setup，\n"
    "  浏览器开 http://192.168.4.1。\n"
    "3 填 WiFi(2.4G)、服务器地址、令牌，保存重启。\n"
    "\n"
    "【服务器】recorder-server（部署教程见仓库）：\n"
    "github.com/Zcx-BaixiaoBai/badge-recorder-firmware\n"
    "地址 = 服务器主机 IP + :8787。\n"
    "（本页可上下键滚动）";

static void show_onboarding(void)
{
    s_level = LVL_WS;
    s_onboarding = true;
    ui_set_header("工牌录音");
    ui_show_detail("欢迎使用");
    ui_set_state("还未配置网络，请按引导操作");
    ui_set_answer(GUIDE_TEXT);
    ui_set_hint("长按OK进设置去配网");
}

static void build_settings(void)
{
    static char mute_txt[24];
    snprintf(mute_txt, sizeof(mute_txt), "静音：%s", g_cfg.mute ? "开" : "关");
    ui_row_t rows[8];
    rows[0].main = "配网设置 (WiFi+网关)"; rows[0].right = NULL;
    rows[1].main = mute_txt;              rows[1].right = NULL;
    rows[2].main = "测试网关连接";         rows[2].right = NULL;
    rows[3].main = "录音（30分钟一节）";   rows[3].right = NULL;
    rows[4].main = "连续录音（自动续录）"; rows[4].right = NULL;
    rows[5].main = "配网指导";             rows[5].right = NULL;
    rows[6].main = "关于";                rows[6].right = NULL;
    rows[7].main = "返回";                rows[7].right = NULL;
    ui_set_header("设置");
    ui_show_list(rows, 8, 0);
    ui_set_hint("上下选择 OK执行 长按OK返回");
    s_level = LVL_SETTINGS;
}

static void back_to_main(void)
{
    if (s_onboarding && !g_cfg.ssid[0]) {
        show_onboarding();
    } else {
        s_onboarding = false;
        refresh_lists();
    }
}

// ---------- 会话制录音（v0.4.27）：启动入口 + 收尾 UI ----------

// 进入一节录音（worker 上下文，阻塞 ~1s）。有网先校时（无网也允许录，段头
// start_ts=0 由服务端兜底）。成功：phase=RECORDING，worker 下轮转段时熄屏，
// 后续由 rec_mode_poll / rec_mode_finish_session 驱动。
static bool start_rec_session(rec_start_mode_t mode)
{
    if (wifi_is_connected()) time_sync_from_gateway(g_cfg.gw_url);
    if (rec_mode_start(mode) != ESP_OK) {
        ui_set_error(rec_mode_last_error());
        vTaskDelay(pdMS_TO_TICKS(1500));
        build_settings();
        return false;
    }
    return true;
}

// 会话结束收尾 UI（worker 检测 phase 回到 IDLE 时调用；宿主任务已亮屏）。
static void session_end_ui(void)
{
    const char *err = rec_mode_last_error();
    int pend = rec_upload_pending();
    s_level = LVL_SUBPAGE;
    ui_set_header("录音");
    if (err[0]) {
        ui_show_detail("录音异常结束");
        ui_set_error(err);
        ui_set_state(pend > 0 ? "已录部分留本机，连网后自动补传" : "已收尾");
    } else if (pend > 0) {
        // 排空失败（网不可用）：续录已停（Flash 环保护），遗留段连网后自动补传
        ui_show_detail("同步未完成");
        ui_set_error("网络不可用");
        ui_set_state("已停续录（存储保护），连网后自动补传");
    } else {
        ui_show_detail("同步完成");
        ui_set_state("可再次录音");
    }
    ui_set_hint("长按OK返回设置");
}

static void settings_activate(int sel)
{
    s_provision_confirm = false;             // 进任何设置项先清确认态
    switch (sel) {
    case 0:   // 配网：先确认（v0.4.50：防误触直接覆盖配置）
        s_level = LVL_SUBPAGE;
        s_provision_confirm = true;
        ui_set_header("配网确认");
        ui_show_detail("配网将覆盖当前设置");
        ui_set_state("长按OK确认进入配网");
        ui_set_hint("短按OK取消  长按OK确认");
        break;
    case 1:   // 静音开关
        g_cfg.mute = !g_cfg.mute;
        cfg_save(&g_cfg);
        ui_set_mute(g_cfg.mute);
        build_settings();
        break;
    case 2: { // 测试网关连接（v0.4.50：用 /health 测，recorder-server 无 /ui/workspaces）
        s_level = LVL_SUBPAGE;
        ui_set_header("测试网关");
        ui_show_detail("测试网关连接");
        ui_set_state("连接中…");
        ui_set_answer(g_cfg.gw_url[0] ? g_cfg.gw_url : "（未配置网关地址）");
        if (time_sync_from_gateway(g_cfg.gw_url)) {
            ui_set_state("网关可达，已校时");
        } else {
            ui_set_error("网关不可达，检查地址/令牌/WiFi");
        }
        ui_set_hint("长按OK返回设置");
        break;
    }
    case 3:   // 录音（手动 30 分钟一节）：录满 → 同步 → 提示"可再次录音"
    case 4:   // 连续录音（自动续录）：录满 → 同步排空 → 自动续录
        start_rec_session(sel == 3 ? REC_MODE_MANUAL : REC_MODE_AUTO);
        break;
    case 5:   // 配网指导
        s_level = LVL_SUBPAGE;
        ui_set_header("配网指导");
        ui_show_detail("配网指导");
        ui_set_state("按步骤操作");
        ui_set_answer(GUIDE_TEXT);
        ui_set_hint("长按OK返回设置");
        break;
    case 6:   // 关于
        s_level = LVL_SUBPAGE;
        ui_set_header("关于");
        ui_show_detail("关于");
        ui_set_state("工牌录音固件 v0.4.49");
        ui_set_answer("会话制录音（30分钟一节）+ 同步上传 + 每日纪要。\n服务端：recorder-server\n（badge-recorder-firmware 仓库）。");
        ui_set_hint("长按OK返回设置");
        break;
    default:  // 返回
        back_to_main();
        break;
    }
}

static void badge_worker(void *arg)
{
    if (g_cfg.ssid[0]) {
        wifi_sta_start(g_cfg.ssid, g_cfg.pass);
        ui_set_busy("连接 WiFi…");
        if (!wifi_wait_connected(30)) {
            ui_set_busy(NULL);
            ui_set_header("工牌录音");
            ui_show_detail("WiFi 连不上");
            ui_set_error("检查 WiFi 名称/密码");
            ui_set_hint("长按OK进设置");
            s_level = LVL_WS;
        } else {
            ui_set_busy(NULL);
            time_sync_from_gateway(g_cfg.gw_url);   // 连上即校时（/health server_time_ms）
            // WiFi 已连、堆最低点已过后挂载 recordings（实测：开机挂载会吃掉
            // esp_wifi_init 所需的堆 → boot loop。挂载开销以 heap 日志为准）
            ESP_LOGI(TAG, "挂载前空闲堆 %u B（最大连续块 %u B）",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            if (frec_store_mount() != ESP_OK) {
                ESP_LOGW(TAG, "recordings 分区挂载失败（录音不可用，其余功能不受影响）");
            } else {
                ESP_LOGI(TAG, "挂载后空闲堆 %u B", (unsigned)esp_get_free_heap_size());
            }
            refresh_lists();
        }
    } else {
        show_onboarding();   // 无凭据：不拿空凭据硬连，显示首次引导
    }

    badge_ev_t ev;
    rec_phase_t prev_ph = REC_PHASE_IDLE;
    static bool s_finishing;                 // finish 重入保护（worker 单任务本无，防御）
    static int64_t s_wake_us;                // >0 = 屏幕已唤醒（录音中按键触发）
    for (;;) {
        // 会话阶段监视（v0.4.30 worker 驱动）：IDLE 之外 250ms 快轮询。
        rec_mode_poll();
        rec_phase_t ph = rec_mode_phase();
        if (ph == REC_PHASE_RECORDING && prev_ph == REC_PHASE_IDLE) {
            rec_mode_screen_off();           // 屏幕归 worker 单任务拥有（LVGL 锁内）
            s_wake_us = 0;
        }
        // 录音中屏幕唤醒 10s 无操作自动熄（省电）
        if (s_wake_us > 0 && esp_timer_get_time() - s_wake_us > 10000000) {
            if (rec_mode_phase() == REC_PHASE_RECORDING) rec_mode_screen_off();
            s_wake_us = 0;
        }
        if (ph == REC_PHASE_SYNC && !s_finishing) {
            s_finishing = true;
            rec_mode_finish_session();       // 阻塞至排空/超时；续录则内部起新节
            s_finishing = false;
            s_wake_us = 0;
            ph = rec_mode_phase();
        }
        if (ph == REC_PHASE_IDLE && prev_ph != REC_PHASE_IDLE) session_end_ui();
        prev_ph = ph;

        if (xQueueReceive(s_evq, &ev, pdMS_TO_TICKS(ph == REC_PHASE_IDLE ? 15000 : 250))
                == pdFALSE) {
            if (ph == REC_PHASE_IDLE) {
                ui_set_battery(bsp_battery_soc());   // 心跳：电量刷新（会话中跳过）
                static int64_t s_last_idle_upload_us;
                if (esp_timer_get_time() - s_last_idle_upload_us > 60000000) {
                    s_last_idle_upload_us = esp_timer_get_time();
                    if (wifi_is_connected() && rec_upload_pending() > 0) {
                        rec_upload_kick();
                    }
                }
                if (s_level == LVL_WS) refresh_home();
            }
            continue;
        }
        if (ph != REC_PHASE_IDLE) {
            // 会话中（录音/同步）：长按 OK = 结束；任意短按 = 唤醒屏幕看状态。
            if (ev == EV_OKLONG) {
                rec_mode_stop();
            } else if (ev == EV_OK || ev == EV_UP || ev == EV_DOWN
                       || ev == EV_UPLONG || ev == EV_DOWNLONG) {
                if (s_wake_us == 0) {
                    rec_mode_screen_wake();
                    refresh_home();          // 显示"录音中"仪表盘
                }
                s_wake_us = esp_timer_get_time();  // 重置 10s 计时
            }
            continue;
        }
        switch (ev) {
        case EV_UP:
            // 滚动：详情页/设置子页/首次引导(引导为detail正文)；
            // 其余层级(工作区列表/会话列表/设置菜单)都是列表→移动光标。
            if (s_level == LVL_DETAIL || s_level == LVL_SUBPAGE ||
                (s_onboarding && s_level == LVL_WS)) ui_scroll_answer(28);
            else ui_list_move(-1);
            break;
        case EV_DOWN:
            if (s_level == LVL_DETAIL || s_level == LVL_SUBPAGE ||
                (s_onboarding && s_level == LVL_WS)) ui_scroll_answer(-28);
            else ui_list_move(1);
            break;
        case EV_UPLONG:               // 长按上 = 手动：30 分钟一节
        case EV_DOWNLONG:             // 长按下 = 续录：同步排空后自动续
            start_rec_session(ev == EV_UPLONG ? REC_MODE_MANUAL : REC_MODE_AUTO);
            break;
        case EV_OK:
            // LONG 之后松手可能再报一次 CLICK：600ms 内忽略
            if (esp_timer_get_time() - s_last_oklong_us < 600000) break;
            if (s_level == LVL_SETTINGS) {
                settings_activate(ui_list_sel());
            } else if (s_level == LVL_SUBPAGE) {
                break;                       // 子页只读，OK 无操作
            } else if (s_level == LVL_WS) {
                if (!s_onboarding) build_settings(); // v0.4.49: 主界面 OK → 设置（不再进工作区）
            } else if (s_level == LVL_SESS) {
                int sel = ui_list_sel();
                if (sel >= 0 && sel < s_sess_n) {
                    s_level = LVL_DETAIL;
                    enter_detail(sel);
                }
            } else {
                if (s_ptt == 1) s_rec_stop = true;   // 录音中：结束并发送
                else if (s_ptt == 0) do_ask();       // 空闲：开始录音
                // s_ptt==2（等网关处理）：忽略，防重复触发
            }
            break;
        case EV_OKLONG:
            s_last_oklong_us = esp_timer_get_time();
            if (s_ptt == 1) { s_rec_cancel = true; break; }    // 录音中：取消，do_ask 立即返回
            if (s_ptt == 2) break;                             // 等网关处理中：无法打断，忽略
            if (s_level == LVL_DETAIL) {
                s_level = LVL_SESS;
                if (s_sess_n > 0) build_sess_list();
                else refresh_lists();
            } else if (s_level == LVL_SESS) {
                s_level = LVL_WS;
                build_ws_list();
            } else if (s_level == LVL_SUBPAGE) {
                if (s_provision_confirm) {
                    // v0.4.50：长按OK确认配网——才真正进入
                    s_provision_confirm = false;
                    cfg_set_provision_request(true);
                    ui_set_busy("进入配网模式…");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_restart();
                } else {
                    build_settings();          // 子页返回设置菜单
                }
            } else if (s_level == LVL_SETTINGS) {
                back_to_main();            // 设置返回主页
            } else {
                build_settings();          // 顶级：长按 OK = 进设置（配网/静音/测试/刷新）
            }
            break;
        }
    }
}

// ---------- 按键（button 任务回调，只投递事件，不做重活） ----------

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    badge_ev_t e;
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        if (s_ptt == 2) return;                  // 等网关中：忽略（防迟到事件在处理完后误返回）
        e = EV_OKLONG;                           // 录音中长按=取消，由 worker 处理
    } else if (btn == BSP_BTN_UP && ev == BSP_BTN_LONG) {
        if (s_ptt != 0) return;
        e = EV_UPLONG;                           // 长按上 = 手动 30 分钟一节
    } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
        if (s_ptt != 0) return;
        e = EV_DOWNLONG;                         // 长按下 = 连续录音（自动续录）
    } else if (ev != BSP_BTN_CLICK) {
        return;
    } else if (btn == BSP_BTN_UP) {
        if (s_ptt != 0) return;
        e = EV_UP;
    } else if (btn == BSP_BTN_DOWN) {
        if (s_ptt != 0) return;
        e = EV_DOWN;
    } else {
        // OK 单击：录音中=结束并发送（直接置标志，不入队，
        // 否则 worker 忙完 do_ask 会消费过期事件、误触发第二次录音）；
        // 等网关中=忽略；空闲=入队
        if (s_ptt == 1) { s_rec_stop = true; return; }
        if (s_ptt == 2) return;
        e = EV_OK;
    }
    xQueueSend(s_evq, &e, 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "工牌录音固件启动");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }
    bsp_display_backlight(100);

    ui_init();
    if (audio_init() != ESP_OK) ESP_LOGW(TAG, "音频初始化失败（语音不可用）");
    bsp_battery_init();

    s_evq = xQueueCreate(16, sizeof(badge_ev_t));
    if (bsp_button_init(on_key, NULL) != ESP_OK) ESP_LOGE(TAG, "按键初始化失败");

    cfg_load(&g_cfg);
    ui_set_mute(g_cfg.mute);
    if (cfg_get_provision_request()) {
        provision_run();          // 配网模式：阻塞，网页保存后内部重启
    }
    gw_client_init(g_cfg.gw_url, g_cfg.gw_token);
    rec_upload_init();              // 上传任务（v2 被动模型：kick 驱动，不碰无线电）
    s_onboarding = !g_cfg.ssid[0];

    // Opus 编码器开机预开并永久持有：必须在 WiFi 之前——WiFi 之后堆碎片化，
    // 最大连续块 < 编码器状态 ~25KB，运行期 open 必败 ret:-7（v0.4.17 实测
    // 开机 ret=0 破案）。占用 ~25KB 但换来录音可用。
    if (rec_mode_init() != ESP_OK) {
        ESP_LOGW(TAG, "录音不可用（Opus 编码器开机预开失败，其余功能正常）");
    }

    xTaskCreate(badge_worker, "badge_worker", 6144, NULL, 5, NULL);
    badge_console_start();           // USB 控制台（COM 口 REPL：rec-start/rec-stop/batt/kick…）
    ESP_LOGI(TAG, "worker 已启动，网关=%s", g_cfg.gw_url);
}
