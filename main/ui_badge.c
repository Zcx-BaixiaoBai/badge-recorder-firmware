// main/ui_badge.c —— 秒忆卡 MemoSnap 工牌 UI v2（v0.5.0 全面重绘）
//
// 视觉语言（2026-09-20 用户确认的参考设计）：
//   - 纯黑页底 + 内嵌橙色圆角卡（录音仪表盘主视觉，四边留黑边）
//   - 底部黑区：实时白色波形条（录音=麦克风电平滚动波形；同步=追逐波；
//     空闲=呼吸波）+ 品牌行（logo 方块 + 秒忆卡 MemoSnap）
//   - 主标题用拉丁大字/大数字（Montserrat），中文做小注（思源黑体 14px）
//   - 状态驱动卡心：REC=大计时器 / SYNC=旋转环 / IDLE=品牌标记+状态
//   - 开机 morph 动画：横线 → 圆环 → 品牌字淡入（参考设计的 boot 序列）
// 列表/设置/详情等二级页保持黑底：选中行=橙底白字。
// 字体：lv_font_ai_passport_14（中文）+ Montserrat 14/20/36（拉丁/数字）。
#include "ui_badge.h"
#include "audio_pipe.h"
#include "bsp_display.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_ai_passport_14);
#define FONT_CJK   (&lv_font_ai_passport_14)
#define FONT_LAT_S (&lv_font_montserrat_14)
#define FONT_LAT_M (&lv_font_montserrat_20)
#define FONT_LAT_L (&lv_font_montserrat_36)

// 调色板：黑底橙卡（参考设计取色）
#define C_INK    0x0B0B0Cu   // 页底纯黑
#define C_ORANGE 0xE96A2Eu   // 品牌橙（主卡/选中行）
#define C_CARD   0x17181Au   // 黑底上的深色卡（未选中行/回复框）
#define C_TEXT   0xE8EAEAu   // 黑底主文字
#define C_DIM    0x9BA0A3u   // 黑底次级文字
#define C_WHITE  0xFFFFFFu
#define C_WARN   0xFBBF24u
#define C_ERR    0xF85149u

// 几何（240x320）
#define CARD_X 6
#define CARD_Y 6
#define CARD_W 228
#define CARD_H 232
#define CARD_R 18
#define BAR_N     16                 // 波形条数
#define BAR_W     6
#define BAR_GAP   4
#define BAR_X0    ((240 - (BAR_N * BAR_W + (BAR_N - 1) * BAR_GAP)) / 2)
#define BAR_BASE  288                // 条底 y（向上生长）
#define BAR_MAX   44
#define ROWS_MAX  32
#define ROW_H     36

// 卡心状态
enum { HS_NONE = -1, HS_IDLE = 0, HS_REC = 1, HS_SYNC = 2, HS_DONE = 3 };

static void body_clear(void);   // 前向声明（ui_show_home 要清二级页残留）

static lv_obj_t *s_scr;
// 顶栏（二级页；首页隐藏）
static lv_obj_t *s_hdr_title, *s_hdr_batt, *s_hdr_mute;
// 底栏
static lv_obj_t *s_hint;             // 按键提示（二级页底栏；首页在卡内）
static lv_obj_t *s_bars[BAR_N];      // 波形条（首页）
static lv_obj_t *s_brand;            // 品牌行（首页）
// 首页橙卡
static lv_obj_t *s_card, *s_chip_dot, *s_chip_txt, *s_batt_lbl;
static lv_obj_t *s_env, *s_env_n;    // 待传信封角标
static lv_obj_t *s_card_hint;        // 卡内底部按键提示
static lv_obj_t *s_mid;              // 卡心容器（按状态重建）
static lv_obj_t *s_timer_lbl;        // REC：大计时器
static lv_obj_t *s_sub1, *s_sub2;    // 状态小注行
static lv_obj_t *s_done_check, *s_done_excl, *s_done_word;   // DONE 页构件
static int  s_home_state = HS_NONE;
// 计时/电量缓存（避免无谓重绘）
static int  s_rec_base_s = -1;
static uint32_t s_rec_base_tick;
static int  s_timer_shown = -1;
static int  s_soc_shown = -2;
// 详情态
static lv_obj_t *s_state_lbl, *s_ans_cont, *s_ans_lbl;
// 列表态
static lv_obj_t *s_body;
static lv_obj_t *s_rows[ROWS_MAX];
static int s_row_n, s_sel;
// 遮罩 / 开机动画
static lv_obj_t *s_busy, *s_busy_lbl;
static lv_obj_t *s_boot, *s_boot_brand;

