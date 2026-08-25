// main/demo_party.c —— 酒桌游戏:随机抽签类派对小游戏合集。
// 主菜单选游戏/选人数 → 抽签 → 每局结束随机抽一位「酒神」展示结算图,举起来拍照留念。
// 沿用 demo 三函数契约(demo.h)与 ui_pixel 像素风。按键:UP/DOWN 切换,OK 确认/抽签,
// OK 长按由 main.c 统一拦截回菜单。
#include "demo.h"
#include "ui_pixel.h"
#include "font_zh.h"
#include "party_data.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "party";

// ---------- 页面 ----------
typedef enum { P_MENU, P_PLAYERS, P_PLAY, P_RESULT } page_t;

// 菜单项索引:0..3 四个游戏,4 = 玩家人数设置
enum { IDX_TRUTH = 0, IDX_KING, IDX_ROULETTE, IDX_FATE, IDX_PLAYERS, IDX_N };

static const char *GAME_NAMES[] = { "真心话大冒险", "国王游戏", "俄罗斯转盘", "命运签" };

// ---------- 全局状态 ----------
static lv_obj_t *s_scr;
static lv_obj_t *s_panel;
static lv_obj_t *s_text;       // 结果/提示主文本
static lv_obj_t *s_hint;       // 底部操作提示
static lv_obj_t *s_cards[IDX_N];
static lv_obj_t *s_rows[IDX_N];

// 前向声明(定义在使用之后)
static void result_show(void);
static void players_show(void);
static void play_enter(void);

static page_t s_page = P_MENU;
static int    s_sel = 0;
static int    s_players = 4;   // 玩家人数 2..8
static int    s_game = 0;      // 当前游戏
static int    s_truth_mode = 0;// 0=真心话 1=大冒险
static int    s_drawn = 0;     // 本轮是否已抽过(抽过→再按 OK 进结算)
static char   s_buf[160];

// ---------- 像素块 helper(与 ui_pixel.c 同款) ----------
static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

// ---------- 屏重建辅助 ----------
static void clear_scr(void)
{
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_panel = s_text = s_hint = NULL;
    for (int i = 0; i < IDX_N; i++) s_cards[i] = s_rows[i] = NULL;
}

// 内容面板:标题牌下方的大区域
static lv_obj_t *content_panel(void)
{
    s_panel = ui_pixel_panel_create(s_scr, 18, 58, 204, 178, UI_PAPER);
    s_text = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_text, &font_zh14, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_text, 176);
    lv_obj_align(s_text, LV_ALIGN_CENTER, 0, -10);
    return s_text;
}

static void set_hint(const char *txt)
{
    if (s_hint) lv_label_set_text(s_hint, txt);
}

// ---------- 主菜单 ----------
static void menu_refresh(void)
{
    for (int i = 0; i < IDX_N; i++) {
        if (i == IDX_PLAYERS) {
            lv_label_set_text_fmt(s_rows[i], "玩家人数 %d", s_players);
        } else {
            lv_label_set_text(s_rows[i], GAME_NAMES[i]);
        }
        ui_pixel_set_selected(s_cards[i], i == s_sel, true);
    }
}

static void menu_show(void)
{
    clear_scr();
    s_page = P_MENU;
    s_scr = ui_pixel_screen_create("酒桌游戏");

    for (int i = 0; i < IDX_N; i++) {
        int x = 16;
        int y = 52 + i * 48;
        s_cards[i] = ui_pixel_panel_create(s_scr, x, y, 208, 40, UI_PAPER);
        s_rows[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_rows[i], &font_zh14, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_rows[i]);
    }
    menu_refresh();
    lv_screen_load(s_scr);
}

static void menu_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP)   { s_sel = (s_sel + IDX_N - 1) % IDX_N; menu_refresh(); }
    else if (btn == BSP_BTN_DOWN) { s_sel = (s_sel + 1) % IDX_N; menu_refresh(); }
    else if (btn == BSP_BTN_OK) {
        if (s_sel == IDX_PLAYERS) { players_show(); }
        else { s_game = s_sel; play_enter(); }
    }
}

// ---------- 选人数 ----------
static void players_show(void)
{
    clear_scr();
    s_page = P_PLAYERS;
    s_scr = ui_pixel_screen_create("玩家人数");

    s_panel = ui_pixel_panel_create(s_scr, 18, 58, 204, 178, UI_PAPER);

    // 大数字(用 32px 结算字体)
    s_text = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_text, &font_game, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(UI_RED), 0);
    lv_obj_align(s_text, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *unit = lv_label_create(s_panel);
    lv_obj_set_style_text_font(unit, &font_zh14, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(UI_INK), 0);
    lv_label_set_text(unit, "人");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 26);

    lv_obj_t *tip = lv_label_create(s_panel);
    lv_obj_set_style_text_font(tip, &font_zh14, 0);
    lv_obj_set_style_text_color(tip, lv_color_hex(UI_INK), 0);
    lv_label_set_text(tip, "UP/DOWN 调整  OK 确认");
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_label_set_text_fmt(s_text, "%d", s_players);
    lv_screen_load(s_scr);
}

static void players_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP)   { if (s_players < 8) s_players++; }
    else if (btn == BSP_BTN_DOWN) { if (s_players > 2) s_players--; }
    else if (btn == BSP_BTN_OK) { menu_show(); return; }
    else return;
    lv_label_set_text_fmt(s_text, "%d", s_players);
}

