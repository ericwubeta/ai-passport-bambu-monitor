// main/app_printer.c —— 打印机状态主界面。
// 页面(动态):0 精灵首页(状态动作+大%+进度条) 1 温度
//           2..2+AMS台数 AMS 料槽页(每台一页,2x2 槽) 设备(含 HMS)。
// 数据源:app_mqtt 快照 + bsp_battery;1 秒定时刷新,OK 键上下文操作。
#include "app_printer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_mqtt.h"
#include "app_notify.h"
#include "app_spirit_view.h"
#include "app_spirit_skins.h"
#include "app_telemetry.h"
#include "spirit_dispatch.h"
#include "spirit_bindings.h"
#include "spirit_event_logic.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#include "font_zh.h"
#include "hms_codes.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#define PAGE_MAX 10          // Progress +4 temperature pairs +4 AMS + device.
#define MSG_MAX  48
#ifndef CHATTER_PERIOD_MS
#define CHATTER_PERIOD_MS 180000ULL   // Text-only low-frequency bubble interval.
#endif
#ifndef CHATTER_FIRST_MS
#define CHATTER_FIRST_MS 20000ULL     // First ambient line after entering the page.
#endif
#define BUBBLE_MS 6000ULL

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

static lv_obj_t *s_ov_name[BAMBU_MACHINE_MAX];
static lv_obj_t *s_ov_pct[BAMBU_MACHINE_MAX];



static int s_slot[PAGE_MAX];   // 导航序号 → 实体页槽位(实体固定:0进度 1温度 2~5AMS 6设备)
static lv_timer_t *s_timer;
static lv_timer_t *s_fast;     // 30ms 快速重绘:承接按键回调里延后的重绘请求
static lv_timer_t *s_retry;    // 单个延后刷新,页面销毁前取消
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
static lv_obj_t *s_status, *s_job, *s_primary;
static lv_obj_t *s_prog_track, *s_prog_fill;   // 横向进度条(替代折线圆环)
static lv_obj_t *s_time_lbl, *s_time_hint;
static lv_obj_t *s_bubble, *s_bubble_lbl;      // 状态语音/低频文字气泡
static uint64_t s_bubble_until_ms;
static uint64_t s_chatter_ms;
static bool s_chatter_seen;
static bool s_bubble_sticky;     // event bubble survives page switches; chatter does not
static int s_hint_slot = -1;     // last page that already delivered its dialogue
static int s_hint_variant[3] = { -1, -1, -1 };  // last alternate used per page
static uint64_t s_last_refresh_ms;
static bool s_show_eta;      // false=显示剩余;true=显示完成时刻
// 温度页
static lv_obj_t *s_temp_title;
static lv_obj_t *s_head_compact[4]; // one compact row per nozzle, no pages
static lv_obj_t *s_chamber_lbl;
static lv_obj_t *s_bed_value, *s_chamber_value;
static lv_obj_t *s_ams_title[4];
static lv_obj_t *s_ams_swatch[4][4], *s_ams_type[4][4], *s_ams_remain[4][4];
static lv_obj_t *s_d_sn, *s_d_model, *s_d_cloud, *s_d_wifi, *s_d_ip, *s_d_hms_lbl;
static lv_obj_t *s_sig_bar[4]; // local WiFi RSSI bars
static lv_obj_t *s_report_name, *s_report_value, *s_report_line;
static spirit_dispatch_t s_dispatch;
static int s_report_machine;
static int s_temperature_pair;
static int s_built_slot = -1;

static bambu_state_t s_work;
static spirit_dispatch_status_t s_dispatch_status[BAMBU_MACHINE_MAX];
static char s_dispatch_states[BAMBU_MACHINE_MAX][10];
static spirit_event_latch_t s_events[BAMBU_MACHINE_MAX];
static bool s_event_fresh[BAMBU_MACHINE_MAX];

static void refresh_all(void);    // 前向声明(tick_fast 要调,定义在后面)
static void ov_refresh(void);     // 前向声明(refresh_all 概览模式要调)
static void detail_build(void);   // 前向声明(概览→详情切换要用)
static void detail_free(void);    // 前向声明(详情→概览切换要用)
static lv_obj_t *page_new(void);
static void apply_page_vis(int show); // 前向声明(refresh_all 懒建页后收敛可见性)
static void show_page(int idx);   // 前向声明(ov_key 进详情要调)
static void key_handle(bsp_btn_t btn, bsp_btn_ev_t ev);

static uint64_t spirit_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void bubble_show(const char *text, bool sticky)
{
    if (!s_bubble || !s_bubble_lbl || !text || !text[0]) return;
    lv_label_set_text(s_bubble_lbl, text);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_bubble);
    s_bubble_sticky = sticky;
    s_bubble_until_ms = spirit_now_ms() + BUBBLE_MS;
}

