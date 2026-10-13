// main/app_printer.c —— 打印机状态主界面。
// 页面(动态):0 进度(环形进度圈+大%) 1 温度
//           2..2+AMS台数 AMS 料槽页(每台一页,2x2 槽) 设备(含 HMS) 控制(暂停/继续/停止/腔灯)。
// 数据源:app_mqtt 快照 + bsp_battery;1 秒定时刷新,OK 键上下文操作。
#include "app_printer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_mqtt.h"
#include "app_notify.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#include "font_zh.h"
#include "hms_codes.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"

#define PAGE_MAX 8          // 最多 2(固定)+4(AMS)+1(设备)+1(控制)
#define MSG_MAX  48

// 版面几何:上方 32..50 是常驻状态条(告警/云摘要),页面区从 52 起到屏底。
// 状态条常驻而非"有告警才弹出",这样告警出现时不会盖住页面内容、布局也不跳变。
#define BAR_Y     32
#define BAR_H     18
#define PAGE_Y    52
#define PAGE_H    268        // 52 + 268 = 320(屏底)

// 配色:工业仪表盘 —— 深蓝黑 + 拓竹荧光绿 + 喷嘴橙
#define C_BG      0x0B0F16
#define C_PANEL   0x10151D
#define C_PANEL2  0x141B25
#define C_LINE    0x1E2A38
#define C_GREEN   0x00C853
#define C_GREEN_L 0x34D97B
#define C_ORANGE  0xFF6B35
#define C_CYAN    0x2EC4B6
#define C_TEXT    0xE6EDF3
#define C_DIM     0x6B7A8C
#define C_RED     0xE5484D
#define C_YELLOW  0xF5A524

static lv_obj_t *s_scr;
static lv_obj_t *s_pages[PAGE_MAX];
static int s_pages_n;
static int s_cur;
static int s_midx;                 // 当前查看的机器下标(0..3)

// ------------------------------------------------------------ 机器概览(多机列表)
static bool s_ov_mode;      // true = 停留在机器列表(未进入详情)
static int  s_ov_sel;       // 列表选中项
static lv_obj_t *s_ov_page;
static lv_obj_t *s_ov_card[BAMBU_MACHINE_MAX];
static lv_obj_t *s_ov_dot[BAMBU_MACHINE_MAX];
static lv_obj_t *s_ov_name[BAMBU_MACHINE_MAX];
static lv_obj_t *s_ov_pct[BAMBU_MACHINE_MAX];
static lv_obj_t *s_ov_bar[BAMBU_MACHINE_MAX];
static lv_obj_t *s_ov_state[BAMBU_MACHINE_MAX];
static int s_ov_bar_w;
static int s_slot[PAGE_MAX];   // 导航序号 → 实体页槽位(实体固定:0进度 1温度 2~5AMS 6设备 7控制)
static lv_timer_t *s_timer;
static lv_timer_t *s_fast;     // 30ms 快速重绘:承接按键回调里延后的重绘请求
static bool s_need_refresh;    // 按键触发的重绘请求(放到 LVGL 任务里执行)
static bsp_btn_t   s_pk_btn;   // 挂起的按键(按钮回调只记录,不干活)
static bsp_btn_ev_t s_pk_ev;
static bool        s_pk_valid;
static bool s_active;

// 头部
static lv_obj_t *s_time, *s_bat_pct, *s_bat_fill, *s_cloud_dot;
static lv_obj_t *s_dots[PAGE_MAX];
// 常驻状态条(有 HMS 时显示告警,否则显示云/WiFi 摘要)
static lv_obj_t *s_bar, *s_bar_lbl;

// 进度页
static lv_obj_t *s_status, *s_job, *s_pct_box, *s_pct_sign;
static lv_obj_t *s_prog_track, *s_prog_fill;   // 横向进度条(替代折线圆环)
static lv_obj_t *s_time_lbl, *s_time_hint;
// 温度/层/风扇 chip(进度页下方 2x2)
static lv_obj_t *s_c_noz_v, *s_c_bed_v, *s_c_layer_v, *s_c_fan_v;
static bool s_show_eta;      // false=显示剩余;true=显示完成时刻
// 温度页
static lv_obj_t *s_noz_cur, *s_noz_tgt, *s_noz_bar, *s_noz_mark;
static lv_obj_t *s_bed_cur, *s_bed_tgt, *s_bed_bar, *s_bed_mark;
static lv_obj_t *s_cham_cur, *s_cham_bar;
static lv_obj_t *s_temp_remain;
// AMS 页(最多 4 台 × 4 槽)
static lv_obj_t *s_ams_title[4], *s_ams_sub[4];
static lv_obj_t *s_ams_swatch[4][4], *s_ams_type[4][4], *s_ams_remain[4][4], *s_ams_mark[4][4];
static lv_obj_t *s_ams_gauge[4][4];     // 槽位右侧余量竖条
// 设备页
static lv_obj_t *s_d_sn, *s_d_rssi, *s_d_ip, *s_d_cloud, *s_d_bat, *s_d_wifi, *s_d_uptime;
static lv_obj_t *s_d_hms_panel, *s_d_hms_lbl;
// 控制页
static lv_obj_t *s_ctl_title, *s_ctl_items[3], *s_ctl_labels[3], *s_ctl_state;
static lv_obj_t *s_ctl_arm_bar;   // 停止二次确认倒计时条
static int  s_ctl_sel;
static bool s_stop_armed;
static int64_t s_stop_armed_at;   // esp_timer us
static bool s_light_on;           // 乐观腔灯状态

// 上次打印状态(用于状态切换提示音)
static char s_last_state[10];

// 脏检查:数据快照没变时跳过整页重绘,只刷时钟/电量(省电、避免无谓重排)
static bambu_state_t s_prev;
static bool s_prev_valid;
static bool s_dirty = true;       // 按键等交互强制重绘

static void refresh_all(void);    // 前向声明(tick_fast 要调,定义在后面)
static void ov_refresh(void);     // 前向声明(refresh_all 概览模式要调)
static void detail_build(void);   // 前向声明(概览→详情切换要用)
static void detail_free(void);    // 前向声明(详情→概览切换要用)
static void show_page(int idx);   // 前向声明(ov_key 进详情要调)
static void key_handle(bsp_btn_t btn, bsp_btn_ev_t ev);

// 按键回调跑在 button 组件的小栈任务(默认 2~3KB)里,而翻页/重绘很重:
// refresh_all() 会把 bambu_state_t 大结构体上栈 + memcpy + 一堆 LVGL/字体调用,
// 在回调里直接干会爆栈 → 看门狗/栈溢出 → 设备重启(「按两回上键就重启」的根因)。
// 所以按键回调只记下事件,由 s_fast 定时器在 LVGL 任务(栈足够)里真正执行。
static void request_refresh(void) { s_dirty = true; s_need_refresh = true; }

static void tick_fast(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;
    if (s_pk_valid) {                       // 先消费挂起的按键(在 LVGL 任务里跑)
        bsp_btn_t b = s_pk_btn; bsp_btn_ev_t e = s_pk_ev;
        s_pk_valid = false;
        key_handle(b, e);
    }
    if (s_need_refresh) { s_need_refresh = false; refresh_all(); }
}

// ------------------------------------------------------------ 基础控件

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                       lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

static void set_bar_pct(lv_obj_t *bar, int pct, int max_w)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_obj_set_width(bar, max_w * pct / 100);
}

// 目标温度刻度线:竖线画在进度条上对应目标值的位置
static void set_bar_mark(lv_obj_t *mark, int pct, int max_w, int x0)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_obj_set_x(mark, x0 + max_w * pct / 100 - 1);
}

// 圆角卡片(带细边框)
static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *o = rect(parent, x, y, w, h, bg);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_LINE), 0);
    return o;
}

