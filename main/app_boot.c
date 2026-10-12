// main/app_boot.c —— 开机动画:拓竹 logo + 赛博终端启动(逐字 boot 日志 + exec 高亮 + 霓虹加载条)
// 结束后回调交给主流程(打印机状态屏)。
#include "app_boot.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "font_zh.h"
#include "logo_bambu.h"
#include "lvgl.h"

#define BG      0x04070A
#define PANEL   0x06120F
#define BORDER  0x0E4A43
#define NEON    0x19E3C0
#define ACCENT  0xFF4D8D
#define DIM     0x2E6B62

// boot 日志(行宽控制在 ~22 字符内,适配 14px 字体与面板宽度)
// 第 0 行在 app_boot_show 里用 FW_VERSION 填充,不再硬编码版本号。
static const char *k_lines[5] = {
    "> boot v?.?.?",
    "> link wifi ...... ok",
    "> link bambu ..... ok",
    "> mount panel .... ok",
    "> exec monitor()",
};
static char s_line0[32];

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *log;
    lv_obj_t *exec;
    lv_obj_t *bar;
    lv_obj_t *ready;
    lv_obj_t *cursor;
    lv_timer_t *typ;
    lv_timer_t *cur;
    lv_timer_t *fin;
    void (*done)(void);
    int li;
    int ch;
    int pause;
    char logbuf[160];
    char execbuf[48];
} boot_ctx_t;

static boot_ctx_t s_ctx;

static void real_finish(lv_timer_t *t)
{
    lv_timer_delete(t);
    void (*done)(void) = s_ctx.done;
    lv_obj_t *scr = s_ctx.scr;
    if (s_ctx.typ) lv_timer_delete(s_ctx.typ);
    if (s_ctx.cur) lv_timer_delete(s_ctx.cur);
    ESP_LOGI("app_boot", "terminal done -> menu");
    memset(&s_ctx, 0, sizeof(s_ctx));
    if (done) done();
    if (scr) lv_obj_delete(scr);
}

static void bar_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_width((lv_obj_t *)obj, v);
}

static void bar_complete_cb(lv_anim_t *a)
{
    lv_obj_set_style_opa(s_ctx.ready, LV_OPA_COVER, 0);
    s_ctx.fin = lv_timer_create(real_finish, 600, NULL);
}

static void start_bar(void)
{
    lv_obj_set_style_opa(s_ctx.ready, LV_OPA_MIN, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ctx.bar);
    lv_anim_set_exec_cb(&a, bar_anim_cb);
    lv_anim_set_values(&a, 0, 144);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, bar_complete_cb);
    lv_anim_start(&a);
}

static void cursor_cb(lv_timer_t *t)
{
    static bool on = false;
    on = !on;
    lv_obj_set_style_opa(s_ctx.cursor, on ? LV_OPA_COVER : LV_OPA_MIN, 0);
}

static void type_one(void)
{
    boot_ctx_t *c = &s_ctx;
    if (c->li >= 5) {
        lv_obj_set_style_opa(c->cursor, LV_OPA_MIN, 0);
        return;
    }
    if (c->pause > 0) {
        c->pause -= 14;
        return;
    }
    const char *line = k_lines[c->li];
    if (c->li < 4) {
        if (c->ch == 0 && c->li > 0) strcat(c->logbuf, "\n");
        char cc = line[c->ch];
        if (cc) {
            char tmp[2] = {cc, 0};
            strcat(c->logbuf, tmp);
        }
        c->ch++;
        lv_label_set_text(c->log, c->logbuf);
        if (line[c->ch] == 0) {
            c->li++;
            c->ch = 0;
            c->pause = 90;
        }
    } else {
        char cc = line[c->ch];
        if (cc) {
            char tmp[2] = {cc, 0};
            strcat(c->execbuf, tmp);
        }
        c->ch++;
        lv_label_set_text(c->exec, c->execbuf);
        if (line[c->ch] == 0) {
            c->li = 5;
            start_bar();
        }
    }
}

static void typing_cb(lv_timer_t *t)
{
    type_one();
    type_one();
}