// Low-frequency visual chatter. It never calls the voice worker; the linked
// state clips remain event-only. Each role has three wordings per branch and
// the pick rotates, so the same line does not come back every time.
static const char *chatter_text(spirit_skin_id_t skin, spirit_action_t action,
                                const bambu_state_t *st, uint64_t now_ms)
{
    static const char *const idle[SPIRIT_SKIN_COUNT][3] = {
        {"本鱼待命中。", "闲着，随时开工呀。", "没事做，陪你待着。"},
        {"暂时没有任务。", "空闲中，随时可以开始。", "没有在打的单子。"},
        {"当前无打印任务。", "空闲，等待任务。", "没有进行中的任务。"},
        {"这会儿空闲。", "闲着，等你。", "没活儿，待机。"},
        {"幕间休息，等下一场。", "舞台就绪，等待任务。", "有新消息再叫我。"},
        {"这会儿没单，陪你玩会儿。", "机器歇着，我也歇会儿。", "主人，下一单打什么？"},
    };
    static const char *const printing[SPIRIT_SKIN_COUNT][3] = {
        {"还在打印。", "机器还在动。", "打着呢，我看着。"},
        {"我在这儿看着进度。", "打印进行中。", "还在打，我留意着。"},
        {"状态已核对，继续观察。", "打印中，持续观察。", "任务进行中。"},
        {"这单还在打。", "还在跑，我盯着。", "没打完，等着。"},
        {"这场演出还在继续。", "下一幕还在准备。", "进度有消息，我会告诉你。"},
        {"这单我盯着呢。", "机器忙着，我来陪你。", "还在打，有新消息叫你。"},
    };
    static char buf[64];
    static unsigned pick;
    unsigned role = (unsigned)skin < SPIRIT_SKIN_COUNT ? (unsigned)skin : 0;
    unsigned k = pick++ % 3u;
    if (action == SPIRIT_PRINTING) {
        const char *model = st->model[0] ? st->model : "打印机";
        if (app_telemetry_fresh(st->remain_valid, st->remain_ms, now_ms)) {
            int remain = st->remain_min;
            switch (role) {
            case 0:
                if (k == 0) snprintf(buf, sizeof(buf), "还剩%d分钟，我盯着呢。", remain);
                else if (k == 1) snprintf(buf, sizeof(buf), "还有%d分钟，我陪着你。", remain);
                else snprintf(buf, sizeof(buf), "%d分钟就好，别急呀。", remain);
                break;
            case 1:
                if (k == 0) snprintf(buf, sizeof(buf), "进度%d%%，还剩%d分钟。", st->pct, remain);
                else if (k == 1) snprintf(buf, sizeof(buf), "还剩%d分钟，节奏正常。", remain);
                else snprintf(buf, sizeof(buf), "大约%d分钟后完成。", remain);
                break;
            case 2:
                if (k == 0) snprintf(buf, sizeof(buf), "还剩%d分钟，继续观察。", remain);
                else if (k == 1) snprintf(buf, sizeof(buf), "剩余%d分钟，状态稳定。", remain);
                else snprintf(buf, sizeof(buf), "%d分钟后可以验收。", remain);
                break;
            case 4:
                snprintf(buf, sizeof(buf), "这场演出还剩%d分钟。", remain);
                break;
            case 5:
                snprintf(buf, sizeof(buf), "主人，这单还剩%d分钟哦。", remain);
                break;
            default:
                if (k == 0) snprintf(buf, sizeof(buf), "这单还剩%d分钟。", remain);
                else if (k == 1) snprintf(buf, sizeof(buf), "还有%d分钟，我盯着。", remain);
                else snprintf(buf, sizeof(buf), "%d分钟，等着吧。", remain);
                break;
            }
            return buf;
        }
        if (app_telemetry_fresh(st->pct_valid, st->pct_ms, now_ms)) {
            switch (role) {
            case 0:
                if (k == 0) snprintf(buf, sizeof(buf), "%s %d%%", model, st->pct);
                else if (k == 1) snprintf(buf, sizeof(buf), "已经%d%%啦。", st->pct);
                else snprintf(buf, sizeof(buf), "%s打到%d%%。", model, st->pct);
                break;
            case 1:
                if (k == 0) snprintf(buf, sizeof(buf), "%s打印中", model);
                else if (k == 1) snprintf(buf, sizeof(buf), "当前进度%d%%，继续。", st->pct);
                else snprintf(buf, sizeof(buf), "打印到%d%%了。", st->pct);
                break;
            case 2:
                if (k == 0) snprintf(buf, sizeof(buf), "打印进度%d%%", st->pct);
                else if (k == 1) snprintf(buf, sizeof(buf), "进度%d%%，已核对。", st->pct);
                else snprintf(buf, sizeof(buf), "当前完成%d%%。", st->pct);
                break;
            case 4:
                snprintf(buf, sizeof(buf), "这场演出进度%d%%。", st->pct);
                break;
            case 5:
                snprintf(buf, sizeof(buf), "打到%d%%啦，我还盯着呢。", st->pct);
                break;
            default:
                if (k == 0) snprintf(buf, sizeof(buf), "这单%d%%了。", st->pct);
                else if (k == 1) snprintf(buf, sizeof(buf), "%d%%，不急。", st->pct);
                else snprintf(buf, sizeof(buf), "进度%d%%。", st->pct);
                break;
            }
            return buf;
        }
        return printing[role][k];
    }
    return idle[role][k];
}

static int voice_kind_for_action(spirit_action_t action);

static const char *page_hint_text(int slot, spirit_skin_id_t skin,
                                  const bambu_state_t *st, uint64_t now_ms,
                                  unsigned variant)
{
    unsigned role = (unsigned)skin < SPIRIT_SKIN_COUNT ? (unsigned)skin : 0;
    if (slot == 0) {
        spirit_action_t action = spirit_action_for_status(&(spirit_status_t){
            .cloud_online = st->cloud == BAMBU_CLOUD_ONLINE,
            .state_received = st->state_received,
            .state_updated_ms = st->state_updated_ms,
            .gcode_state = st->gcode_state,
            .print_error = st->print_error,
            .job_generation = st->job_generation,
            .task_token = spirit_task_token(st->task_id),
        }, now_ms);
        if (action == SPIRIT_PRINTING || action == SPIRIT_IDLE) {
            return chatter_text(skin, action, st, now_ms);
        }
        int kind = voice_kind_for_action(action);
        if (kind >= 0) return app_notify_voice_text(skin, (unsigned)kind);
        return NULL;
    }
    // Scene-specific spoken line for the temperature/AMS/device pages, so the
    // bubble text always matches the clip (v218-page-voice-v03).
    if (slot >= 1 && slot <= 4) return app_notify_page_text(role, 0, variant);
    if (slot >= 5 && slot <= 8) return app_notify_page_text(role, 1, variant);
    if (slot == 9) return app_notify_page_text(role, 2, variant);
    return NULL;
}

static int voice_kind_for_action(spirit_action_t action)
{
    switch (action) {
    case SPIRIT_PRINTING:  return 0;
    case SPIRIT_PAUSED:    return 1;
    case SPIRIT_COMPLETED: return 2;
    case SPIRIT_FAILED:    return 3;
    case SPIRIT_RUNOUT:    return 4;
    default:               return -1;
    }
}

static spirit_status_t spirit_status(const bambu_state_t *st)
{
    return (spirit_status_t) {
        .cloud_online = st->cloud == BAMBU_CLOUD_ONLINE,
        .state_received = st->state_received,
        .state_updated_ms = st->state_updated_ms,
        .gcode_state = st->gcode_state,
        .print_error = st->print_error,
        .job_generation = st->job_generation,
        .task_token = spirit_task_token(st->task_id),
    };
}

// 按键回调跑在 button 组件的小栈任务(默认 2~3KB)里,而翻页/重绘很重:
// refresh_all() 会把 bambu_state_t 大结构体上栈 + memcpy + 一堆 LVGL/字体调用,
// 在回调里直接干会爆栈 → 看门狗/栈溢出 → 设备重启(「按两回上键就重启」的根因)。
// 所以按键回调只记下事件,由 s_fast 定时器在 LVGL 任务(栈足够)里真正执行。
static void request_refresh(void) { s_need_refresh = true; }

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