// 状态标签 chip:左标签 + 右等宽数值(可指定值颜色)
static void chip_build(lv_obj_t *parent, int x, int y, int w, int h,
                       const char *k, uint32_t val_col, lv_obj_t **kl, lv_obj_t **vl)
{
    lv_obj_t *o = rect(parent, x, y, w, h, C_PANEL2);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_LINE), 0);
    *kl = label(o, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(*kl, k);
    lv_obj_align(*kl, LV_ALIGN_LEFT_MID, 7, 0);
    *vl = label(o, &lv_font_montserrat_14, val_col, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(*vl, "-");
    lv_obj_align(*vl, LV_ALIGN_RIGHT_MID, -7, 0);
}

// ------------------------------------------------------------ 像素大数字

// 4x7 点阵数字(行值 4bit,高位在左)
static const uint8_t DIG[10][7] = {
    { 6,  9,  9,  9,  9,  9,  6 }, // 0
    { 2,  6,  2,  2,  2,  2,  7 }, // 1
    { 6,  9,  1,  2,  4,  8, 15 }, // 2
    { 14, 1,  2,  6,  2,  1, 14 }, // 3
    { 2,  6,  10, 9,  15, 1,  1 }, // 4
    { 15, 8,  14, 1,  1,  9,  6 }, // 5
    { 6,  8,  14, 9,  9,  9,  6 }, // 6
    { 15, 1,  2,  4,  4,  4,  4 }, // 7
    { 6,  9,  6,  9,  9,  9,  6 }, // 8
    { 6,  9,  9,  9,  7,  1,  6 }, // 9
};

// 在容器里重画一串数字,scale=单格像素
static void draw_digits(lv_obj_t *box, const char *txt, int scale, uint32_t color)
{
    lv_obj_clean(box);
    int adv = 5 * scale;
    int x = 0;
    for (const char *p = txt; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        const uint8_t *pat = DIG[*p - '0'];
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 4; c++) {
                if (pat[r] & (1 << (3 - c))) {
                    rect(box, x + c * scale, r * scale, scale, scale, color);
                }
            }
        }
        x += adv;
    }
}

static int digits_width(int n, int scale)
{
    return n * 5 * scale;
}

// 状态 → 中文/颜色
static const char *state_zh(const char *st, uint32_t *col)
{
    if (!st || !st[0] || !strcmp(st, "IDLE"))  { *col = C_DIM;   return "空闲"; }
    if (!strcmp(st, "RUNNING"))                { *col = C_GREEN; return "打印中"; }
    if (!strcmp(st, "PAUSE"))                  { *col = C_YELLOW; return "已暂停"; }
    if (!strcmp(st, "FAILED"))                 { *col = C_RED;   return "打印失败"; }
    if (!strcmp(st, "FINISH"))                 { *col = C_CYAN;  return "打印完成"; }
    *col = C_DIM;
    return st;
}

static uint32_t state_ring_color(const char *st)
{
    if (!st || !st[0] || !strcmp(st, "IDLE"))  return C_DIM;
    if (!strcmp(st, "RUNNING"))                return C_GREEN;
    if (!strcmp(st, "PAUSE"))                  return C_YELLOW;
    if (!strcmp(st, "FAILED"))                 return C_RED;
    if (!strcmp(st, "FINISH"))                 return C_CYAN;
    return C_DIM;
}

// ------------------------------------------------------------ 头部

static void header_build(void)
{
    rect(s_scr, 0, 0, 240, 30, C_PANEL);
    rect(s_scr, 0, 30, 240, 2, C_LINE);

    // 实时时钟(左上)
    s_time = label(s_scr, &lv_font_montserrat_14, C_GREEN_L, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_time, "--:--");
    lv_obj_align(s_time, LV_ALIGN_TOP_LEFT, 8, 7);

    // 云/网络状态点(时钟右侧):绿=云在线 黄=连接中 灰=离线 红=WiFi 未连
    s_cloud_dot = rect(s_scr, 54, 12, 6, 6, C_DIM);
    lv_obj_set_style_radius(s_cloud_dot, LV_RADIUS_CIRCLE, 0);

    // 页点(居中,最多 8 个,按实际页数显示)
    for (int i = 0; i < PAGE_MAX; i++) {
        s_dots[i] = rect(s_scr, 88 + i * 10, 13, 5, 5, C_LINE);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
    }

    // 电量:图标右置(210-236),百分比文字在图标左侧,互不重叠
    s_bat_pct = label(s_scr, &lv_font_montserrat_14, C_DIM, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_bat_pct, "-");
    lv_obj_align(s_bat_pct, LV_ALIGN_TOP_RIGHT, -34, 7);

    lv_obj_t *body = rect(s_scr, 210, 9, 24, 13, C_BG);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_radius(body, 2, 0);
    s_bat_fill = rect(body, 0, 0, 10, 9, C_GREEN);
    lv_obj_set_style_radius(s_bat_fill, 0, 0);
    rect(s_scr, 236, 12, 3, 7, C_DIM);
}

// 常驻状态条(header 下方 18px):有 HMS 显示告警,否则显示云/WiFi 摘要。
// 常驻而非"有告警才弹出":告警时不会盖住页面内容,布局也不会跳变。
static void status_bar_build(void)
{
    s_bar = rect(s_scr, 0, BAR_Y, 240, BAR_H, C_PANEL);
    lv_obj_set_style_radius(s_bar, 0, 0);
    rect(s_scr, 0, BAR_Y + BAR_H, 240, 1, C_LINE);
    s_bar_lbl = label(s_bar, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_bar_lbl, "等待数据");
    lv_label_set_long_mode(s_bar_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_bar_lbl, 226);
    lv_obj_align(s_bar_lbl, LV_ALIGN_LEFT_MID, 7, 0);
}

static void refresh_status_bar(const bambu_state_t *st)
{
    // 多机时前缀机器名,单机保持原样
    const char *cust = app_mqtt_name(s_midx);
    int machines = app_mqtt_machine_count();
    if (st->hms_count > 0) {
        static char buf[MSG_MAX];
        const char *txt = hms_lookup(st->hms[0].code, buf, sizeof(buf));
        if (cust && machines >= 2) {
            static char t2[MSG_MAX + 24];
            snprintf(t2, sizeof(t2), "%s · %s", cust, txt);
            lv_label_set_text(s_bar_lbl, t2);
        } else {
            lv_label_set_text(s_bar_lbl, txt);
        }
        bool paused = st->gcode_state[0] && !strcmp(st->gcode_state, "PAUSE");
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(paused ? C_YELLOW : C_RED), 0);
        lv_obj_set_style_text_color(s_bar_lbl,
            lv_color_hex(paused ? 0x241A00 : 0xFFFFFF), 0);
    } else {
        const char *cloud = st->cloud == BAMBU_CLOUD_ONLINE    ? "云在线"
                          : st->cloud == BAMBU_CLOUD_CONNECTING ? "云连接中" : "云离线";
        static char t[56];
        if (cust && machines >= 2) {
            if (st->wifi_signal) snprintf(t, sizeof(t), "%s · %s · %d dBm", cust, cloud, st->wifi_signal);
            else                 snprintf(t, sizeof(t), "%s · %s · 信号 --", cust, cloud);
        } else {
            if (st->wifi_signal) snprintf(t, sizeof(t), "%s · 打印机 %d dBm", cloud, st->wifi_signal);
            else                 snprintf(t, sizeof(t), "%s · 打印机信号 --", cloud);
        }
        lv_label_set_text(s_bar_lbl, t);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_text_color(s_bar_lbl, lv_color_hex(C_DIM), 0);
    }
}

// ------------------------------------------------------------ 进度页