// ---------- 游戏进行页 ----------
static void play_enter(void)
{
    clear_scr();
    s_page = P_PLAY;
    s_drawn = 0;
    s_scr = ui_pixel_screen_create(GAME_NAMES[s_game]);
    content_panel();

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &font_zh14, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, 70);

    switch (s_game) {
    case IDX_TRUTH:
        s_truth_mode = 0;
        lv_label_set_text(s_text, "真心话\n\n按 OK 抽题");
        set_hint("UP/DOWN 切换真心话/大冒险");
        break;
    case IDX_KING:
        lv_label_set_text(s_text, "国王游戏\n\n按 OK 抽国王令");
        set_hint("OK 抽国王令");
        break;
    case IDX_ROULETTE:
        lv_label_set_text(s_text, "俄罗斯转盘\n\n按 OK 扣扳机");
        set_hint("OK 扣扳机");
        break;
    case IDX_FATE:
        lv_label_set_text(s_text, "命运签\n\n按 OK 抽签");
        set_hint("OK 抽签");
        break;
    }
    lv_screen_load(s_scr);
}

static void play_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (s_game == IDX_TRUTH && s_drawn == 0 && btn != BSP_BTN_OK) {
        // 未抽题前:UP/DOWN 切换真心话/大冒险
        s_truth_mode = !s_truth_mode;
        lv_label_set_text(s_text, s_truth_mode ? "大冒险\n\n按 OK 抽题" : "真心话\n\n按 OK 抽题");
        return;
    }
    if (btn != BSP_BTN_OK) return;

    if (s_drawn == 0) {
        // 第一次按 OK:抽签
        switch (s_game) {
        case IDX_TRUTH:
            snprintf(s_buf, sizeof(s_buf), "%s", s_truth_mode ? party_pick_dare() : party_pick_truth());
            break;
        case IDX_KING:
            party_king_text(s_buf, sizeof(s_buf), s_players);
            break;
        case IDX_ROULETTE:
            snprintf(s_buf, sizeof(s_buf), "%s", party_roulette_shot() ? "中弹！罚一杯" : "空枪，安全");
            break;
        case IDX_FATE:
            snprintf(s_buf, sizeof(s_buf), "%s", party_pick_fate());
            break;
        }
        lv_label_set_text(s_text, s_buf);
        set_hint("OK 揭晓酒神");
        s_drawn = 1;
    } else {
        // 第二次按 OK:揭晓酒神结算图
        result_show();
    }
}

// ---------- 酒神结算图 ----------
static void draw_trophy(lv_obj_t *parent, int x, int y)
{
    const uint32_t G  = 0xFFD928;   // 金
    const uint32_t GD = 0xC79A00;   // 深金
    block(parent, x + 10, y,      20, 6, G);   // 杯口
    block(parent, x + 12, y + 6,  16, 4, GD);
    block(parent, x + 14, y + 10, 12, 10, G);  // 杯身
    block(parent, x + 16, y + 20, 8,  8,  G);
    block(parent, x + 17, y + 28, 6,  6,  GD);
    block(parent, x + 19, y + 34, 2,  8,  G);  // 杯柄
    block(parent, x + 12, y + 42, 16, 4, G);   // 底座
    block(parent, x + 8,  y + 46, 24, 6, G);
    block(parent, x + 8,  y + 52, 24, 3, GD);
    block(parent, x + 6,  y + 8,  4,  14, G);  // 把手
    block(parent, x + 30, y + 8,  4,  14, G);
}

static void result_show(void)
{
    int winner = party_pick_winner(s_players);
    clear_scr();
    s_page = P_RESULT;

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    // 顶部横幅
    block(s_scr, 0, 0, 240, 34, UI_RED);
    lv_obj_t *top = lv_label_create(s_scr);
    lv_obj_set_style_text_font(top, &font_zh14, 0);
    lv_obj_set_style_text_color(top, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(top, "胜利结算");
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 7);

    // 像素奖杯
    draw_trophy(s_scr, 100, 48);

    // 酒神大字
    lv_obj_t *god = lv_label_create(s_scr);
    lv_obj_set_style_text_font(god, &font_game, 0);
    lv_obj_set_style_text_color(god, lv_color_hex(UI_RED), 0);
    lv_label_set_text(god, "酒神");
    lv_obj_align(god, LV_ALIGN_CENTER, 0, -40);

    // 玩家 N 号
    snprintf(s_buf, sizeof(s_buf), "玩家 %d 号", winner);
    lv_obj_t *who = lv_label_create(s_scr);
    lv_obj_set_style_text_font(who, &font_game, 0);
    lv_obj_set_style_text_color(who, lv_color_hex(UI_INK), 0);
    lv_label_set_text(who, s_buf);
    lv_obj_align(who, LV_ALIGN_CENTER, 0, 8);

    // 干杯
    lv_obj_t *cheers = lv_label_create(s_scr);
    lv_obj_set_style_text_font(cheers, &font_zh14, 0);
    lv_obj_set_style_text_color(cheers, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(cheers, "干杯！");
    lv_obj_align(cheers, LV_ALIGN_CENTER, 0, 52);

    // 底部横幅 + 提示
    block(s_scr, 0, 286, 240, 34, UI_RED);
    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &font_zh14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(hint, "举起来拍照吧  OK 再来一局");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -9);

    lv_screen_load(s_scr);
}

static void result_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) play_enter();   // 同游戏再来一局
}

// ---------- demo 三函数契约 ----------
void demo_party_enter(void)
{
    s_players = 4;
    s_sel = 0;
    menu_show();
}

void demo_party_exit(void)
{
    clear_scr();
}

void demo_party_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    switch (s_page) {
    case P_MENU:    menu_key(btn, ev); break;
    case P_PLAYERS: players_key(btn, ev); break;
    case P_PLAY:    play_key(btn, ev); break;
    case P_RESULT:  result_key(btn, ev); break;
    }
}