// One global bubble on the screen (not on a page), so an event is visible
// regardless of which detail page the user is currently reading.
static void bubble_build(void)
{
    if (s_bubble || !s_scr) return;
    s_bubble = rect(s_scr, 4, 286, 232, 32, C_PANEL2); // page y=234 + PAGE_Y
    lv_obj_set_style_radius(s_bubble, 10, 0);
    lv_obj_set_style_border_width(s_bubble, 1, 0);
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(C_LINE), 0);
    s_bubble_lbl = label(s_bubble, &font_zh14, C_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(s_bubble_lbl, 220, 18);
    lv_obj_set_pos(s_bubble_lbl, 6, 6);
    lv_label_set_long_mode(s_bubble_lbl, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_bubble(uint64_t now_ms)
{
    if (s_bubble_until_ms && now_ms >= s_bubble_until_ms) {
        s_bubble_until_ms = 0;
        s_bubble_sticky = false;
        if (s_bubble) lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_bubble) return;
    if (s_bubble_until_ms > now_ms) {
        lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_bubble);
    } else {
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    }
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
        s_dots[i] = rect(s_scr, 76 + i * 9, 13, 5, 5, C_LINE);
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
    lv_label_set_long_mode(s_bar_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_bar_lbl, 226, 16); // One line, including long machine names/SSIDs.
    lv_obj_set_pos(s_bar_lbl, 7, 1);
}

static void refresh_status_bar(const bambu_state_t *st)
{
    // 多机时前缀机器名,单机保持原样
    const char *cust = app_mqtt_name(s_midx);
    int machines = app_mqtt_machine_count();
    spirit_status_t status = spirit_status(st);
    bool fresh = spirit_status_fresh(&status, spirit_now_ms());
    if (fresh && st->hms_count > 0) {
        static char buf[MSG_MAX];
        const char *txt = hms_lookup(st->hms[0].code, buf, sizeof(buf));
        if (cust && machines >= 2) {
            static char t2[96];
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
        // 头部圆点已经表达云状态;这里只补网络上下文,不再重复“云在线”文字。
        // 状态条常驻显示当前连的 WiFi 名,出门自动切网后不用翻页也能确认。
        // 名字最长 32 字节,缓冲给足;超宽由 label 的 DOT 模式末尾省略。
        const char *cloud = st->cloud == BAMBU_CLOUD_ONLINE    ? NULL
                          : st->cloud == BAMBU_CLOUD_CONNECTING ? "连接中" : "离线";
        const char *ssid = app_wifi_current_ssid();
        static char t[96];
        if (cust && machines >= 2) {
            if (cloud && ssid)      snprintf(t, sizeof(t), "%s · %s · %s", cust, cloud, ssid);
            else if (cloud)         snprintf(t, sizeof(t), "%s · %s · 未联网", cust, cloud);
            else if (ssid)          snprintf(t, sizeof(t), "%s · %s", cust, ssid);
            else                    snprintf(t, sizeof(t), "%s · 在线", cust);
        } else {
            if (cloud && ssid)      snprintf(t, sizeof(t), "%s · %s", cloud, ssid);
            else if (cloud)         snprintf(t, sizeof(t), "%s · 未联网", cloud);
            else if (ssid)          snprintf(t, sizeof(t), "%s", ssid);
            else                    snprintf(t, sizeof(t), "在线");
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
    lv_label_set_text(s_status, "等待数据");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 0);

    s_job = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_job, "");
    lv_label_set_long_mode(s_job, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_job, 220);
    lv_obj_align(s_job, LV_ALIGN_TOP_MID, 0, 24);

    // Native 112x112 image at screen(64,92), never scaled at runtime.
    app_spirit_view_create(p, 64, 40);

    // A single font label replaces dozens of per-digit rectangle objects.
    s_primary = label(p, &lv_font_montserrat_36, C_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_primary, "--");
    lv_obj_align(s_primary, LV_ALIGN_TOP_MID, 0, 154);
    s_prog_track = rect(p, 20, 204, 200, 4, C_LINE);
    s_prog_fill = rect(p, 20, 204, 0, 4, C_GREEN);

    s_time_lbl = label(p, &font_zh14, C_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_time_lbl, "");
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_MID, 0, 218);
    s_time_hint = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_time_hint, "OK 刷新");
    lv_obj_align(s_time_hint, LV_ALIGN_TOP_MID, 0, 242);
}

static void refresh_progress(const bambu_state_t *st)
{
    uint64_t now_ms = spirit_now_ms();
    spirit_status_t status = spirit_status(st);
    bool fresh = spirit_status_fresh(&status, now_ms);
    spirit_action_t action = spirit_action_for_status(&status, now_ms);
    uint32_t color = C_DIM;
    const char *title;
    if (!fresh) {
        if (st->cloud == BAMBU_CLOUD_CONNECTING) title = "连接中";
        else if (st->cloud != BAMBU_CLOUD_ONLINE) title = "离线";
        else if (!st->state_received) title = "等待数据";
        else title = "数据过期";
        color = C_YELLOW;
    } else if (action == SPIRIT_RUNOUT) {
        title = "缺料";
        color = C_YELLOW;
    } else {
        title = state_zh(st->gcode_state, &color);
    }
    lv_label_set_text(s_status, title);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
    lv_label_set_text(s_job, fresh && st->job[0] ? st->job : "");

    bool finished = fresh && !strcmp(st->gcode_state, "FINISH");
    bool pct_known = finished || app_telemetry_fresh(st->pct_valid, st->pct_ms, now_ms);
    bool in_job = fresh && (action == SPIRIT_PRINTING || action == SPIRIT_PAUSED ||
                           action == SPIRIT_RUNOUT || finished);
    int pct = finished ? 100 : st->pct;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_obj_set_style_text_font(s_primary,
                              in_job || !fresh ? &lv_font_montserrat_36 : &font_zh20, 0);
    if (in_job && pct_known) lv_label_set_text_fmt(s_primary, "%d%%", pct);
    else if (in_job) lv_label_set_text(s_primary, "--");
    else lv_label_set_text(s_primary, !fresh ? "--" : action == SPIRIT_FAILED ? "请检查" : "待命");
    lv_obj_set_style_text_color(s_primary, lv_color_hex(color), 0);
    // Different font sizes keep the same visual center below the sprite.
    lv_obj_align(s_primary, LV_ALIGN_TOP_MID, 0, in_job || !fresh ? 154 : 162);
    if (in_job && pct_known) {
        lv_obj_remove_flag(s_prog_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_prog_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_prog_fill, pct * 200 / 100);
        lv_obj_set_style_bg_color(s_prog_fill, lv_color_hex(color), 0);
    } else {
        lv_obj_add_flag(s_prog_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_prog_fill, LV_OBJ_FLAG_HIDDEN);
    }

    char time_text[80] = "";
    bool remain_fresh = app_telemetry_fresh(st->remain_valid, st->remain_ms, now_ms);
    if (in_job && !finished) {
        if (remain_fresh) {
            if (s_show_eta && app_wifi_time_ready()) {
                time_t eta = time(NULL) + (time_t)st->remain_min * 60;
                struct tm tmv;
                localtime_r(&eta, &tmv);
                snprintf(time_text, sizeof(time_text), "%02d:%02d 完成", tmv.tm_hour, tmv.tm_min);
            } else if (s_show_eta) {
                snprintf(time_text, sizeof(time_text), "--:-- 完成");
            } else if (st->remain_min >= 60) {
                snprintf(time_text, sizeof(time_text), "剩余 %dh%02dm", st->remain_min / 60, st->remain_min % 60);
            } else {
                snprintf(time_text, sizeof(time_text), "剩余 %dm", st->remain_min);
            }
        } else {
            snprintf(time_text, sizeof(time_text), "%s", s_show_eta ? "--:-- 完成" : "剩余 --");
        }
        // The value line itself carries the OK affordance; a separate instruction
        // label would repeat what the layout already shows.
        if (fresh && action == SPIRIT_PRINTING) {
            size_t used = strlen(time_text);
            if (used + 6 < sizeof(time_text))
                snprintf(time_text + used, sizeof(time_text) - used, " · OK");
        }
    }
    lv_label_set_text(s_time_lbl, time_text);
    const char *hint = "";
    if (!fresh) hint = "OK 刷新";
    else if (finished) hint = app_spirit_view_finish_acknowledged(s_midx) ? "等待下一次打印" : "按 OK 确认";
    else if (action == SPIRIT_FAILED) hint = "查看设备页告警";
    else if (action == SPIRIT_RUNOUT) hint = "请补充耗材";
    else if (action == SPIRIT_PAUSED) hint = "嗯？暂停了，请检查。";
    else if (action == SPIRIT_IDLE) hint = "等待下一次打印";
    lv_label_set_text(s_time_hint, hint);
    // The bubble sits on the same bottom strip; hide the hint while it is up.
    lv_obj_set_hidden(s_time_hint, s_bubble_until_ms > now_ms);
}