static void page_progress_build(lv_obj_t *p)
{
    s_status = label(p, &font_zh20, C_GREEN, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_status, "空闲");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 0);

    s_job = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_job, "等待任务");
    lv_obj_align(s_job, LV_ALIGN_TOP_MID, 0, 22);
    lv_label_set_long_mode(s_job, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_job, 220);

    // 大号居中百分比(像素数字) + 横向圆角进度条(替代原折线圆环)
    s_pct_box = rect(p, 0, 0, 4, 4, C_BG);
    lv_obj_set_style_bg_opa(s_pct_box, LV_OPA_TRANSP, 0);
    draw_digits(s_pct_box, "0", 7, C_TEXT);
    lv_obj_set_pos(s_pct_box, 0, 0);

    s_pct_sign = label(p, &font_zh20, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_pct_sign, "%");
    lv_obj_set_style_text_font(s_pct_sign, &font_zh20, 0);

    // 横向进度条:轨道(圆角卡片) + 填充
    s_prog_track = card(p, 20, 150, 200, 16, C_PANEL2);
    s_prog_fill  = rect(p, 22, 152, 0, 12, C_GREEN);
    lv_obj_set_style_radius(s_prog_fill, 6, 0);

    // 温度/层/风扇 chip(2x2)
    lv_obj_t *kl;
    chip_build(p, 14, 188, 108, 20, "喷嘴", C_ORANGE, &kl, &s_c_noz_v);
    chip_build(p, 118, 188, 108, 20, "热床", C_GREEN_L, &kl, &s_c_bed_v);
    chip_build(p, 14, 210, 108, 20, "层", C_CYAN, &kl, &s_c_layer_v);
    chip_build(p, 118, 210, 108, 20, "风扇", C_DIM, &kl, &s_c_fan_v);

    // 剩余 / 完成时刻(OK 切换)
    s_time_lbl = label(p, &font_zh20, C_GREEN_L, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_time_lbl, "--");
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_MID, 0, 232);

    s_time_hint = label(p, &font_zh14, 0x445262, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_time_hint, "OK 切换剩余/完成");
    lv_obj_align(s_time_hint, LV_ALIGN_TOP_MID, 0, 254);
}

static void refresh_progress(const bambu_state_t *st)
{
    uint32_t st_col;
    const char *zh = state_zh(st->gcode_state, &st_col);
    lv_label_set_text(s_status, zh);
    lv_obj_set_style_text_color(s_status, lv_color_hex(st_col), 0);
    lv_label_set_text(s_job, st->job[0] ? st->job : "等待任务");

    // 大号百分比 + 横向进度条(替代折线圆环)
    uint32_t num_col = state_ring_color(st->gcode_state);
    char dts[8];
    snprintf(dts, sizeof(dts), "%d", st->pct);
    int n = strlen(dts);
    int scale = 7;
    int dig_h = 7 * scale;
    int num_w = digits_width(n, scale);
    int pct_w = 18, gap = 6;
    int total = num_w + gap + pct_w;
    int sx = (240 - total) / 2;
    int cy = 95;
    lv_obj_set_size(s_pct_box, num_w, dig_h);
    lv_obj_set_pos(s_pct_box, sx, cy - dig_h / 2);
    draw_digits(s_pct_box, dts, scale, num_col);
    lv_obj_set_pos(s_pct_sign, sx + num_w + gap, cy - 11);
    lv_obj_set_style_text_color(s_pct_sign, lv_color_hex(num_col), 0);

    // 进度条填充(轨道内 2px 边距,最大宽 196)
    int fill_w = (196 * st->pct) / 100;
    if (fill_w < 0) fill_w = 0; else if (fill_w > 196) fill_w = 196;
    lv_obj_set_width(s_prog_fill, fill_w);
    lv_obj_set_style_bg_color(s_prog_fill, lv_color_hex(num_col), 0);

    // 温度/层/风扇 chip(温度显示 当前/目标)
    char t[24];
    snprintf(t, sizeof(t), "%d/%d", st->nozzle_t, st->nozzle_target);
    lv_label_set_text(s_c_noz_v, t);
    snprintf(t, sizeof(t), "%d/%d", st->bed_t, st->bed_target);
    lv_label_set_text(s_c_bed_v, t);
    snprintf(t, sizeof(t), "%d/%d", st->layer, st->total_layer);
    lv_label_set_text(s_c_layer_v, t);
    snprintf(t, sizeof(t), "%d%%", st->fan_cooling_pct);
    lv_label_set_text(s_c_fan_v, t);

    // 剩余 ↔ 完成时刻
    char t2[64];
    if (st->remain_min > 0) {
        if (!s_show_eta) {
            int h = st->remain_min / 60, m = st->remain_min % 60;
            if (h > 0) snprintf(t2, sizeof(t2), "剩余 %dh%02dm", h, m);
            else       snprintf(t2, sizeof(t2), "剩余 %dm", m);
            lv_label_set_text(s_time_hint, "OK 切换完成时刻");
        } else if (app_wifi_time_ready()) {
            time_t eta = time(NULL) + (time_t)st->remain_min * 60;
            struct tm tmv;
            localtime_r(&eta, &tmv);
            snprintf(t2, sizeof(t2), "%02d:%02d 完成", tmv.tm_hour, tmv.tm_min);
            lv_label_set_text(s_time_hint, "OK 切换剩余时间");
        } else {
            snprintf(t2, sizeof(t2), "--:-- 完成");
            lv_label_set_text(s_time_hint, "OK 切换剩余时间");
        }
        lv_label_set_text(s_time_lbl, t2);
    } else {
        lv_label_set_text(s_time_lbl, "--");
        lv_label_set_text(s_time_hint, "OK 切换剩余/完成");
    }
}

// ------------------------------------------------------------ 温度页

static void page_temp_build(lv_obj_t *p)
{
    // 喷头
    card(p, 10, 2, 220, 62, C_PANEL);
    lv_obj_t *n1 = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(n1, "喷头"); lv_obj_align(n1, LV_ALIGN_TOP_LEFT, 20, 8);
    s_noz_cur = label(p, &lv_font_montserrat_20, C_ORANGE, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_noz_cur, "--");
    lv_obj_align(s_noz_cur, LV_ALIGN_TOP_RIGHT, -20, 4);
    s_noz_tgt = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_noz_tgt, "目标 --");
    lv_obj_align(s_noz_tgt, LV_ALIGN_TOP_RIGHT, -20, 30);
    rect(p, 20, 50, 200, 6, C_LINE);
    s_noz_bar = rect(p, 20, 50, 0, 6, C_ORANGE);
    s_noz_mark = rect(p, 20, 45, 2, 16, C_TEXT);

    // 热床
    card(p, 10, 68, 220, 62, C_PANEL);
    lv_obj_t *n2 = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(n2, "热床"); lv_obj_align(n2, LV_ALIGN_TOP_LEFT, 20, 74);
    s_bed_cur = label(p, &lv_font_montserrat_20, C_GREEN_L, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_bed_cur, "--");
    lv_obj_align(s_bed_cur, LV_ALIGN_TOP_RIGHT, -20, 70);
    s_bed_tgt = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_bed_tgt, "目标 --");
    lv_obj_align(s_bed_tgt, LV_ALIGN_TOP_RIGHT, -20, 96);
    rect(p, 20, 116, 200, 6, C_LINE);
    s_bed_bar = rect(p, 20, 116, 0, 6, C_GREEN_L);
    s_bed_mark = rect(p, 20, 111, 2, 16, C_TEXT);

    // 腔温
    card(p, 10, 134, 220, 58, C_PANEL);
    lv_obj_t *n3 = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(n3, "腔温"); lv_obj_align(n3, LV_ALIGN_TOP_LEFT, 20, 148);
    s_cham_cur = label(p, &lv_font_montserrat_20, C_CYAN, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_cham_cur, "--");
    lv_obj_align(s_cham_cur, LV_ALIGN_TOP_RIGHT, -20, 144);
    rect(p, 20, 176, 200, 6, C_LINE);
    s_cham_bar = rect(p, 20, 176, 0, 6, C_CYAN);

    // 预计剩余
    card(p, 10, 196, 220, 58, C_PANEL);
    lv_obj_t *n4 = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(n4, "预计剩余"); lv_obj_align(n4, LV_ALIGN_TOP_LEFT, 20, 214);
    s_temp_remain = label(p, &lv_font_montserrat_20, C_GREEN_L, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_temp_remain, "--");
    lv_obj_align(s_temp_remain, LV_ALIGN_TOP_RIGHT, -20, 210);
}

