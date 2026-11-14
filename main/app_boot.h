// main/app_boot.h —— PrintPet Spirit 产品名与终端开机动画。
#pragma once

// 显示开机画面,动画结束后在 LVGL 上下文里回调 done(此时已持锁)。
// done 回调里应加载下一个屏幕(本工程:进入打印机主界面)。
void app_boot_show(void (*done)(void));