static void lock(void)   { bsp_lvgl_lock(2000); }
static void unlock(void) { bsp_lvgl_unlock(); }

// ---------- 基础构件 ----------

static void flat(lv_obj_t *o, uint32_t bg, int radius)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, uint32_t color,
                          const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void style_scrollbar(lv_obj_t *o)
{
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(o, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_WHITE), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(o, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(o, 2, LV_PART_SCROLLBAR);
}

// ---------- 波形条动画（60ms tick，LVGL 任务上下文，无需锁） ----------

static void ui_tick_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();

    if (s_bars[0] && !lv_obj_has_flag(s_bars[0], LV_OBJ_FLAG_HIDDEN)) {
        static uint8_t hist[BAR_N];
        for (int i = 0; i < BAR_N - 1; i++) hist[i] = hist[i + 1];
        int h[BAR_N];
        if (s_home_state == HS_REC) {
            // 麦克风电平滚动波形；静音时给一点底噪起伏避免死平
            int lvl = audio_pipe_level();
            hist[BAR_N - 1] = (uint8_t)lvl;
            for (int i = 0; i < BAR_N; i++) {
                int v = hist[i];
                if (v < 4) v = (int)((now / 90 + (uint32_t)i * 7) % 3);
                h[i] = 4 + v * (BAR_MAX - 4) / 100;
            }
        } else if (s_home_state == HS_SYNC) {
            // 追逐波：上传脉冲从左扫到右
            int ph = (int)((now / 70) % BAR_N);
            for (int i = 0; i < BAR_N; i++) {
                int d = i - ph;
                h[i] = 5 + (int)(34.0f * expf(-(float)(d * d) / 6.0f));
            }
        } else {
            // 呼吸波：双频叠加的待机起伏
            for (int i = 0; i < BAR_N; i++) {
                h[i] = 6 + (int)(8.0f * (0.5f + 0.5f * sinf((float)now / 650.0f + i * 0.5f)))
                     + (int)(3.0f * (0.5f + 0.5f * sinf((float)now / 237.0f + i * 1.3f)));
            }
            hist[BAR_N - 1] = 0;
        }
        for (int i = 0; i < BAR_N; i++) {
            if ((int)lv_obj_get_height(s_bars[i]) != h[i])
                lv_obj_set_height(s_bars[i], h[i]);
        }
    }

    // REC 大计时器：秒级刷新（基准由 ui_set_recinfo 注入）
    if (s_home_state == HS_REC && s_timer_lbl && s_rec_base_s >= 0) {
        int cur = s_rec_base_s + (int)((now - s_rec_base_tick) / 1000);
        if (cur != s_timer_shown) {
            s_timer_shown = cur;
            char buf[12];
            snprintf(buf, sizeof(buf), "%02d:%02d", cur / 60, cur % 60);
            lv_label_set_text(s_timer_lbl, buf);
        }
    }
}

// ---------- 开机 morph 动画 ----------