static void refresh_temp(const bambu_state_t *st)
{
    char t[24];
    // 量程跟随目标温度:PLA 热床 60° 时条也能走满,不再看着像"只到一半"
    int noz_max = st->nozzle_target > 0 ? st->nozzle_target + 30 : 300;
    int bed_max = st->bed_target > 0 ? st->bed_target + 20 : 120;
    if (noz_max < 200) noz_max = 200;
    if (bed_max < 60)  bed_max = 60;

    snprintf(t, sizeof(t), "%d°", st->nozzle_t);
    lv_label_set_text(s_noz_cur, t);
    snprintf(t, sizeof(t), "目标 %d°", st->nozzle_target);
    lv_label_set_text(s_noz_tgt, t);
    set_bar_pct(s_noz_bar, st->nozzle_t * 100 / noz_max, 200);
    set_bar_mark(s_noz_mark, st->nozzle_target * 100 / noz_max, 200, 20);

    snprintf(t, sizeof(t), "%d°", st->bed_t);
    lv_label_set_text(s_bed_cur, t);
    snprintf(t, sizeof(t), "目标 %d°", st->bed_target);
    lv_label_set_text(s_bed_tgt, t);
    set_bar_pct(s_bed_bar, st->bed_t * 100 / bed_max, 200);
    set_bar_mark(s_bed_mark, st->bed_target * 100 / bed_max, 200, 20);

    // 腔温 0 = 机型无腔温传感器(未上报),显示 -- 而非误导性的 0°
    if (st->chamber_t > 0) snprintf(t, sizeof(t), "%d°", st->chamber_t);
    else                   strcpy(t, "--");
    lv_label_set_text(s_cham_cur, t);
    set_bar_pct(s_cham_bar, st->chamber_t * 100 / 60, 200);   // 腔温量程 60℃

    if (st->remain_min > 0) {
        int h = st->remain_min / 60, m = st->remain_min % 60;
        if (h > 0) snprintf(t, sizeof(t), "%dh%02dm", h, m);
        else       snprintf(t, sizeof(t), "%dm", m);
    } else {
        strcpy(t, "--");
    }
    lv_label_set_text(s_temp_remain, t);
}

// ------------------------------------------------------------ AMS 料槽页

