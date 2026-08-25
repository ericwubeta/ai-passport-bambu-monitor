// main/app_boot.c —— 开机动画:深色底、拓竹官方 logo 淡入上浮、
// 标语"打印机监控"、绿色加载条走满,约 1.8 秒后回调交给主流程。
#include "app_boot.h"

#include <string.h>

#include "font_zh.h"
#include "logo_bambu.h"
#include "lvgl.h"

#define BOOT_BG   0x0A0E14
#define C_GREEN   0x00AE42
#define C_DIM     0x7AA08C
#define BAR_W     150

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *bar_fill;
    lv_obj_t *logo;
    lv_timer_t *fin;
    void (*done)(void);
} boot_ctx_t;

static boot_ctx_t s_ctx;

static void bar_anim(void *obj, int32_t v)
{
    lv_obj_set_width((lv_obj_t *)obj, v);
}

static void fade_in_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void float_up_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void finish(lv_timer_t *t)
{
    lv_timer_delete(t);
    void (*done)(void) = s_ctx.done;
    lv_obj_t *boot_scr = s_ctx.scr;
    memset(&s_ctx, 0, sizeof(s_ctx));
    // 先让 done() 创建并 load 新屏幕,再删开机动画屏幕,
    // 避免 LVGL 活动屏幕变成 NULL 导致后续 set_style 崩溃。
    if (done) done();
    if (boot_scr) lv_obj_delete(boot_scr);
}

void app_boot_show(void (*done)(void))
{
    s_ctx.done = done;
    s_ctx.scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_ctx.scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ctx.scr, lv_color_hex(BOOT_BG), 0);
    lv_obj_set_style_border_width(s_ctx.scr, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.scr, 0, 0);

    // 官方 logo(240x87,含竹叶绿 + Bambu 白字标),居中偏上
    s_ctx.logo = lv_image_create(s_ctx.scr);
    lv_image_set_src(s_ctx.logo, &logo_bambu_img);
    lv_obj_align(s_ctx.logo, LV_ALIGN_CENTER, 0, -72);
    lv_obj_set_style_opa(s_ctx.logo, LV_OPA_MIN, 0);

    // 标语"打印机监控"
    lv_obj_t *tag = lv_label_create(s_ctx.scr);
    lv_obj_set_style_text_font(tag, &font_zh14, 0);
    lv_obj_set_style_text_color(tag, lv_color_hex(C_DIM), 0);
    lv_label_set_text(tag, "打印机监控");
    lv_obj_align(tag, LV_ALIGN_CENTER, 0, -14);

    // 加载条:外框 + 内部绿色填充
    lv_obj_t *track = lv_obj_create(s_ctx.scr);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(track, BAR_W + 8, 14);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, 34);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_set_style_border_width(track, 2, 0);
    lv_obj_set_style_border_color(track, lv_color_hex(0x1F3A2C), 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(BOOT_BG), 0);
    lv_obj_set_style_pad_all(track, 2, 0);

    s_ctx.bar_fill = lv_obj_create(track);
    lv_obj_remove_flag(s_ctx.bar_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ctx.bar_fill, 0, 0);
    lv_obj_set_size(s_ctx.bar_fill, 0, 6);
    lv_obj_set_style_radius(s_ctx.bar_fill, 0, 0);
    lv_obj_set_style_border_width(s_ctx.bar_fill, 0, 0);
    lv_obj_set_style_bg_color(s_ctx.bar_fill, lv_color_hex(C_GREEN), 0);
    lv_obj_set_style_pad_all(s_ctx.bar_fill, 0, 0);

    lv_screen_load(s_ctx.scr);

    // logo 淡入 + 轻微上浮(从 -72 浮到 -80)
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_ctx.logo);
    lv_anim_set_exec_cb(&fade, fade_in_cb);
    lv_anim_set_values(&fade, LV_OPA_10, LV_OPA_COVER);
    lv_anim_set_duration(&fade, 650);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_start(&fade);

    lv_anim_t up;
    lv_anim_init(&up);
    lv_anim_set_var(&up, s_ctx.logo);
    lv_anim_set_exec_cb(&up, float_up_cb);
    lv_anim_set_values(&up, -64, -80);
    lv_anim_set_duration(&up, 900);
    lv_anim_set_path_cb(&up, lv_anim_path_ease_out);
    lv_anim_start(&up);

    // 加载条 0 → BAR_W
    lv_anim_t bar;
    lv_anim_init(&bar);
    lv_anim_set_var(&bar, s_ctx.bar_fill);
    lv_anim_set_exec_cb(&bar, bar_anim);
    lv_anim_set_values(&bar, 0, BAR_W);
    lv_anim_set_duration(&bar, 1300);
    lv_anim_set_path_cb(&bar, lv_anim_path_ease_in_out);
    lv_anim_start(&bar);

    // 1.8 秒后收尾进主界面
    s_ctx.fin = lv_timer_create(finish, 1800, NULL);
}