// ------------------------------------------------------------ 温度页


static void page_temp_build(lv_obj_t *p)
{
    s_temp_title = label(p, &font_zh14, C_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_temp_title, 10, 4); lv_obj_set_width(s_temp_title, 220);
    app_spirit_view_create(p, 64, 18);
    for (int i = 0; i < 4; i++) {
        s_head_compact[i] = label(p, &font_zh14, C_TEXT, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(s_head_compact[i], 14, 132 + i * 18);
        lv_obj_set_width(s_head_compact[i], 212);
        lv_label_set_long_mode(s_head_compact[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(s_head_compact[i], LV_OBJ_FLAG_HIDDEN);
    }
    rect(p, 14, 206, 212, 1, C_LINE);
    lv_obj_t *bed = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(bed, "热床"); lv_obj_set_pos(bed, 14, 212);
    s_bed_value = label(p, &font_zh14, C_GREEN_L, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(s_bed_value, 60, 212); lv_obj_set_width(s_bed_value, 72);
    s_chamber_lbl = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_chamber_lbl, "腔温"); lv_obj_set_pos(s_chamber_lbl, 140, 212);
    s_chamber_value = label(p, &font_zh14, C_CYAN, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(s_chamber_value, 190, 212); lv_obj_set_width(s_chamber_value, 36);
}

static void refresh_temp(const bambu_state_t *st)
{
    uint64_t now = spirit_now_ms(); bool online = st->cloud == BAMBU_CLOUD_ONLINE;
    bool has_head = st->structured_heads && st->heads.count > 0;
    int count = has_head ? (int)st->heads.count : 1;
    if (count > 4) count = 4;
    lv_label_set_text_fmt(s_temp_title, count > 1 ? "温度 · %d 喷嘴" : "温度", count);

    // Display order: the active (main) head is always 喷头1; the remaining
    // heads follow in ascending protocol ID order. Reported IDs are sparse
    // (an X2D can report 1,0,2,3), so raw IDs must never reach the label.
    int order[4], shown = 0, active_idx = -1;
    if (has_head) {
        int reported = (int)st->heads.count;
        if (reported > 4) reported = 4;
        if (st->extruder.current_known) {
            for (int k = 0; k < reported; k++)
                if (st->heads.heads[k].id == st->extruder.current_id) { active_idx = k; break; }
        }
        int rest[4], rest_n = 0;
        for (int k = 0; k < reported; k++)
            if (k != active_idx) rest[rest_n++] = k;
        for (int a = 0; a < rest_n; a++)
            for (int b = a + 1; b < rest_n; b++)
                if (st->heads.heads[rest[b]].id < st->heads.heads[rest[a]].id) {
                    int swap = rest[a]; rest[a] = rest[b]; rest[b] = swap;
                }
        if (active_idx >= 0) order[shown++] = active_idx;
        for (int a = 0; a < rest_n; a++) order[shown++] = rest[a];
    }
    if (!shown) { order[0] = 0; shown = 1; }

    for (int i = 0; i < 4; i++) {
        bool show = i < shown;
        lv_obj_set_hidden(s_head_compact[i], !show);
        if (!show) continue;
        char name[20] = "喷嘴";
        char cv[8] = "--", tv[8] = "--";
        bool ok = false;
        if (has_head) {
            const nozzle_head_t *head = &st->heads.heads[order[i]];
            bool active = order[i] == active_idx;
            ok = online && nozzle_head_temperature_fresh(head, now, SPIRIT_FRESH_MS);
            int cur = head->temperature.current_c, target = head->temperature.target_c;
            if (cur > 1000 || target > 1000) ok = false;
            if (ok) {
                snprintf(cv, sizeof(cv), "%d°", cur);
                snprintf(tv, sizeof(tv), "%d°", target);
            }
            if (shown == 1) snprintf(name, sizeof(name), "%s", active ? "喷嘴 *" : "喷嘴");
            else snprintf(name, sizeof(name), "%s喷头%d", active ? "*" : "", i + 1);
        } else {
            ok = online && app_telemetry_fresh(st->nozzle_valid, st->nozzle_ms, now);
            bool target_ok = online && app_telemetry_fresh(st->nozzle_target_valid, st->nozzle_target_ms, now);
            if (ok) snprintf(cv, sizeof(cv), "%d°", st->nozzle_t);
            if (target_ok) snprintf(tv, sizeof(tv), "%d°", st->nozzle_target);
        }
        lv_label_set_text_fmt(s_head_compact[i], "%s  %s/%s", name, cv, tv);
    }

    char current[16] = "--", target[16] = "--";
    if (online && app_telemetry_fresh(st->bed_valid, st->bed_ms, now)) snprintf(current, sizeof(current), "%d°", st->bed_t);
    if (online && app_telemetry_fresh(st->bed_target_valid, st->bed_target_ms, now)) snprintf(target, sizeof(target), "%d°", st->bed_target);
    lv_label_set_text_fmt(s_bed_value, "%s / %s", current, target);

    // A1/A1 mini/A2L-class has no chamber; keep the row out of the page instead
    // of spending a line on a permanent "--".
    bool show_chamber = spirit_model_family(st->model) != 0;
    lv_obj_set_hidden(s_chamber_lbl, !show_chamber);
    lv_obj_set_hidden(s_chamber_value, !show_chamber);
    if (online && app_telemetry_fresh(st->chamber_valid, st->chamber_ms, now)) lv_label_set_text_fmt(s_chamber_value, "%d°", st->chamber_t);
    else lv_label_set_text(s_chamber_value, "--");
}

static void page_ams_build(lv_obj_t *p, int ams_idx)
{
    s_ams_title[ams_idx] = label(p, &font_zh14, C_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_ams_title[ams_idx], 10, 4); lv_obj_set_width(s_ams_title[ams_idx], 220);
    // Character on top, a 2x2 material summary at the bottom. One compact line
    // per slot (name + percent); the weight is intentionally dropped.
    app_spirit_view_create(p, 64, 30);
    for (int i = 0; i < 4; i++) {
        int col = i % 2, row = i / 2;
        int x = 14 + col * 114;
        int y = 190 + row * 24;
        s_ams_swatch[ams_idx][i] = rect(p, x, y + 3, 12, 12, C_LINE);
        s_ams_type[ams_idx][i] = label(p, &font_zh14, C_TEXT, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(s_ams_type[ams_idx][i], x + 18, y); lv_obj_set_width(s_ams_type[ams_idx][i], 58);
        lv_label_set_long_mode(s_ams_type[ams_idx][i], LV_LABEL_LONG_DOT);
        s_ams_remain[ams_idx][i] = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_pos(s_ams_remain[ams_idx][i], x + 78, y); lv_obj_set_width(s_ams_remain[ams_idx][i], 30);
    }
}

static void refresh_ams(const bambu_state_t *st, int ams_idx, int total_ams)
{
    (void)total_ams;
    const bambu_ams_t *a = &st->ams[ams_idx];
    uint64_t now = spirit_now_ms(); bool online = st->cloud == BAMBU_CLOUD_ONLINE;
    bool humidity_fresh = online && app_telemetry_fresh(a->present, a->humidity_updated_ms, now);
    bool raw_fresh = online && app_telemetry_fresh(a->present, a->humidity_raw_updated_ms, now);
    char humidity[32] = "湿度 --";
    if (raw_fresh && a->humidity_raw >= 0) snprintf(humidity, sizeof(humidity), "湿度 %d%%", a->humidity_raw);
    else if (humidity_fresh && a->humidity > 0) snprintf(humidity, sizeof(humidity), "湿度档 %d", a->humidity);
    if (a->present) lv_label_set_text_fmt(s_ams_title[ams_idx], "AMS %u · %s", a->id + 1, humidity);
    else lv_label_set_text(s_ams_title[ams_idx], "外部料筒");
    for (int i = 0; i < 4; i++) {
        const bambu_tray_t *t = &a->tray[i];
        bool visible = a->present || i == 0;
        lv_obj_set_hidden(s_ams_swatch[ams_idx][i], !visible);
        lv_obj_set_hidden(s_ams_type[ams_idx][i], !visible);
        lv_obj_set_hidden(s_ams_remain[ams_idx][i], !visible);
        bool fresh = online && app_telemetry_fresh(t->reported, t->updated_ms, now);
        const char *name = t->tray_type[0] ? t->tray_type : t->tray_name[0] ? t->tray_name : t->is_used ? "耗材" : "空槽";
        lv_label_set_text_fmt(s_ams_type[ams_idx][i], "%s%d %s", t->active ? "*" : "", i + 1, fresh ? name : "--");
        lv_obj_set_style_bg_color(s_ams_swatch[ams_idx][i], lv_color_hex(fresh && t->color_valid ? t->tray_color : C_LINE), 0);
        bool pct_fresh = fresh && app_telemetry_fresh(t->pct_valid, t->pct_updated_ms, now);
        if (pct_fresh) lv_label_set_text_fmt(s_ams_remain[ams_idx][i], "%d%%", t->tray_pct);
        else lv_label_set_text(s_ams_remain[ams_idx][i], "--");
    }
}

static void page_device_build(lv_obj_t *p)
{
    s_d_model = label(p, &font_zh20, C_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_d_model, 10, 2); lv_obj_set_width(s_d_model, 220);
    app_spirit_view_create(p, 64, 26);
    s_d_cloud = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_d_cloud, 14, 152); lv_obj_set_width(s_d_cloud, 140);
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        s_sig_bar[i] = rect(p, 158 + i * 5, 168 - h, 3, h, C_LINE);
    }
    s_d_wifi = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(s_d_wifi, 176, 152); lv_obj_set_width(s_d_wifi, 60);
    s_d_sn = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_d_sn, 14, 174); lv_obj_set_width(s_d_sn, 212);
    lv_label_set_long_mode(s_d_sn, LV_LABEL_LONG_DOT);
    s_d_ip = label(p, &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_d_ip, 14, 194); lv_obj_set_width(s_d_ip, 212);
    rect(p, 14, 210, 212, 1, C_LINE);
    s_d_hms_lbl = label(p, &font_zh14, C_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_d_hms_lbl, 14, 214); lv_obj_set_size(s_d_hms_lbl, 212, 18);
    lv_label_set_long_mode(s_d_hms_lbl, LV_LABEL_LONG_DOT);
}

static void refresh_device(const bambu_state_t *st)
{
    lv_label_set_text(s_d_model, st->model[0] ? st->model : "机型未上报");
    lv_label_set_text_fmt(s_d_sn, "SN %s", app_mqtt_sn(s_midx));
    const char *cloud = st->cloud == BAMBU_CLOUD_ONLINE   ? "云端已连接"
                      : st->cloud == BAMBU_CLOUD_CONNECTING ? "云端连接中" : "云端离线";
    lv_label_set_text(s_d_cloud, cloud);
    int rssi = app_wifi_connected() ? app_wifi_rssi() : -127;
    int bars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
    for (int i = 0; i < 4; i++)
        lv_obj_set_style_bg_color(s_sig_bar[i], lv_color_hex(i < bars ? C_GREEN : C_LINE), 0);
    if (app_wifi_connected()) lv_label_set_text_fmt(s_d_wifi, "%d dBm", rssi);
    else lv_label_set_text(s_d_wifi, "未连接");
    if (st->printer_ip) {
        const uint8_t *ip = (const uint8_t *)&st->printer_ip;
        lv_label_set_text_fmt(s_d_ip, "IP %u.%u.%u.%u", ip[3], ip[2], ip[1], ip[0]);
    } else lv_label_set_text(s_d_ip, "IP --");
    spirit_status_t status = spirit_status(st);
    if (!spirit_status_fresh(&status, spirit_now_ms())) lv_label_set_text(s_d_hms_lbl, "等待最新设备数据");
    else if (st->hms_count) {
        char text[MSG_MAX]; lv_label_set_text(s_d_hms_lbl, hms_lookup(st->hms[0].code, text, sizeof(text)));
    } else if (!strcmp(st->gcode_state, "PAUSE")) lv_label_set_text(s_d_hms_lbl, "暂停原因未上报，请检查打印机");
    else lv_label_set_text(s_d_hms_lbl, "暂无告警");
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
    // Observe every configured printer, including while its detail is hidden.
    uint64_t event_now = spirit_now_ms();
    app_spirit_bindings_poll();
    for (int machine = 0; machine < app_mqtt_machine_count(); machine++) {
        app_mqtt_snapshot(machine, &s_work);
        spirit_status_t event_status = spirit_status(&s_work);
        bool fresh = spirit_status_fresh(&event_status, event_now);
        spirit_action_t action = spirit_action_for_status(&event_status, event_now);
        if (s_event_fresh[machine] != fresh || (fresh && (!s_events[machine].initialized || s_events[machine].generation != event_status.job_generation))) app_notify_epoch(machine);
        s_event_fresh[machine] = fresh;
        bool old_finish = app_spirit_finish_saved(machine, event_status.task_token, NULL);
        int kind = spirit_event_observe(&s_events[machine], &event_status, event_status.task_token, old_finish, event_now);
        if (kind >= 0) {
            unsigned skin = app_spirit_skin_for_machine(machine);
            app_notify_voice(machine, skin, (unsigned)kind);
            // Screen-level bubble: visible on whichever detail page is open.
            if (s_active && !s_ov_mode && s_midx == machine) {
                const char *text = app_notify_voice_text(skin, (unsigned)kind);
                if (text) bubble_show(text, true);
            }
        }
        if (fresh && action == SPIRIT_COMPLETED) app_spirit_finish_remember(machine, event_status.task_token, false);
    }
    app_notify_poll();
    // 概览列表模式:只刷列表卡片,不动详情页
    if (s_ov_mode) {
        if (s_bubble) lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        s_bubble_until_ms = 0;
        s_bubble_sticky = false;
        ov_refresh();
        s_midx = s_report_machine;
        app_mqtt_snapshot(s_report_machine, &s_work);
        refresh_header(&s_work);
        refresh_status_bar(&s_work);
        // Page dots belong to detail navigation, not to the printer list.
        for (int i = 0; i < PAGE_MAX; i++) lv_obj_set_hidden(s_dots[i], true);
        return;
    }
    app_mqtt_snapshot(s_midx, &s_work);
    const bambu_state_t *current = &s_work;
#define st (*current)
    uint64_t now_ms = spirit_now_ms();
    spirit_status_t status = spirit_status(&st);

    // The screen sleeps after 30s and stops LVGL timers. On wake, greet with
    // the current state instead of leaving the user with a silent black screen.
    bool resumed = s_last_refresh_ms && now_ms > s_last_refresh_ms + 15000ULL;
    s_last_refresh_ms = now_ms;
    if (resumed) {
        s_chatter_seen = false;
        s_chatter_ms = now_ms;
        if (spirit_status_fresh(&status, now_ms)) {
            int kind = voice_kind_for_action(spirit_action_for_status(&status, now_ms));
            const char *text = kind >= 0 ? app_notify_voice_text(app_spirit_skin_for_machine(s_midx), (unsigned)kind) : NULL;
            if (text) bubble_show(text, true);
        }
    }

    int previous_slot = s_cur >= 0 && s_cur < s_pages_n ? s_slot[s_cur] : 0;
    int ams_pages = st.ams_count > 0 ? st.ams_count : 1;
    if (ams_pages > 4) ams_pages = 4;
    int temp_pages = 1; // 1-4 nozzles share one compact page; no pager
    int n = 0; s_slot[n++] = 0;
    for (int i = 0; i < temp_pages; i++) s_slot[n++] = 1 + i;
    for (int i = 0; i < ams_pages; i++) s_slot[n++] = 5 + i;
    s_slot[n++] = 9; s_pages_n = n;
    for (int i = 0; i < n; i++) if (s_slot[i] == previous_slot) { s_cur = i; break; }
    if (s_cur >= n) s_cur = n - 1;
    apply_page_vis(s_slot[s_cur]);
    int slot = s_slot[s_cur];
    s_temperature_pair = slot >= 1 && slot <= 4 ? slot - 1 : 0;
    app_spirit_view_context(st.model, slot >= 1 && slot <= 4 ? 6 : slot >= 5 && slot <= 8 ? 7 : -1, now_ms);
    app_spirit_view_update(s_midx, &status, now_ms);

    refresh_header(&st);
    // Individual telemetry fields can expire while gcode_state stays fresh.
    // Refresh the one visible page once per tick; LVGL skips equal label text.
    refresh_status_bar(&st);

    if (slot == 0) refresh_progress(&st);
    else if (slot <= 4) refresh_temp(&st);
    else if (slot <= 8) refresh_ams(&st, slot - 5, ams_pages);
    else refresh_device(&st);

    refresh_bubble(now_ms);

    // Low-frequency text-only chatter on the progress page. It never asks the
    // audio worker to play; event voices remain tied to state transitions.
    spirit_action_t chatter_action = spirit_action_for_status(&status, now_ms);
    bool chatter_idle = chatter_action == SPIRIT_IDLE ||
                        (chatter_action == SPIRIT_COMPLETED && app_spirit_view_finish_acknowledged(s_midx));
    uint64_t chatter_wait = s_chatter_seen ? CHATTER_PERIOD_MS : CHATTER_FIRST_MS;
    if (slot == 0 && spirit_status_fresh(&status, now_ms) &&
        (chatter_action == SPIRIT_PRINTING || chatter_idle) &&
        now_ms >= s_chatter_ms + chatter_wait && now_ms >= s_bubble_until_ms &&
        !app_notify_busy()) {
        s_chatter_seen = true;
        s_chatter_ms = now_ms;
        bubble_show(chatter_text(app_spirit_skin_for_machine(s_midx), chatter_action, &st, now_ms), false);
    }
#undef st
}


static void tick_1s(lv_timer_t *t)
{
    (void)t;
    if (s_active) {
        refresh_all();
        static uint64_t last_budget_ms;
        uint64_t now_ms = spirit_now_ms();
        if (now_ms - last_budget_ms >= 60000) {
            app_spirit_view_log_budget("periodic");
            last_budget_ms = now_ms;
        }
    }
}

static void tick_once(lv_timer_t *t)
{
    if (t == s_retry) s_retry = NULL;
    lv_timer_delete(t);
    if (s_active) refresh_all();
}

static void schedule_retry(void)
{
    if (s_retry) lv_timer_delete(s_retry);
    s_retry = lv_timer_create(tick_once, 1500, NULL);
}

// ------------------------------------------------------------ 机器概览实现
// 多机列表页:2台=大卡 / 3台=中卡 / 4台=紧凑卡,关键进度信息常驻。
// 单机时跳过本页,直接进详情(与旧行为一致)。

static void ov_build(void)
{
    s_ov_page = page_new();
    lv_obj_set_hidden(s_ov_page, false);
    app_spirit_view_create(s_ov_page, 6, 22);
    s_report_name = label(s_ov_page, &font_zh14, C_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_report_name, 122, 10); lv_obj_set_size(s_report_name, 106, 36);
    lv_label_set_long_mode(s_report_name, LV_LABEL_LONG_DOT);
    s_report_value = label(s_ov_page, &lv_font_montserrat_36, C_GREEN_L, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_report_value, 120, 54);
    s_report_line = label(s_ov_page, &font_zh14, C_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_report_line, 122, 102); lv_obj_set_size(s_report_line, 106, 52);
    lv_label_set_long_mode(s_report_line, LV_LABEL_LONG_WRAP);
    int n = app_mqtt_machine_count();
    for (int i = 0; i < n && i < BAMBU_MACHINE_MAX; i++) {
        s_ov_card[i] = rect(s_ov_page, 8, 172 + i * 23, 224, 22, C_BG);
        s_ov_name[i] = label(s_ov_card[i], &font_zh14, C_DIM, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(s_ov_name[i], 4, 2); lv_obj_set_width(s_ov_name[i], 146);
        lv_label_set_long_mode(s_ov_name[i], LV_LABEL_LONG_DOT);
        s_ov_pct[i] = label(s_ov_card[i], &font_zh14, C_TEXT, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_pos(s_ov_pct[i], 150, 2); lv_obj_set_width(s_ov_pct[i], 68);
    }
    app_spirit_view_set_visible(true);
}

// 状态文字与颜色:RUNNING=打印中/PAUSE=已暂停/FINISH=已完成/FAILED=失败/IDLE=空闲
static uint32_t ov_state_text(const bambu_state_t *st, char *out, int n)
{
    spirit_status_t status = spirit_status(st);
    if (!spirit_status_fresh(&status, spirit_now_ms())) {
        snprintf(out,n,"%s",st->cloud == BAMBU_CLOUD_CONNECTING ? "连接中" : st->cloud != BAMBU_CLOUD_ONLINE ? "离线" : !st->state_received ? "未上报" : "数据过期"); return C_YELLOW;
    }
    uint32_t color;
    const char *text = spirit_action_for_status(&status,spirit_now_ms()) == SPIRIT_RUNOUT ? "缺料" : state_zh(st->gcode_state,&color);
    if (!strcmp(text,"缺料")) color = C_YELLOW;
    snprintf(out,n,"%s",text); return color;
}

static void ov_refresh(void)
{
    int n = app_mqtt_machine_count(); if (n > BAMBU_MACHINE_MAX) n = BAMBU_MACHINE_MAX;
    uint64_t now = spirit_now_ms();
    for (int i = 0; i < n; i++) {
        app_mqtt_snapshot(i,&s_work);
        s_dispatch_status[i].status = spirit_status(&s_work);
        // gcode_state is borrowed from the scratch snapshot; retain a small copy.
        memcpy(s_dispatch_states[i],s_work.gcode_state,sizeof(s_work.gcode_state));
        s_dispatch_status[i].status.gcode_state = s_dispatch_states[i];
        s_dispatch_status[i].hms = s_work.hms_count > 0;
        const char *name = app_mqtt_name(i);
        lv_label_set_text_fmt(s_ov_name[i],"%s %s",name ? name : app_mqtt_sn(i),app_spirit_skin_name(app_spirit_skin_for_machine(i)));
        char text[32]; uint32_t color = ov_state_text(&s_work,text,sizeof(text));
        bool fresh = spirit_status_fresh(&s_dispatch_status[i].status,now);
        if (fresh && !strcmp(s_work.gcode_state,"RUNNING") && app_telemetry_fresh(s_work.pct_valid,s_work.pct_ms,now)) lv_label_set_text_fmt(s_ov_pct[i],"%d%%",s_work.pct);
        else lv_label_set_text(s_ov_pct[i],text);
        lv_obj_set_style_text_color(s_ov_pct[i],lv_color_hex(color),0);
        lv_obj_set_style_bg_color(s_ov_card[i],lv_color_hex(i == s_ov_sel ? C_PANEL2 : C_BG),0);
    }
    int selected = spirit_dispatch_choose(&s_dispatch,s_dispatch_status,(size_t)n,now);
    if (selected < 0) return;
    bool reply = false;
    // An idle partner answers only during a normal printing report; exceptions
    // never lose focus to chatter. The detail target is unaffected.
    if (s_dispatch.priority == 0 && now - s_dispatch.since_ms >= 4000 &&
        spirit_action_for_status(&s_dispatch_status[selected].status,now) == SPIRIT_PRINTING) {
        for (int i=1; i<n; i++) {
            int candidate=(selected+i)%n;
            if (spirit_status_fresh(&s_dispatch_status[candidate].status,now) &&
                spirit_action_for_status(&s_dispatch_status[candidate].status,now)==SPIRIT_IDLE) {
                selected=candidate; reply=true; break;
            }
        }
    }
    s_report_machine = selected;
    app_mqtt_snapshot(selected,&s_work);
    const char *name = app_mqtt_name(selected);
    lv_label_set_text_fmt(s_report_name,"%s\n%s",name ? name : app_mqtt_sn(selected),app_spirit_skin_name(app_spirit_skin_for_machine(selected)));
    spirit_status_t status = spirit_status(&s_work);
    bool fresh = spirit_status_fresh(&status,now);
    spirit_action_t action = spirit_action_for_status(&status,now);
    bool running = fresh && action == SPIRIT_PRINTING && app_telemetry_fresh(s_work.pct_valid,s_work.pct_ms,now);
    lv_label_set_text_fmt(s_report_value,running ? "%d%%" : "--",s_work.pct);
    char text[48]; ov_state_text(&s_work,text,sizeof(text));
    if (running && app_telemetry_fresh(s_work.remain_valid,s_work.remain_ms,now)) snprintf(text,sizeof(text),"剩余 %dh%02dm",s_work.remain_min/60,s_work.remain_min%60);
    if (reply) {
        static const char *const lines[SPIRIT_SKIN_COUNT] = {"我这边空闲，陪你等呀。","我这里空闲，一起等它完成。","当前空闲，继续协助观察。","我这边没活，陪你盯会儿。","我这边幕间休息，等它谢幕。","我这边空着，陪你看它忙吧。"};
        snprintf(text,sizeof(text),"%s",lines[app_spirit_skin_for_machine(selected)]);
    }
    if (fresh && action == SPIRIT_PAUSED) snprintf(text,sizeof(text),"嗯？暂停了，请检查。");
    lv_label_set_text(s_report_line,text);
    app_spirit_view_context(s_work.model,-1,now);
    app_spirit_view_update(selected,&status,now);
    app_spirit_view_set_visible(true);
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
        app_spirit_view_destroy();
        if (s_ov_page) { lv_obj_delete(s_ov_page); s_ov_page = NULL; }
        detail_build();
        s_cur = 0;
        s_show_eta = false;
        show_page(0);
        return;
    }

    request_refresh();
}

// ------------------------------------------------------------ 对外

// 只显示 show 对应的实体页,其余全部隐藏(NULL = 未建的 AMS 页,跳过)
static void apply_page_vis(int show)
{
    if (show < 0 || show >= PAGE_MAX) return;
    if (s_built_slot != show) {
        // Ambient chatter is page-local; an event bubble follows the user.
        if (s_bubble && !s_bubble_sticky && s_bubble_until_ms) {
            lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
            s_bubble_until_ms = 0;
        }
        detail_free();
        s_pages[show] = page_new();
        if (show == 0) page_progress_build(s_pages[show]);
        else if (show <= 4) page_temp_build(s_pages[show]);
        else if (show <= 8) page_ams_build(s_pages[show], show - 5);
        else page_device_build(s_pages[show]);
        s_built_slot = show;
    }
    lv_obj_set_hidden(s_pages[show], false);
    if (s_bubble) lv_obj_move_foreground(s_bubble);
    app_spirit_view_set_visible(true);

    // Per-page entry dialogue: bubble text plus the matching role clip. Only on
    // an actual page entry (refresh_all re-applies the same slot every second),
    // never while an event bubble is still up.
    uint64_t now = spirit_now_ms();
    if (s_active && !s_ov_mode && show != s_hint_slot) {
        s_hint_slot = show;
        if (!(s_bubble_sticky && s_bubble_until_ms > now)) {
            app_mqtt_snapshot(s_midx, &s_work);
            unsigned skin = (unsigned)app_spirit_skin_for_machine(s_midx);
            int page = show <= 4 ? 0 : show <= 8 ? 1 : 2;
            unsigned variant = 0;
            if (show >= 1 && show <= 9) {
                // Alternate wording on each entry; never repeat the previous one.
                variant = esp_random() % 2u;
                if ((int)variant == s_hint_variant[page]) variant ^= 1u;
                s_hint_variant[page] = (int)variant;
            }
            const char *text = page_hint_text(show, (spirit_skin_id_t)skin, &s_work,
                                              now, variant);
            if (text) bubble_show(text, false);
            if (show >= 1 && show <= 9)
                app_notify_page_voice(s_midx, skin, (unsigned)page, variant);
        }
    }
}

static void show_page(int idx)
{
    if (idx >= s_pages_n) idx = s_pages_n - 1;
    if (idx < 0) idx = 0;
    // 导航序号 ≠ 实体槽位。实体固定为 0进度/1温度/2~5AMS/6设备,
    // 而实际显示哪几页随 AMS 台数变化,必须经 s_slot 映射,否则 AMS 不足 4 台时
    // 会翻到空 AMS 页,设备页反而永远翻不到。
    int show = (idx >= 0 && idx < s_pages_n && idx < PAGE_MAX) ? s_slot[idx] : 0;
    s_hint_slot = -1;       // an explicit key press re-arms the page dialogue
    apply_page_vis(show);
    s_cur = idx;
    request_refresh();      // 延到 LVGL 任务里重绘(不在按键回调里干重活)
}

// 详情页(最多7个实体页)按需构建/释放:
// LVGL 内存池紧张,概览页与详情页对象绝不能共存 —— 切换时一建一删;
// AMS 页也按需建(2 台 AMS 不建 4 页,空页白吃池内存曾把池压到 98% 导致渲染崩溃)。
// 新页一律先隐藏:页面背景透明,可见的新页会直接叠画在当前页上
// (曾因此把 AMS 页内容混进首页),由 show_page/apply_page_vis 统一控制可见性。
static lv_obj_t *page_new(void)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(p, 0, PAGE_Y);
    lv_obj_set_size(p, 240, PAGE_H);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    return p;
}

// 补建前 n 个 AMS 页(已有则跳过);refresh_all 随数据到达逐页补齐
static void detail_build(void)
{
    s_built_slot = -1;
    apply_page_vis(0);
}

static void detail_free(void)
{
    s_built_slot = -1;
    s_time_hint = NULL;
    app_spirit_view_destroy();
    if (s_retry) { lv_timer_delete(s_retry); s_retry = NULL; }
    for (int i = 0; i < PAGE_MAX; i++) {
        if (s_pages[i]) { lv_obj_delete(s_pages[i]); s_pages[i] = NULL; }
    }
    // Leaving detail to the overview: hide the screen-level bubble. Page
    // switches keep it alive so an event is not lost while browsing pages.
    if (s_ov_mode && s_bubble) {
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        s_bubble_until_ms = 0;
        s_bubble_sticky = false;
    }
}

void app_printer_enter(void)
{
    if (s_scr) return;
    app_notify_init();
    app_spirit_bindings_poll();

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    header_build();
    status_bar_build();
    bubble_build();

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

    s_active = true;
    s_hint_slot = -1;
    for (int i = 0; i < 3; i++) s_hint_variant[i] = -1;
    s_chatter_ms = spirit_now_ms();
    s_chatter_seen = false;
    s_last_refresh_ms = s_chatter_ms;
    s_timer = lv_timer_create(tick_1s, 1000, NULL);
    s_fast  = lv_timer_create(tick_fast, 30, NULL);   // 承接按键延后的重绘
    s_slot[0] = 0; s_slot[1] = 1; s_slot[2] = 9;  // 默认(还没 AMS 数据)
    s_pages_n = 3;
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
    app_spirit_view_log_budget("enter");
}

void app_printer_exit(void)
{
    if (!s_scr) return;
    s_active = false;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_fast)  { lv_timer_delete(s_fast);  s_fast  = NULL; }
    if (s_retry) { lv_timer_delete(s_retry); s_retry = NULL; }
    app_spirit_view_destroy();
    s_bubble = s_bubble_lbl = NULL;
    s_bubble_until_ms = 0;
    s_bubble_sticky = false;
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

    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        show_page((s_cur + s_pages_n - 1) % s_pages_n);
    } else if (btn == BSP_BTN_DOWN) {
        show_page((s_cur + 1) % s_pages_n);
    } else if (btn == BSP_BTN_OK) {
        if (s_cur == 0) {
            app_mqtt_snapshot(s_midx, &s_work);
#define st s_work
            uint64_t now_ms = spirit_now_ms();
            spirit_status_t status = spirit_status(&st);
            if (!spirit_status_fresh(&status, now_ms)) {
                app_mqtt_request_now(s_midx);
                schedule_retry();
            } else if (!app_spirit_view_acknowledge(s_midx, &status, now_ms)) {
                s_show_eta = !s_show_eta;
            }
            if (app_spirit_view_finish_acknowledged(s_midx)) app_spirit_finish_remember(s_midx, spirit_task_token(st.task_id), true);
#undef st
            refresh_all();
        } else {
            app_mqtt_request_now(s_midx);
            refresh_all();
            schedule_retry();
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