static void page_ams_build(lv_obj_t *p, int ams_idx)
{
    // 标题(刷新时更新台数/湿度)
    s_ams_title[ams_idx] = label(p, &font_zh14, C_GREEN_L, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ams_title[ams_idx], "耗材 · AMS");
    lv_obj_align(s_ams_title[ams_idx], LV_ALIGN_TOP_MID, 0, 4);
    // 副标题:当前在用槽的完整料名(卡片里只放得下材质简称)
    s_ams_sub[ams_idx] = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ams_sub[ams_idx], "在用 --");
    lv_label_set_long_mode(s_ams_sub[ams_idx], LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_ams_sub[ams_idx], 220);
    lv_obj_align(s_ams_sub[ams_idx], LV_ALIGN_TOP_MID, 0, 20);

    // 2x2 料槽卡片(每张 100x98)
    static const int GX[4] = { 14, 126 };
    static const int GY[4] = { 44, 148 };
    for (int i = 0; i < 4; i++) {
        int col = i % 2, row = i / 2;
        int x = GX[col], y = GY[row];
        card(p, x, y, 100, 98, C_PANEL2);

        s_ams_swatch[ams_idx][i] = rect(p, x + 9, y + 8, 58, 52, 0x141C26);
        lv_obj_set_style_radius(s_ams_swatch[ams_idx][i], 6, 0);
        lv_obj_set_style_border_width(s_ams_swatch[ams_idx][i], 2, 0);
        lv_obj_set_style_border_color(s_ams_swatch[ams_idx][i], lv_color_hex(C_LINE), 0);

        // 右侧余量竖条(满量程 1000g),填充自底向上
        lv_obj_t *track = rect(p, x + 73, y + 8, 12, 52, C_LINE);
        lv_obj_set_style_radius(track, 3, 0);
        s_ams_gauge[ams_idx][i] = rect(track, 0, 0, 12, 0, C_GREEN_L);
        lv_obj_set_style_radius(s_ams_gauge[ams_idx][i], 3, 0);
        lv_obj_align(s_ams_gauge[ams_idx][i], LV_ALIGN_BOTTOM_MID, 0, 0);

        s_ams_type[ams_idx][i] = label(p, &font_zh14, C_TEXT, LV_TEXT_ALIGN_LEFT);
        lv_label_set_long_mode(s_ams_type[ams_idx][i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_ams_type[ams_idx][i], 84);   // 材质/料名过长省略号截断
        lv_obj_align(s_ams_type[ams_idx][i], LV_ALIGN_TOP_LEFT, x + 9, y + 64);

        s_ams_remain[ams_idx][i] = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
        lv_obj_align(s_ams_remain[ams_idx][i], LV_ALIGN_TOP_LEFT, x + 9, y + 82);

        // 活动标记(色块右上角绿点)
        s_ams_mark[ams_idx][i] = rect(p, x + 60, y + 2, 8, 8, C_GREEN);
        lv_obj_set_style_radius(s_ams_mark[ams_idx][i], LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(s_ams_mark[ams_idx][i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_ams(const bambu_state_t *st, int ams_idx, int total_ams)
{
    const bambu_ams_t *a = &st->ams[ams_idx];
    char t[64];

    if (!a->present) {
        // 外部料筒
        lv_label_set_text(s_ams_title[ams_idx], "外部料筒");
    } else {
        if (a->humidity_raw > 0 && a->temp > 0)
            snprintf(t, sizeof(t), "AMS %d · %d台 · 湿%d%% · %d°",
                     ams_idx + 1, total_ams, a->humidity_raw, a->temp);
        else if (a->temp > 0)
            snprintf(t, sizeof(t), "AMS %d · %d台 · 干燥%d · %d°",
                     ams_idx + 1, total_ams,
                     a->humidity >= 0 ? a->humidity : 0, a->temp);
        else
            snprintf(t, sizeof(t), "AMS %d · %d台 · 干燥%d",
                     ams_idx + 1, total_ams,
                     a->humidity >= 0 ? a->humidity : 0);
        lv_label_set_text(s_ams_title[ams_idx], t);
    }

    // 副标题:卡片里只有 6 个字的空间,完整料名放这里
    char sub[56];
    const char *active_name = NULL;
    for (int i = 0; i < 4; i++) {
        if (a->tray[i].active && a->tray[i].is_used) {
            // 手动耗材没有 tray_id_name,退回材质名(如 PLA),别显示空的"在用 "
            active_name = a->tray[i].tray_name[0] ? a->tray[i].tray_name
                                                  : a->tray[i].tray_type;
            break;
        }
    }
    if (active_name && active_name[0]) snprintf(sub, sizeof(sub), "在用 %s", active_name);
    else if (!a->present)              snprintf(sub, sizeof(sub), "外部料筒 · 无 AMS 数据");
    else                               snprintf(sub, sizeof(sub), "在用 --");
    lv_label_set_text(s_ams_sub[ams_idx], sub);

    for (int i = 0; i < 4; i++) {
        const bambu_tray_t *slot = &a->tray[i];
        bool used = slot->is_used;
        // 色块(color_valid 区分"无色"与纯黑,纯黑料显示真黑色)
        uint32_t swc = used ? (slot->color_valid ? slot->tray_color : 0x22303F)
                            : 0x141C26;
        lv_obj_set_style_bg_color(s_ams_swatch[ams_idx][i], lv_color_hex(swc), 0);
        // 活动槽高亮边框
        uint32_t bcol = slot->active ? C_GREEN_L : C_LINE;
        lv_obj_set_style_border_color(s_ams_swatch[ams_idx][i], lv_color_hex(bcol), 0);
        // 活动标记(右上角绿点)
        if (slot->active) lv_obj_remove_flag(s_ams_mark[ams_idx][i], LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag(s_ams_mark[ams_idx][i], LV_OBJ_FLAG_HIDDEN);

        // 有 tray_type 显示材质(PLA/PETG);X2D/AMS2 只有 tray_name(料名),用青色区分
        const char *label_txt = "-";
        if (used)
            label_txt = slot->tray_type[0] ? slot->tray_type : slot->tray_name;
        lv_label_set_text(s_ams_type[ams_idx][i], label_txt);
        lv_obj_set_style_text_color(s_ams_type[ams_idx][i],
            lv_color_hex(slot->tray_type[0] ? C_TEXT : C_CYAN), 0);
        if (used) {
            // 有余量数据 → 克数(整卷克数 × 剩余百分比);整卷克数未知 → 百分比;
            // 手动耗材/无 RFID 料卷 remain=-1 → 余量未知显示 "--"(材质与颜色照常显示)
            if (slot->tray_pct >= 0)
                snprintf(t, sizeof(t), slot->tray_weight > 0 ? "%dg" : "%d%%",
                         slot->tray_weight > 0 ? slot->tray_remain : slot->tray_pct);
            else
                strcpy(t, "--");
        } else {
            strcpy(t, "空槽");
        }
        lv_label_set_text(s_ams_remain[ams_idx][i], t);

        // 余量竖条按整卷百分比(0~100)填充:250g 小卷半满也显示半格,
        // 不会像按"克/1000"那样几乎全空。低于 20% 转黄提醒。
        int p = slot->tray_pct;
        int gh = (p > 0) ? (p > 100 ? 52 : p * 52 / 100) : 0;
        lv_obj_set_height(s_ams_gauge[ams_idx][i], gh);
        lv_obj_align(s_ams_gauge[ams_idx][i], LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(s_ams_gauge[ams_idx][i],
            lv_color_hex(p > 20 ? C_GREEN_L : (p > 0 ? C_YELLOW : C_LINE)), 0);
    }
}

// ------------------------------------------------------------ 设备页

static void device_line(lv_obj_t *p, int y, const char *k, lv_obj_t **val)
{
    lv_obj_t *kl = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(kl, k);
    lv_obj_align(kl, LV_ALIGN_TOP_LEFT, 14, y);
    *val = label(p, &font_zh14, C_TEXT, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(*val, LV_ALIGN_TOP_RIGHT, -14, y);
}

static void page_device_build(lv_obj_t *p)
{
    device_line(p, 6,  "打印机", &s_d_sn);
    device_line(p, 30, "打印机WiFi", &s_d_rssi);
    device_line(p, 54, "打印机IP", &s_d_ip);
    device_line(p, 78, "云连接", &s_d_cloud);
    device_line(p, 102,"在线时长", &s_d_uptime);
    device_line(p, 126,"通行证WiFi", &s_d_wifi);
    device_line(p, 150,"通行证电量", &s_d_bat);

    // HMS 告警面板
    s_d_hms_panel = rect(p, 10, 170, 220, 72, 0x1A1418);
    lv_obj_set_style_radius(s_d_hms_panel, 6, 0);
    lv_obj_set_style_border_width(s_d_hms_panel, 1, 0);
    lv_obj_set_style_border_color(s_d_hms_panel, lv_color_hex(0x3A2226), 0);
    s_d_hms_lbl = label(s_d_hms_panel, &font_zh14, C_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_d_hms_lbl, "告警:无");
    lv_obj_align(s_d_hms_lbl, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_label_set_long_mode(s_d_hms_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_d_hms_lbl, 204);

    // 固件版本(底部固定)
    lv_obj_t *ver = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text_fmt(ver, "bambumonitor v%s", FW_VERSION);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void refresh_device(const bambu_state_t *st)
{
    char t[40];
    lv_label_set_text(s_d_sn, app_mqtt_sn(s_midx));
    snprintf(t, sizeof(t), "%d dBm", st->wifi_signal);
    lv_label_set_text(s_d_rssi, t);

    if (st->printer_ip) {
        uint8_t *b = (uint8_t *)&st->printer_ip;
        snprintf(t, sizeof(t), "%u.%u.%u.%u", b[3], b[2], b[1], b[0]);
    } else {
        strcpy(t, "-");
    }
    lv_label_set_text(s_d_ip, t);

    switch (st->cloud) {
    case BAMBU_CLOUD_ONLINE:
        strcpy(t, "已连接");
        lv_obj_set_style_text_color(s_d_cloud, lv_color_hex(C_GREEN_L), 0);
        break;
    case BAMBU_CLOUD_CONNECTING:
        strcpy(t, "连接中");
        lv_obj_set_style_text_color(s_d_cloud, lv_color_hex(C_YELLOW), 0);
        break;
    default:
        strcpy(t, "离线");
        lv_obj_set_style_text_color(s_d_cloud, lv_color_hex(C_DIM), 0);
        break;
    }
    lv_label_set_text(s_d_cloud, t);

    int up = app_mqtt_uptime_sec();
    if (up >= 3600) snprintf(t, sizeof(t), "%dh%02dm", up / 3600, (up % 3600) / 60);
    else             snprintf(t, sizeof(t), "%dm%02ds", up / 60, up % 60);
    lv_label_set_text(s_d_uptime, t);

    if (app_wifi_connected()) {
        snprintf(t, sizeof(t), "%d dBm", app_wifi_rssi());
    } else {
        strcpy(t, "未连接");
    }
    lv_label_set_text(s_d_wifi, t);

    snprintf(t, sizeof(t), "%d mV", bsp_battery_mv());
    lv_label_set_text(s_d_bat, t);

    // HMS 告警面板
    if (st->hms_count > 0) {
        snprintf(t, sizeof(t), "告警 %d 条", st->hms_count);
        for (int i = 0; i < st->hms_count; i++) {
            char one[MSG_MAX];
            const char *txt = hms_lookup(st->hms[i].code, one, sizeof(one));
            if (strlen(t) + strlen(txt) + 4 < sizeof(t)) {
                strcat(t, "\n");
                strcat(t, txt);
            }
        }
        lv_label_set_text(s_d_hms_lbl, t);
        lv_obj_set_style_bg_color(s_d_hms_panel, lv_color_hex(
            (st->gcode_state[0] && !strcmp(st->gcode_state, "PAUSE")) ? 0x1A180D : 0x1A1418), 0);
    } else {
        lv_label_set_text(s_d_hms_lbl, "告警:无");
        lv_obj_set_style_bg_color(s_d_hms_panel, lv_color_hex(0x141C18), 0);
    }
}

// ------------------------------------------------------------ 控制页

// 控制项文本动态生成,返回是否可用
static const char *ctl_item_text(int idx, const char *st, bool *enabled)
{
    *enabled = true;
    if (idx == 0) {
        if (!strcmp(st, "RUNNING")) return "暂停打印";
        if (!strcmp(st, "PAUSE"))   return "继续打印";
        *enabled = false;
        return "暂停打印";
    }
    if (idx == 1) {
        if (!strcmp(st, "RUNNING") || !strcmp(st, "PAUSE")) return "停止打印";
        *enabled = false;
        return "停止打印";
    }
    *enabled = true;
    return s_light_on ? "腔灯: 开" : "腔灯: 关";
}

static void ctl_refresh_items(const bambu_state_t *st)
{
    for (int i = 0; i < 3; i++) {
        bool en;
        const char *txt = ctl_item_text(i, st->gcode_state, &en);
        lv_label_set_text(s_ctl_labels[i], txt);
        // 停止二次确认中的红色提示
        bool armed = (i == 1 && s_stop_armed);
        uint32_t col = armed ? C_RED : (en ? C_TEXT : 0x4A5A50);
        lv_obj_set_style_text_color(s_ctl_labels[i], lv_color_hex(col), 0);
        // 选中高亮
        bool sel = (i == s_ctl_sel);
        lv_obj_set_style_bg_color(s_ctl_items[i],
            lv_color_hex(sel ? 0x14321F : C_PANEL), 0);
        lv_obj_set_style_border_color(s_ctl_items[i],
            lv_color_hex(sel ? C_GREEN : C_LINE), 0);
    }
    if (s_stop_armed) {
        int64_t left_us = 3 * 1000000LL - (esp_timer_get_time() - s_stop_armed_at);
        if (left_us < 0) left_us = 0;
        int sec = (int)((left_us + 999999) / 1000000);
        lv_label_set_text(s_ctl_labels[1], "再按确认停止!");
        lv_label_set_text_fmt(s_ctl_state, "再按 OK 停止 · 剩 %d 秒", sec);
        lv_obj_set_width(s_ctl_arm_bar, (int)(left_us * 192 / (3 * 1000000LL)));
        lv_obj_remove_flag(s_ctl_arm_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_ctl_state, "上下选择 · OK 执行");
        lv_obj_add_flag(s_ctl_arm_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void page_control_build(lv_obj_t *p)
{
    s_ctl_title = label(p, &font_zh20, C_GREEN, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ctl_title, "打印控制");
    lv_obj_align(s_ctl_title, LV_ALIGN_TOP_MID, 0, 6);

    s_ctl_state = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ctl_state, "上下选择 · OK 执行");
    lv_obj_align(s_ctl_state, LV_ALIGN_TOP_MID, 0, 30);

    static const char *items[3] = { "暂停打印", "停止打印", "腔灯: 关" };
    for (int i = 0; i < 3; i++) {
        s_ctl_items[i] = rect(p, 24, 52 + i * 40, 192, 34, C_PANEL);
        lv_obj_set_style_radius(s_ctl_items[i], 6, 0);
        lv_obj_set_style_border_width(s_ctl_items[i], 1, 0);
        lv_obj_set_style_border_color(s_ctl_items[i], lv_color_hex(C_LINE), 0);
        s_ctl_labels[i] = label(s_ctl_items[i], &font_zh14, C_TEXT, LV_TEXT_ALIGN_LEFT);
        lv_label_set_text(s_ctl_labels[i], items[i]);
        lv_obj_align(s_ctl_labels[i], LV_ALIGN_LEFT_MID, 14, 0);
    }

    // 停止二次确认倒计时条(3 秒窗口可视化)
    lv_obj_t *arm_track = rect(p, 24, 176, 192, 6, C_LINE);
    lv_obj_set_style_radius(arm_track, 3, 0);
    s_ctl_arm_bar = rect(p, 24, 176, 0, 6, C_RED);
    lv_obj_set_style_radius(s_ctl_arm_bar, 3, 0);
    lv_obj_add_flag(s_ctl_arm_bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = label(p, &font_zh14, 0x445262, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(hint, "顶/底再按 = 翻页");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ------------------------------------------------------------ 刷新

static void refresh_header(const bambu_state_t *st)
{
    // 云/网络状态点:绿=云在线 黄=连接中 红=本机 WiFi 未连 灰=离线
    uint32_t dc;
    if (!app_wifi_connected())                    dc = C_RED;
    else if (st->cloud == BAMBU_CLOUD_ONLINE)     dc = C_GREEN;
    else if (st->cloud == BAMBU_CLOUD_CONNECTING) dc = C_YELLOW;
    else                                          dc = C_DIM;
    lv_obj_set_style_bg_color(s_cloud_dot, lv_color_hex(dc), 0);

    // 实时时钟(SNTP 未同步显示 --:--)
    if (app_wifi_time_ready()) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char ts[8];
        snprintf(ts, sizeof(ts), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        lv_label_set_text(s_time, ts);
    } else {
        lv_label_set_text(s_time, "--:--");
    }

    for (int i = 0; i < PAGE_MAX; i++) {
        bool on = (i < s_pages_n);
        if (i == s_cur) lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(C_GREEN), 0);
        else            lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(C_LINE), 0);
        if (on) lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
    }

    int soc = bsp_battery_soc();
    int mv  = bsp_battery_mv();

    // SOC 无效(多为 USB 供电无有效电量计读数)时,用电压线性估算兜底,避免常驻 “-”
    if (soc < 0 && mv >= 3000 && mv <= 4300) {
        int est = (mv - 3000) * 100 / (4200 - 3000);
        if (est < 0) est = 0; else if (est > 100) est = 100;
        soc = est;
    }

    char t[8];
    snprintf(t, sizeof(t), "%d%%", soc);
    lv_label_set_text(s_bat_pct, soc >= 0 ? t : "-");
    uint32_t fc = soc > 50 ? C_GREEN : (soc > 20 ? C_YELLOW : C_RED);
    lv_obj_set_width(s_bat_fill, (soc < 0 ? 0 : soc) * 20 / 100);   // 电池内部净宽 20
    lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(fc), 0);
}

static void refresh_all(void)
{
    // 概览列表模式:只刷列表卡片,不动详情页
    if (s_ov_mode) {
        ov_refresh();
        return;
    }
    bambu_state_t st;
    app_mqtt_snapshot(s_midx, &st);

    // 脏检查:快照没变、无交互、无倒计时,就只刷头部(时钟/电量/页点),不重排整页
    bool changed = !s_prev_valid || memcmp(&s_prev, &st, sizeof(st)) != 0;
    if (changed) { s_prev = st; s_prev_valid = true; }

    // 动态页数:2 固定 + AMS 台数;已收到数据但 ams_count=0(外部料筒)也保留 1 页
    int ams_pages = st.ams_count;
    if (ams_pages > 4) ams_pages = 4;
    if (ams_pages == 0 && st.gcode_state[0]) ams_pages = 1;
    // 导航序 → 实体槽位:进度/温度固定,AMS 只取前 ams_pages 个,设备/控制固定 6/7
    {
        int n = 0;
        s_slot[n++] = 0;
        s_slot[n++] = 1;
        for (int i = 0; i < ams_pages; i++) s_slot[n++] = 2 + i;
        s_slot[n++] = 6;                 // 设备页实体
        s_slot[n++] = 7;                 // 控制页实体
        s_pages_n = n;                   // = 2 + ams + 设备 + 控制
    }
    if (s_cur >= s_pages_n) s_cur = s_pages_n - 1;
    if (s_cur < 0) s_cur = 0;
    int dev_idx = 2 + ams_pages;
    int ctl_idx = dev_idx + 1;

    refresh_header(&st);
    if (!changed && !s_dirty && !s_stop_armed) return;
    s_dirty = false;

    // 状态切换提示音(完成/失败/暂停)
    if (s_last_state[0] && strcmp(s_last_state, st.gcode_state)) {
        if (!strcmp(st.gcode_state, "FINISH"))  app_notify_trigger(NOTIFY_FINISH);
        else if (!strcmp(st.gcode_state, "FAILED")) app_notify_trigger(NOTIFY_FAIL);
        else if (!strcmp(st.gcode_state, "PAUSE"))  app_notify_trigger(NOTIFY_PAUSE);
    }
    strlcpy(s_last_state, st.gcode_state[0] ? st.gcode_state : "IDLE", sizeof(s_last_state));

    refresh_status_bar(&st);

    switch (s_cur) {
    case 0: refresh_progress(&st); break;
    case 1: refresh_temp(&st); break;
    default:
        if (s_cur >= 2 && s_cur < dev_idx) refresh_ams(&st, s_cur - 2, ams_pages);
        else if (s_cur == dev_idx)         refresh_device(&st);
        else if (s_cur == ctl_idx)         ctl_refresh_items(&st);
        break;
    }

    // 停止二次确认超时取消
    if (s_stop_armed && (esp_timer_get_time() - s_stop_armed_at) > 3 * 1000000LL) {
        s_stop_armed = false;
        ctl_refresh_items(&st);
    }
}

static void tick_1s(lv_timer_t *t)
{
    (void)t;
    if (s_active) refresh_all();
}

static void tick_once(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (s_active) refresh_all();
}

// ------------------------------------------------------------ 机器概览实现
// 多机列表页:2台=大卡 / 3台=中卡 / 4台=紧凑卡,关键进度信息常驻。
// 单机时跳过本页,直接进详情(与旧行为一致)。

static void ov_build(void)
{
    s_ov_page = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_ov_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ov_page, 0, PAGE_Y);
    lv_obj_set_size(s_ov_page, 240, PAGE_H);
    lv_obj_set_style_bg_opa(s_ov_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ov_page, 0, 0);
    lv_obj_set_style_pad_all(s_ov_page, 0, 0);

    int n = app_mqtt_machine_count();
    if (n < 1) n = 1;
    if (n > BAMBU_MACHINE_MAX) n = BAMBU_MACHINE_MAX;

    // 分档布局:卡高/间距按机器数取,整体宽松不挤
    int card_h, gap, top = 10;
    if (n <= 2)      { card_h = 110; gap = 12; }
    else if (n == 3) { card_h = 72;  gap = 10; }
    else             { card_h = 55;  gap = 7;  }
    s_ov_bar_w = (n <= 2) ? 196 : (n == 3 ? 190 : 184);

    for (int i = 0; i < n; i++) {
        int y = top + i * (card_h + gap);
        s_ov_card[i] = card(s_ov_page, 8, y, 224, card_h, C_PANEL2);
        // 状态点 + 名称
        s_ov_dot[i] = rect(s_ov_card[i], 12, 10, 8, 8, C_DIM);
        lv_obj_set_style_radius(s_ov_dot[i], 4, 0);
        s_ov_name[i] = label(s_ov_card[i], &font_zh14, C_TEXT, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(s_ov_name[i], 26, 3);
        lv_obj_set_width(s_ov_name[i], 120);
        lv_label_set_long_mode(s_ov_name[i], LV_LABEL_LONG_DOT);
        // 进度%(右上)
        s_ov_pct[i] = label(s_ov_card[i], (n <= 2) ? &font_zh20 : &font_zh14,
                            C_GREEN, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_pos(s_ov_pct[i], 132, 1);
        lv_obj_set_width(s_ov_pct[i], 80);
        // 进度条
        int bar_y = (n <= 2) ? 56 : (n == 3 ? 30 : 26);
        lv_obj_t *trk = rect(s_ov_card[i], 12, bar_y, s_ov_bar_w, 6, C_LINE);
        lv_obj_set_style_radius(trk, 3, 0);
        s_ov_bar[i] = rect(s_ov_card[i], 12, bar_y, 0, 6, C_GREEN);
        lv_obj_set_style_radius(s_ov_bar[i], 3, 0);
        // 状态 + 剩余(左下)
        s_ov_state[i] = label(s_ov_card[i], &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(s_ov_state[i], 12, bar_y + 12);
        lv_obj_set_width(s_ov_state[i], 200);
        lv_label_set_long_mode(s_ov_state[i], LV_LABEL_LONG_DOT);
    }
    lv_obj_add_flag(s_ov_page, LV_OBJ_FLAG_HIDDEN);
}

// 状态文字与颜色:RUNNING=打印中/PAUSE=已暂停/FINISH=已完成/FAILED=失败/IDLE=空闲
static uint32_t ov_state_text(const bambu_state_t *st, char *out, int n)
{
    if (st->hms_count > 0) {
        snprintf(out, n, "HMS 告警");
        return C_RED;
    }
    const char *s = st->gcode_state;
    if (!strcmp(s, "RUNNING")) {
        snprintf(out, n, "打印中 · 剩 %d时%d分", st->remain_min / 60, st->remain_min % 60);
        return C_GREEN;
    }
    if (!strcmp(s, "PAUSE"))  { snprintf(out, n, "已暂停"); return C_YELLOW; }
    if (!strcmp(s, "FINISH")) { snprintf(out, n, "已完成"); return C_CYAN; }
    if (!strcmp(s, "FAILED")) { snprintf(out, n, "失败");   return C_RED; }
    snprintf(out, n, "空闲 · 等待任务");
    return C_DIM;
}

static void ov_refresh(void)
{
    int n = app_mqtt_machine_count();
    if (n < 1) n = 1;
    if (n > BAMBU_MACHINE_MAX) n = BAMBU_MACHINE_MAX;

    for (int i = 0; i < n; i++) {
        bambu_state_t st;
        app_mqtt_snapshot(i, &st);

        // 名称:自定义名优先,否则"N号机";右侧再带 #尾4 便于区分
        char nm[40];
        const char *cust = app_mqtt_name(i);
        const char *snv = app_mqtt_sn(i);
        int selen = snv ? (int)strlen(snv) : 0;
        const char *tail = (selen >= 4) ? snv + selen - 4 : (snv ? snv : "");
        if (cust && cust[0]) snprintf(nm, sizeof(nm), "%s #%s", cust, tail);
        else                 snprintf(nm, sizeof(nm), "%d号机 #%s", i + 1, tail);
        lv_label_set_text(s_ov_name[i], nm);

        // 状态与进度
        char txt[48];
        uint32_t col = ov_state_text(&st, txt, sizeof(txt));
        lv_label_set_text(s_ov_state[i], txt);
        lv_obj_set_style_text_color(s_ov_state[i], lv_color_hex(col), 0);
        lv_obj_set_style_bg_color(s_ov_dot[i], lv_color_hex(col), 0);
        if (st.pct > 0) {
            lv_label_set_text_fmt(s_ov_pct[i], "%d%%", st.pct);
            lv_obj_set_width(s_ov_bar[i], s_ov_bar_w * st.pct / 100);
        } else {
            lv_label_set_text(s_ov_pct[i], "--");
            lv_obj_set_width(s_ov_bar[i], 0);
        }

        // 选中高亮(绿框),HMS 机器红框
        bool sel = (i == s_ov_sel);
        uint32_t border = sel ? C_GREEN : (st.hms_count > 0 ? C_RED : C_LINE);
        lv_obj_set_style_border_width(s_ov_card[i], sel ? 2 : 1, 0);
        lv_obj_set_style_border_color(s_ov_card[i], lv_color_hex(border), 0);
    }
}

// 概览页按键:上下=选择,OK=进详情
static void ov_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    int n = app_mqtt_machine_count();
    if (n < 1) n = 1;
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_ov_sel = (s_ov_sel + n - 1) % n;
    } else if (btn == BSP_BTN_DOWN) {
        s_ov_sel = (s_ov_sel + 1) % n;
    } else if (btn == BSP_BTN_OK) {
        // 进入选中机器的详情:删概览页释放内存,按需建详情页
        s_midx = s_ov_sel;
        s_ov_mode = false;
        if (s_ov_page) { lv_obj_delete(s_ov_page); s_ov_page = NULL; }
        detail_build();
        s_cur = 0;
        s_show_eta = false;
        s_prev_valid = false;
        show_page(0);
        return;
    }
    request_refresh();
}

// ------------------------------------------------------------ 对外

static void show_page(int idx)
{
    if (idx >= s_pages_n) idx = s_pages_n - 1;
    if (idx < 0) idx = 0;
    // 导航序号 ≠ 实体槽位。实体固定为 0进度/1温度/2~5AMS/6设备/7控制,
    // 而实际显示哪几页随 AMS 台数变化,必须经 s_slot 映射,否则 AMS 不足 4 台时
    // 会翻到空 AMS 页,设备页/控制页反而永远翻不到。
    int show = (idx >= 0 && idx < s_pages_n && idx < PAGE_MAX) ? s_slot[idx] : 0;
    for (int i = 0; i < PAGE_MAX; i++) {
        if (i == show) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_cur = idx;
    s_stop_armed = false;   // 切页取消停止确认
    request_refresh();      // 延到 LVGL 任务里重绘(不在按键回调里干重活)
}

// 详情页(8 个实体页)按需构建/释放:
// LVGL 内存池在建完详情页后仅剩 ~1.6KB,概览页对象(2~4 张卡)装不下,
// 两套 UI 绝不能同时存在 —— 概览与详情切换时一建一删。
static void detail_build(void)
{
    for (int i = 0; i < PAGE_MAX; i++) {
        s_pages[i] = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_pages[i], 0, PAGE_Y);
        lv_obj_set_size(s_pages[i], 240, PAGE_H);
        lv_obj_set_style_bg_opa(s_pages[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_pages[i], 0, 0);
        lv_obj_set_style_pad_all(s_pages[i], 0, 0);
    }
    page_progress_build(s_pages[0]);
    page_temp_build(s_pages[1]);
    for (int i = 0; i < 4; i++) page_ams_build(s_pages[2 + i], i);
    page_device_build(s_pages[6]);
    page_control_build(s_pages[7]);
}

static void detail_free(void)
{
    for (int i = 0; i < PAGE_MAX; i++) {
        if (s_pages[i]) { lv_obj_delete(s_pages[i]); s_pages[i] = NULL; }
    }
}

void app_printer_enter(void)
{
    if (s_scr) return;
    app_notify_init();

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    header_build();
    status_bar_build();

    // 多机(≥2 台)先建概览页、详情页按需构建;单机只建详情页(与旧版一致)。
    s_ov_page = NULL;
    int mcnt = app_mqtt_machine_count();
    s_ov_sel = 0;
    if (mcnt >= 2) {
        s_ov_mode = true;
        s_midx = 0;
        ov_build();
    } else {
        s_ov_mode = false;
        s_midx = 0;
        detail_build();
    }

    s_cur = 0;
    s_show_eta = false;
    s_ctl_sel = 0;
    s_stop_armed = false;
    s_light_on = false;
    s_last_state[0] = 0;
    s_prev_valid = false;    // 重进页面时强制整页重绘
    s_dirty = true;
    s_active = true;
    s_timer = lv_timer_create(tick_1s, 1000, NULL);
    s_fast  = lv_timer_create(tick_fast, 30, NULL);   // 承接按键延后的重绘
    s_slot[0] = 0; s_slot[1] = 1; s_slot[2] = 6; s_slot[3] = 7;  // 默认(还没 AMS 数据)
    s_pages_n = 4;
    s_need_refresh = false;
    s_pk_valid = false;      // 丢弃进入前残留的按键

    if (s_ov_mode) ov_refresh();
    else           show_page(0);

    lv_screen_load(s_scr);
    // 诊断:LVGL 内置内存池(默认 48KB)使用情况,排查对象创建超池导致卡死
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGI("printer", "LVGL池: 总=%d 空=%d 已用=%d%%", (int)mon.total_size,
                 (int)mon.free_size, (int)mon.used_pct);
    }
    refresh_all();
}

void app_printer_exit(void)
{
    if (!s_scr) return;
    s_active = false;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_fast)  { lv_timer_delete(s_fast);  s_fast  = NULL; }
    s_need_refresh = false;
    s_pk_valid = false;
    lv_obj_delete(s_scr);
    s_scr = NULL;
    s_ov_page = NULL;
    memset(s_pages, 0, sizeof(s_pages));
}

// 真正的按键逻辑(只在 LVGL 任务里被调用,不在 button 小栈任务里跑)
static void key_handle(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_scr) return;

    // 概览列表模式:上下选机器,OK 进详情;长按 OK 返回菜单(由上层 main 处理)
    if (s_ov_mode) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) return;   // 留给上层退出
        ov_key(btn, ev);
        return;
    }

    // 详情模式:长按 OK = 返回机器列表(多机)或返回菜单(单机)
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (app_mqtt_machine_count() >= 2) {
            // 详情→概览:删详情页释放内存,重建概览页(两套 UI 不并存)
            s_ov_mode = true;
            s_ov_sel = s_midx;
            detail_free();
            ov_build();
            ov_refresh();
        }
        return;   // 单机时交给上层返回菜单
    }

    if (ev == BSP_BTN_CLICK) {
        s_dirty = true;      // 交互必重绘(数据可能没变,但选中项/显示模式变了)
        // 控制页:上下=选择,顶/底溢出=翻页;OK=执行
        bool in_ctl = (s_cur == s_pages_n - 1);
        if (btn == BSP_BTN_UP) {
            if (in_ctl) {
                if (s_ctl_sel > 0) { s_ctl_sel--; request_refresh(); }
                else show_page((s_cur + s_pages_n - 1) % s_pages_n);
            } else {
                show_page((s_cur + s_pages_n - 1) % s_pages_n);
            }
        } else if (btn == BSP_BTN_DOWN) {
            if (in_ctl) {
                if (s_ctl_sel < 2) { s_ctl_sel++; request_refresh(); }
                else show_page((s_cur + 1) % s_pages_n);
            } else {
                show_page((s_cur + 1) % s_pages_n);
            }
        } else if (btn == BSP_BTN_OK) {
            if (s_cur == 0) {
                s_show_eta = !s_show_eta;      // 进度页:切换剩余/完成
                refresh_all();
            } else if (in_ctl) {
                bambu_state_t st;
                app_mqtt_snapshot(s_midx, &st);
                const char *state = st.gcode_state;
                if (s_ctl_sel == 0) {
                    if (!strcmp(state, "RUNNING"))      app_mqtt_print_command(s_midx, "pause");
                    else if (!strcmp(state, "PAUSE"))   app_mqtt_print_command(s_midx, "resume");
                } else if (s_ctl_sel == 1) {
                    if (!strcmp(state, "RUNNING") || !strcmp(state, "PAUSE")) {
                        if (s_stop_armed) { s_stop_armed = false; app_mqtt_print_command(s_midx, "stop"); }
                        else { s_stop_armed = true; s_stop_armed_at = esp_timer_get_time(); }
                    }
                } else {
                    s_light_on = !s_light_on;    // 乐观切换,命令后由云端回包纠正
                    app_mqtt_set_chamber_light(s_midx, s_light_on);
                }
                app_mqtt_request_now(s_midx);
                refresh_all();
                lv_timer_create(tick_once, 1500, NULL);
            } else {
                app_mqtt_request_now(s_midx);
                refresh_all();
                lv_timer_create(tick_once, 1500, NULL);
            }
        }
    }
}

// 对外入口:只记录按键,立刻返回 —— 绝不在 button 组件的小栈任务里干重活。
void app_printer_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_scr) return;
    s_pk_btn = btn; s_pk_ev = ev; s_pk_valid = true;
}

bool app_printer_in_overview(void)
{
    return s_scr && s_ov_mode;
}