static void boot_exec(void *var, int32_t v)
{
    lv_obj_t *g = (lv_obj_t *)var;
    if (v < 400) {                       // 横线生长
        int k = v;
        lv_obj_set_size(g, 10 + 46 * k / 400, 5);
        lv_obj_set_style_radius(g, 3, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(g, 0, 0);
    } else if (v < 800) {                // 线 → 圆环（填充退场、描边进场）
        int k = v - 400;
        lv_obj_set_size(g, 56, 5 + 51 * k / 400);
        lv_obj_set_style_radius(g, 3 + 25 * k / 400, 0);
        lv_obj_set_style_bg_opa(g, (lv_opa_t)(255 - 255 * k / 400), 0);
        lv_obj_set_style_border_width(g, 3, 0);
        lv_obj_set_style_border_color(g, lv_color_hex(C_WHITE), 0);
        lv_obj_set_style_border_opa(g, (lv_opa_t)(255 * k / 400), 0);
    } else {                             // 圆环保持 + 品牌字淡入
        if (s_boot_brand) {
            int o = (v - 800) * 255 / 400;
            lv_obj_set_style_text_opa(s_boot_brand, (lv_opa_t)(o > 255 ? 255 : o), 0);
        }
    }
}

static void boot_fade_exec(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void boot_done(lv_anim_t *a)
{
    (void)a;
    if (s_boot) { lv_obj_delete(s_boot); s_boot = NULL; s_boot_brand = NULL; }
}

// ---------- 首页（橙卡仪表盘） ----------

static void dot_opa_exec(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void pulse_dot(lv_obj_t *dot)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dot);
    lv_anim_set_values(&a, 255, 70);
    lv_anim_set_time(&a, 700);
    lv_anim_set_playback_time(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, dot_opa_exec);
    lv_anim_start(&a);
}

// 卡心按状态重建；chip/角标同步
static void home_build(int state)
{
    s_home_state = state;
    lv_obj_clean(s_mid);
    s_timer_lbl = s_sub1 = s_sub2 = NULL;
    s_done_check = s_done_excl = s_done_word = NULL;
    lv_obj_add_flag(s_env, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_env_n, LV_OBJ_FLAG_HIDDEN);

    if (state == HS_REC) {
        lv_label_set_text(s_chip_txt, "REC");
        lv_obj_set_style_text_letter_space(s_chip_txt, 3, 0);
        lv_obj_clear_flag(s_chip_dot, LV_OBJ_FLAG_HIDDEN);
        lv_anim_delete(s_chip_dot, NULL);   // 防重建时呼吸动画叠加
        pulse_dot(s_chip_dot);
        s_timer_lbl = mk_label(s_mid, "00:00", C_WHITE, FONT_LAT_L);
        lv_obj_set_style_text_align(s_timer_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_timer_lbl, 196);
        s_sub1 = mk_label(s_mid, "录音中", C_WHITE, FONT_CJK);
        lv_obj_set_style_text_opa(s_sub1, LV_OPA_80, 0);
        lv_obj_set_style_text_align(s_sub1, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_sub1, 196);
        s_timer_shown = -1;
    } else if (state == HS_SYNC) {
        lv_label_set_text(s_chip_txt, "SYNC");
        lv_obj_set_style_text_letter_space(s_chip_txt, 3, 0);
        lv_obj_add_flag(s_chip_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *sp = lv_spinner_create(s_mid);
        lv_spinner_set_anim_params(sp, 1100, 220);
        lv_obj_set_size(sp, 44, 44);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp, lv_color_hex(C_WHITE), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(sp, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, lv_color_hex(C_WHITE), LV_PART_INDICATOR);
        s_sub1 = mk_label(s_mid, "同步中", C_WHITE, FONT_CJK);
        lv_obj_set_style_text_opa(s_sub1, LV_OPA_80, 0);
        lv_obj_set_style_text_align(s_sub1, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_sub1, 196);
    } else if (state == HS_DONE) {
        // 会话结束页：圆环 + 对勾/叹号 + 拉丁大字 + 两行中文小注
        lv_label_set_text(s_chip_txt, "MEMOSNAP");
        lv_obj_set_style_text_letter_space(s_chip_txt, 3, 0);
        lv_obj_add_flag(s_chip_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *ring = lv_obj_create(s_mid);
        lv_obj_set_size(ring, 44, 44);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(C_WHITE), 0);
        lv_obj_set_style_radius(ring, 22, 0);
        lv_obj_set_style_pad_all(ring, 0, 0);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        static const lv_point_precise_t chk[3] = { {13, 23}, {20, 30}, {32, 14} };
        s_done_check = lv_line_create(ring);
        lv_obj_set_size(s_done_check, 44, 44);
        lv_line_set_points(s_done_check, chk, 3);
        lv_obj_set_style_line_color(s_done_check, lv_color_hex(C_WHITE), 0);
        lv_obj_set_style_line_width(s_done_check, 4, 0);
        lv_obj_set_style_line_rounded(s_done_check, true, 0);
        s_done_excl = mk_label(ring, "!", C_WHITE, FONT_LAT_M);
        lv_obj_center(s_done_excl);
        s_done_word = mk_label(s_mid, "DONE", C_WHITE, FONT_LAT_M);
        lv_obj_set_style_text_letter_space(s_done_word, 5, 0);
        lv_obj_set_style_text_align(s_done_word, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_done_word, 196);
        s_sub1 = mk_label(s_mid, "", C_WHITE, FONT_CJK);
        lv_obj_set_style_text_opa(s_sub1, LV_OPA_80, 0);
        lv_obj_set_style_text_align(s_sub1, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_sub1, 196);
        s_sub2 = mk_label(s_mid, "", C_WHITE, FONT_CJK);
        lv_obj_set_style_text_opa(s_sub2, LV_OPA_60, 0);
        lv_obj_set_style_text_align(s_sub2, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_sub2, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_sub2, 196);
    } else {
        lv_label_set_text(s_chip_txt, "MEMOSNAP");
        lv_obj_set_style_text_letter_space(s_chip_txt, 3, 0);
        lv_obj_add_flag(s_chip_dot, LV_OBJ_FLAG_HIDDEN);
        // 品牌标记：三根圆头竖条（波形 logo）
        lv_obj_t *mark = lv_obj_create(s_mid);
        lv_obj_set_size(mark, 28, 28);
        lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(mark, 0, 0);
        lv_obj_set_style_pad_all(mark, 0, 0);
        lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
        static const struct { int x, y, h; } M[3] = { {0, 12, 16}, {11, 0, 28}, {22, 18, 10} };
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = lv_obj_create(mark);
            flat(b, C_WHITE, 3);
            lv_obj_set_size(b, 6, M[i].h);
            lv_obj_set_pos(b, M[i].x, M[i].y);
        }
        lv_obj_t *word = mk_label(s_mid, "MEMOSNAP", C_WHITE, FONT_LAT_M);
        lv_obj_set_style_text_letter_space(word, 5, 0);
        lv_obj_set_style_text_align(word, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(word, 196);
        s_sub1 = mk_label(s_mid, "就绪", C_WHITE, FONT_CJK);
        lv_obj_set_style_text_opa(s_sub1, LV_OPA_80, 0);
        lv_obj_set_style_text_align(s_sub1, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_sub1, 196);
        s_sub2 = mk_label(s_mid, "全天候静默录音", C_WHITE, FONT_CJK);
        lv_obj_set_style_text_opa(s_sub2, LV_OPA_60, 0);
        lv_obj_set_style_text_align(s_sub2, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_sub2, 196);
    }
}

// 首页/二级页的chrome切换：顶栏+底栏提示 vs 橙卡+波形+品牌行
static void show_chrome(bool home)
{
    lv_obj_t *hdr[3] = { s_hdr_title, s_hdr_batt, s_hdr_mute };
    for (int i = 0; i < 3; i++)
        if (hdr[i]) lv_obj_set_flag(hdr[i], LV_OBJ_FLAG_HIDDEN, home);
    lv_obj_set_flag(s_hint, LV_OBJ_FLAG_HIDDEN, home);
    lv_obj_set_flag(s_card, LV_OBJ_FLAG_HIDDEN, !home);
    lv_obj_set_flag(s_brand, LV_OBJ_FLAG_HIDDEN, !home);
    for (int i = 0; i < BAR_N; i++)
        lv_obj_set_flag(s_bars[i], LV_OBJ_FLAG_HIDDEN, !home);
}

void ui_show_home(void)
{
    lock();
    body_clear();              // 设置/子页残留的列表必须销毁（v0.5.5：回首页 bug）
    show_chrome(true);
    if (s_home_state == HS_NONE) home_build(HS_IDLE);
    unlock();
}

void ui_set_home(const char *status, int color, int soc, int mv, int pending)
{
    (void)color; (void)mv;
    int state = HS_IDLE;
    bool ending = false;
    if (status && strstr(status, "录音")) state = HS_REC;
    else if (status && strstr(status, "同步")) state = HS_SYNC;
    else if (status && strstr(status, "收尾")) { state = HS_SYNC; ending = true; }

    lock();
    if (state != s_home_state) home_build(state);

    // 电量（卡内右上角）
    if (soc != s_soc_shown) {
        s_soc_shown = soc;
        char buf[16];
        if (soc < 0) snprintf(buf, sizeof(buf), "--");
        else snprintf(buf, sizeof(buf), "%d%%", soc);
        lv_label_set_text(s_batt_lbl, buf);
        lv_obj_set_style_text_color(s_batt_lbl,
            lv_color_hex(soc >= 0 && soc <= 20 ? C_ERR : C_WHITE), 0);
    }

    if (state == HS_IDLE) {
        bool has_pend = pending > 0;
        lv_obj_set_flag(s_env, LV_OBJ_FLAG_HIDDEN, !has_pend);
        lv_obj_set_flag(s_env_n, LV_OBJ_FLAG_HIDDEN, !has_pend);
        if (has_pend) {
            char n[12];
            snprintf(n, sizeof(n), "%d", pending);
            lv_label_set_text(s_env_n, n);
            if (s_sub1) lv_label_set_text(s_sub1, "待传段");
            if (s_sub2) lv_label_set_text(s_sub2, "联网后自动补传");
        } else {
            if (s_sub1) lv_label_set_text(s_sub1, status && status[0] ? status : "就绪");
            if (s_sub2) lv_label_set_text(s_sub2, "全天候静默录音");
        }
    } else if (state == HS_SYNC && s_sub1) {
        if (ending) {
            lv_label_set_text(s_sub1, "收尾中 · 正在封装录音");
        } else {
            char buf[40];
            snprintf(buf, sizeof(buf), "同步中 · 待传 %d 段", pending);
            lv_label_set_text(s_sub1, buf);
        }
    }
    unlock();
}

// 会话结束页（kind: 0=同步完成 1=待传遗留 2=异常结束）
void ui_show_done(int kind, const char *line1, const char *line2)
{
    lock();
    body_clear();
    show_chrome(true);
    if (s_home_state != HS_DONE) home_build(HS_DONE);
    if (s_done_check) lv_obj_set_flag(s_done_check, LV_OBJ_FLAG_HIDDEN, kind != 0);
    if (s_done_excl)  lv_obj_set_flag(s_done_excl,  LV_OBJ_FLAG_HIDDEN, kind == 0);
    if (s_done_word)  lv_label_set_text(s_done_word, kind == 0 ? "DONE" : (kind == 1 ? "WAIT" : "FAIL"));
    if (s_sub1) lv_label_set_text(s_sub1, line1 && line1[0] ? line1 : "");
    if (s_sub2) lv_label_set_text(s_sub2, line2 && line2[0] ? line2 : "");
    unlock();
}

void ui_set_recinfo(int mode_auto, int elapsed_s)
{
    lock();
    s_rec_base_s = elapsed_s;
    s_rec_base_tick = lv_tick_get();
    if (s_home_state == HS_REC && s_sub1)
        lv_label_set_text(s_sub1, mode_auto ? "录音中 · 连续模式" : "录音中 · 单次");
    unlock();
}

// ---------- 顶栏 / 底栏 ----------

void ui_set_header(const char *title)
{
    lock();
    lv_label_set_text(s_hdr_title, title);
    unlock();
}

void ui_set_battery(int soc)
{
    char buf[16];
    uint32_t col;
    if (soc < 0) { snprintf(buf, sizeof(buf), "--"); col = C_DIM; }
    else {
        snprintf(buf, sizeof(buf), "%d%%", soc);
        col = soc <= 20 ? C_ERR : (soc <= 40 ? C_WARN : C_DIM);
    }
    lock();
    lv_label_set_text(s_hdr_batt, buf);
    lv_obj_set_style_text_color(s_hdr_batt, lv_color_hex(col), 0);
    unlock();
}

void ui_set_mute(bool on)
{
    lock();
    if (s_hdr_mute)
        lv_obj_set_flag(s_hdr_mute, LV_OBJ_FLAG_HIDDEN, !on);
    unlock();
}

void ui_set_hint(const char *text)
{
    lock();
    lv_label_set_text(s_hint, text);
    lv_label_set_text(s_card_hint, text);
    unlock();
}

void ui_set_busy(const char *msg)
{
    lock();
    if (!msg) {
        if (s_busy) { lv_obj_delete(s_busy); s_busy = NULL; s_busy_lbl = NULL; }
    } else if (!s_busy) {
        s_busy = lv_obj_create(s_scr);
        lv_obj_set_size(s_busy, 240, 320);
        lv_obj_set_pos(s_busy, 0, 0);
        flat(s_busy, C_INK, 0);
        lv_obj_set_style_opa(s_busy, LV_OPA_90, 0);
        lv_obj_set_flex_flow(s_busy, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_busy, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(s_busy, 14, 0);

        lv_obj_t *sp = lv_spinner_create(s_busy);
        lv_spinner_set_anim_params(sp, 1100, 220);
        lv_obj_set_size(sp, 34, 34);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp, lv_color_hex(C_WHITE), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(sp, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, lv_color_hex(C_WHITE), LV_PART_INDICATOR);

        s_busy_lbl = mk_label(s_busy, msg, C_TEXT, FONT_CJK);
        lv_label_set_long_mode(s_busy_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_busy_lbl, 200);
        lv_obj_set_style_text_align(s_busy_lbl, LV_TEXT_ALIGN_CENTER, 0);
        // 开机 morph 期间来的 busy（连 WiFi）不能挡住动画：morph 层提到最前，
        // 淡出删除后 busy 自然露出（v0.5.5）
        if (s_boot) lv_obj_move_foreground(s_boot);
    } else if (s_busy_lbl) {
        lv_label_set_text(s_busy_lbl, msg);
    }
    unlock();
}

// ---------- 列表（设置等二级页） ----------

static void body_clear(void)
{
    if (s_body) { lv_obj_delete(s_body); s_body = NULL; }
    s_row_n = 0; s_sel = 0;
    s_state_lbl = s_ans_cont = s_ans_lbl = NULL;
}

static lv_obj_t *body_create(bool scrollable)
{
    body_clear();
    s_body = lv_obj_create(s_scr);
    lv_obj_set_size(s_body, 240, 248);
    lv_obj_set_pos(s_body, 0, 40);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_shadow_width(s_body, 0, 0);
    lv_obj_set_style_radius(s_body, 0, 0);
    lv_obj_set_style_pad_hor(s_body, 12, 0);
    lv_obj_set_style_pad_ver(s_body, 2, 0);
    lv_obj_set_style_pad_row(s_body, 6, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    if (scrollable) {
        lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
        style_scrollbar(s_body);
    } else {
        lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    }
    return s_body;
}

// 选中=橙底白字；普通=深卡浅字
static void style_row(lv_obj_t *row, bool sel)
{
    lv_obj_set_style_bg_color(row, lv_color_hex(sel ? C_ORANGE : C_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_t *lm = lv_obj_get_child(row, 0);
    lv_obj_t *lr = lv_obj_get_child(row, 1);
    if (lm) lv_obj_set_style_text_color(lm, lv_color_hex(sel ? C_WHITE : C_TEXT), 0);
    if (lr) lv_obj_set_style_text_color(lr, lv_color_hex(sel ? C_WHITE : C_DIM), 0);
    if (lr) lv_obj_set_style_text_opa(lr, sel ? LV_OPA_80 : LV_OPA_COVER, 0);
}

void ui_show_list(const ui_row_t *rows, int n, int sel)
{
    lock();
    show_chrome(false);
    body_create(n > 6);
    if (n > ROWS_MAX) n = ROWS_MAX;
    if (n <= 0) {
        lv_obj_t *empty = mk_label(s_body, "（无）", C_DIM, FONT_CJK);
        lv_obj_set_style_pad_top(empty, 36, 0);
        unlock();
        return;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(s_body);
        lv_obj_set_size(row, 216, ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);

        lv_obj_t *lm = mk_label(row, rows[i].main ? rows[i].main : "", C_TEXT, FONT_CJK);
        lv_label_set_long_mode(lm, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lm, 136);
        lv_obj_align(lm, LV_ALIGN_LEFT_MID, 0, 0);

        if (rows[i].right && rows[i].right[0]) {
            lv_obj_t *lr = mk_label(row, rows[i].right, C_DIM, FONT_CJK);
            lv_label_set_long_mode(lr, LV_LABEL_LONG_DOT);
            lv_obj_set_width(lr, 60);
            lv_obj_set_style_text_align(lr, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(lr, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        style_row(row, i == sel);
        s_rows[i] = row;
    }
    s_row_n = n;
    s_sel = sel;
    lv_obj_scroll_to_view(s_rows[sel], LV_ANIM_OFF);
    unlock();
}

void ui_list_move(int delta)
{
    lock();
    if (s_row_n > 0) {
        style_row(s_rows[s_sel], false);
        s_sel = (s_sel + delta + s_row_n) % s_row_n;
        style_row(s_rows[s_sel], true);
        lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_ON);
    }
    unlock();
}

int ui_list_sel(void)   { return s_sel; }
int ui_list_count(void) { return s_row_n; }

// ---------- 详情页（遗留问答态） ----------

void ui_show_detail(const char *title)
{
    lock();
    show_chrome(false);
    body_create(false);
    lv_obj_set_style_pad_row(s_body, 8, 0);

    lv_obj_t *t = mk_label(s_body, title, C_TEXT, FONT_CJK);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, 216);

    s_state_lbl = mk_label(s_body, "就绪", C_WARN, FONT_CJK);
    lv_label_set_long_mode(s_state_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_state_lbl, 216);

    s_ans_cont = lv_obj_create(s_body);
    lv_obj_set_width(s_ans_cont, 216);
    lv_obj_set_flex_grow(s_ans_cont, 1);
    lv_obj_set_style_bg_color(s_ans_cont, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(s_ans_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ans_cont, 0, 0);
    lv_obj_set_style_shadow_width(s_ans_cont, 0, 0);
    lv_obj_set_style_radius(s_ans_cont, 10, 0);
    lv_obj_set_style_pad_all(s_ans_cont, 8, 0);
    lv_obj_set_scroll_dir(s_ans_cont, LV_DIR_VER);
    style_scrollbar(s_ans_cont);

    s_ans_lbl = mk_label(s_ans_cont, "（回复显示在这里）", C_DIM, FONT_CJK);
    lv_label_set_long_mode(s_ans_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ans_lbl, 198);

    unlock();
}

void ui_set_state(const char *state_text)
{
    lock();
    if (s_state_lbl) {
        lv_label_set_text(s_state_lbl, state_text);
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(C_WARN), 0);
    }
    unlock();
}

void ui_set_answer(const char *text)
{
    lock();
    if (s_ans_lbl) {
        lv_label_set_text(s_ans_lbl, text && text[0] ? text : "（无文本回复）");
        lv_obj_set_style_text_color(s_ans_lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_scroll_to_y(s_ans_cont, 0, LV_ANIM_OFF);
    }
    unlock();
}

void ui_scroll_answer(int delta_pixels)
{
    lock();
    if (s_ans_cont) lv_obj_scroll_by(s_ans_cont, 0, delta_pixels, LV_ANIM_ON);
    unlock();
}

void ui_set_error(const char *msg)
{
    lock();
    if (s_state_lbl) {
        char buf[160];
        snprintf(buf, sizeof(buf), "出错：%s", msg ? msg : "未知");
        lv_label_set_text(s_state_lbl, buf);
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(C_ERR), 0);
    }
    unlock();
}

// ---------- 初始化 ----------

void ui_init(void)
{
    lock();
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_INK), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    // 顶栏（二级页）：纯文字行，无底色
    s_hdr_title = mk_label(s_scr, "秒忆卡", C_TEXT, FONT_CJK);
    lv_label_set_long_mode(s_hdr_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_hdr_title, 140);
    lv_obj_set_pos(s_hdr_title, 12, 10);
    s_hdr_mute = mk_label(s_scr, "静音", C_WARN, FONT_CJK);
    lv_obj_align(s_hdr_mute, LV_ALIGN_TOP_RIGHT, -56, 10);   // 电量右对齐占 -12..-52，静音让到 -56 左
    lv_obj_add_flag(s_hdr_mute, LV_OBJ_FLAG_HIDDEN);
    s_hdr_batt = mk_label(s_scr, "--", C_DIM, FONT_LAT_S);
    lv_obj_set_style_text_align(s_hdr_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_hdr_batt, 40);
    lv_obj_set_pos(s_hdr_batt, 188, 10);

    // 底栏按键提示（二级页）
    s_hint = mk_label(s_scr, "", C_DIM, FONT_CJK);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_hint, 232);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hint, 4, 298);

    // 首页橙卡
    s_card = lv_obj_create(s_scr);
    flat(s_card, C_ORANGE, CARD_R);
    lv_obj_set_size(s_card, CARD_W, CARD_H);
    lv_obj_set_pos(s_card, CARD_X, CARD_Y);
    // 顶行：状态 chip（圆点+拉丁字）+ 待传信封 + 电量
    s_chip_dot = lv_obj_create(s_card);
    flat(s_chip_dot, C_WHITE, 4);
    lv_obj_set_size(s_chip_dot, 8, 8);
    lv_obj_set_pos(s_chip_dot, 16, 21);
    lv_obj_add_flag(s_chip_dot, LV_OBJ_FLAG_HIDDEN);
    s_chip_txt = mk_label(s_card, "MEMOSNAP", C_WHITE, FONT_LAT_S);
    lv_obj_set_style_text_letter_space(s_chip_txt, 3, 0);
    lv_obj_set_pos(s_chip_txt, 30, 16);
    s_batt_lbl = mk_label(s_card, "--", C_WHITE, FONT_LAT_S);
    lv_obj_set_style_text_opa(s_batt_lbl, LV_OPA_80, 0);
    lv_obj_set_style_text_align(s_batt_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_batt_lbl, 40);
    lv_obj_align(s_batt_lbl, LV_ALIGN_TOP_RIGHT, -16, 14);
    s_env_n = mk_label(s_card, "0", C_WHITE, FONT_LAT_S);
    lv_obj_set_style_text_opa(s_env_n, LV_OPA_90, 0);
    lv_obj_align(s_env_n, LV_ALIGN_TOP_RIGHT, -50, 14);
    lv_obj_add_flag(s_env_n, LV_OBJ_FLAG_HIDDEN);
    s_env = lv_obj_create(s_card);
    lv_obj_set_size(s_env, 16, 11);
    lv_obj_align(s_env, LV_ALIGN_TOP_RIGHT, -72, 18);
    lv_obj_set_style_bg_opa(s_env, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_env, 2, 0);
    lv_obj_set_style_border_color(s_env, lv_color_hex(C_WHITE), 0);
    lv_obj_set_style_border_opa(s_env, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_env, 2, 0);
    lv_obj_set_style_pad_all(s_env, 0, 0);
    lv_obj_remove_flag(s_env, LV_OBJ_FLAG_SCROLLABLE);
    static const lv_point_precise_t flap[3] = { {0, 0}, {8, 6}, {16, 0} };
    lv_obj_t *fl = lv_line_create(s_env);
    lv_obj_set_size(fl, 16, 8);
    lv_obj_set_pos(fl, 0, 0);
    lv_line_set_points(fl, flap, 3);
    lv_obj_set_style_line_color(fl, lv_color_hex(C_WHITE), 0);
    lv_obj_set_style_line_width(fl, 2, 0);
    lv_obj_set_style_line_opa(fl, LV_OPA_90, 0);
    lv_obj_add_flag(s_env, LV_OBJ_FLAG_HIDDEN);
    // 卡心容器
    s_mid = lv_obj_create(s_card);
    lv_obj_set_size(s_mid, 196, 150);
    lv_obj_set_pos(s_mid, 16, 48);
    lv_obj_set_style_bg_opa(s_mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_mid, 0, 0);
    lv_obj_set_style_shadow_width(s_mid, 0, 0);
    lv_obj_set_style_pad_all(s_mid, 0, 0);
    lv_obj_set_style_pad_row(s_mid, 8, 0);
    lv_obj_set_flex_flow(s_mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_mid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_mid, LV_OBJ_FLAG_SCROLLABLE);
    // 卡内底部按键提示
    s_card_hint = mk_label(s_card, "", C_WHITE, FONT_CJK);
    lv_obj_set_style_text_opa(s_card_hint, LV_OPA_80, 0);
    lv_label_set_long_mode(s_card_hint, LV_LABEL_LONG_WRAP);   // 三键提示较长，换行不截断
    lv_obj_set_width(s_card_hint, 204);
    lv_obj_set_style_text_align(s_card_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_card_hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    // 底部黑区：波形条 + 品牌行
    for (int i = 0; i < BAR_N; i++) {
        s_bars[i] = lv_obj_create(s_scr);
        flat(s_bars[i], C_WHITE, 2);
        lv_obj_set_size(s_bars[i], BAR_W, 4);
        lv_obj_align(s_bars[i], LV_ALIGN_BOTTOM_LEFT,
                     BAR_X0 + i * (BAR_W + BAR_GAP), -(320 - BAR_BASE));
    }
    s_brand = lv_obj_create(s_scr);
    lv_obj_set_size(s_brand, 228, 20);
    lv_obj_set_pos(s_brand, 6, 296);
    lv_obj_set_style_bg_opa(s_brand, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_brand, 0, 0);
    lv_obj_set_style_pad_all(s_brand, 0, 0);
    lv_obj_remove_flag(s_brand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *logo = lv_obj_create(s_brand);
    lv_obj_set_size(logo, 14, 14);
    lv_obj_set_pos(logo, 6, 3);
    lv_obj_set_style_bg_opa(logo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(logo, 2, 0);
    lv_obj_set_style_border_color(logo, lv_color_hex(C_WHITE), 0);
    lv_obj_set_style_border_opa(logo, LV_OPA_60, 0);
    lv_obj_set_style_radius(logo, 4, 0);
    lv_obj_set_style_pad_all(logo, 0, 0);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *d = lv_obj_create(logo);
        flat(d, 0x35C08Au, 2);
        lv_obj_set_size(d, 3, 3);
        lv_obj_set_pos(d, 2 + i * 5, 4);
    }
    lv_obj_t *bt = mk_label(s_brand, "秒忆卡 MemoSnap", C_WHITE, FONT_CJK);
    lv_obj_set_style_text_opa(bt, LV_OPA_60, 0);
    lv_obj_set_pos(bt, 26, 3);

    lv_timer_create(ui_tick_cb, 60, NULL);

    // 开机 morph：横线 → 圆环 → 品牌字，最后整层淡出
    s_boot = lv_obj_create(s_scr);
    lv_obj_set_size(s_boot, 240, 320);
    lv_obj_set_pos(s_boot, 0, 0);
    flat(s_boot, C_INK, 0);
    lv_obj_t *glyph = lv_obj_create(s_boot);
    flat(glyph, C_WHITE, 3);
    lv_obj_set_size(glyph, 10, 5);
    lv_obj_center(glyph);
    lv_obj_set_style_translate_y(glyph, -24, 0);
    s_boot_brand = mk_label(s_boot, "MEMOSNAP", C_WHITE, FONT_LAT_M);
    lv_obj_set_style_text_letter_space(s_boot_brand, 5, 0);
    lv_obj_set_style_text_opa(s_boot_brand, LV_OPA_TRANSP, 0);
    lv_obj_center(s_boot_brand);
    lv_obj_set_style_translate_y(s_boot_brand, 26, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, glyph);
    lv_anim_set_values(&a, 0, 1400);
    lv_anim_set_time(&a, 1400);
    lv_anim_set_exec_cb(&a, boot_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, s_boot);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 450);
    lv_anim_set_delay(&a, 1550);
    lv_anim_set_exec_cb(&a, boot_fade_exec);
    lv_anim_set_ready_cb(&a, boot_done);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);

    lv_screen_load(s_scr);
    unlock();
}