void app_boot_show(void (*done)(void))
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.done = done;
    s_ctx.scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_ctx.scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ctx.scr, lv_color_hex(BG), 0);
    lv_obj_set_style_border_width(s_ctx.scr, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.scr, 0, 0);

    // 拓竹 logo(240x87 深色底位图)
    lv_obj_t *logo = lv_image_create(s_ctx.scr);
    lv_image_set_src(logo, &logo_bambu_img);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 30);

    // 终端面板
    lv_obj_t *panel = lv_obj_create(s_ctx.scr);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(panel, 208, 132);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 16, 126);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(PANEL), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(BORDER), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(NEON), 0);
    lv_obj_set_style_shadow_width(panel, 6, 0);
    lv_obj_set_style_shadow_opa(panel, 30, 0);
    lv_obj_set_style_shadow_spread(panel, 0, 0);

    // 标题栏
    lv_obj_t *hb = lv_label_create(panel);
    lv_obj_set_style_text_font(hb, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(hb, lv_color_hex(DIM), 0);
    lv_label_set_text(hb, "BAMBU MONITOR // BOOT");
    lv_obj_align(hb, LV_ALIGN_TOP_LEFT, 8, 6);

    // 内容区(flex 纵向,自动堆叠)
    lv_obj_t *content = lv_obj_create(panel);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(content, 188, 100);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 6, 26);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 2, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(PANEL), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 4, 0);

    s_ctx.log = lv_label_create(content);
    lv_obj_set_style_text_font(s_ctx.log, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(s_ctx.log, lv_color_hex(NEON), 0);
    lv_obj_set_width(s_ctx.log, 180);
    lv_label_set_long_mode(s_ctx.log, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_ctx.log, "");

    s_ctx.exec = lv_label_create(content);
    lv_obj_set_style_text_font(s_ctx.exec, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(s_ctx.exec, lv_color_hex(ACCENT), 0);
    lv_obj_set_width(s_ctx.exec, 180);
    lv_label_set_long_mode(s_ctx.exec, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_ctx.exec, "");

    // 光标块(在 exec 之后,随内容下移)
    s_ctx.cursor = lv_obj_create(content);
    lv_obj_remove_flag(s_ctx.cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ctx.cursor, 7, 12);
    lv_obj_set_style_bg_color(s_ctx.cursor, lv_color_hex(NEON), 0);
    lv_obj_set_style_border_width(s_ctx.cursor, 0, 0);
    lv_obj_set_style_radius(s_ctx.cursor, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.cursor, 0, 0);

    // 加载条
    lv_obj_t *track = lv_obj_create(s_ctx.scr);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(track, 148, 12);
    lv_obj_align(track, LV_ALIGN_TOP_LEFT, 46, 270);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_border_color(track, lv_color_hex(BORDER), 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(BG), 0);
    lv_obj_set_style_pad_all(track, 1, 0);

    s_ctx.bar = lv_obj_create(track);
    lv_obj_remove_flag(s_ctx.bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ctx.bar, 0, 0);
    lv_obj_set_size(s_ctx.bar, 0, 10);
    lv_obj_set_style_radius(s_ctx.bar, 0, 0);
    lv_obj_set_style_border_width(s_ctx.bar, 0, 0);
    lv_obj_set_style_bg_color(s_ctx.bar, lv_color_hex(NEON), 0);
    lv_obj_set_style_shadow_color(s_ctx.bar, lv_color_hex(NEON), 0);
    lv_obj_set_style_shadow_width(s_ctx.bar, 8, 0);
    lv_obj_set_style_shadow_opa(s_ctx.bar, 80, 0);

    s_ctx.ready = lv_label_create(s_ctx.scr);
    lv_obj_set_style_text_font(s_ctx.ready, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(s_ctx.ready, lv_color_hex(NEON), 0);
    lv_label_set_text(s_ctx.ready, "READY");
    lv_obj_align(s_ctx.ready, LV_ALIGN_TOP_LEFT, 0, 294);
    lv_obj_set_style_opa(s_ctx.ready, LV_OPA_MIN, 0);
    lv_obj_set_style_text_letter_space(s_ctx.ready, 3, 0);

    // 版本号写进首行 boot 日志(不再硬编码)
    snprintf(s_line0, sizeof(s_line0), "> boot v%s", FW_VERSION);
    k_lines[0] = s_line0;

    lv_screen_load(s_ctx.scr);

    ESP_LOGI("app_boot", "terminal boot start");
    s_ctx.typ = lv_timer_create(typing_cb, 14, NULL);
    s_ctx.cur = lv_timer_create(cursor_cb, 250, NULL);
}
