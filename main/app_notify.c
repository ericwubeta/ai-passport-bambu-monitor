// main/app_notify.c —— 打印状态提示音。
// 用 ES8311 播放简短短旋律(16kHz 16bit 单声道,正弦查表带淡入淡出)。
// bsp_audio_write 会阻塞,故所有播放放在独立任务里,不占用按键/LVGL 任务。
#include "app_notify.h"

#include <stdlib.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SAMPLE_RATE   16000
#define CHUNK         256

// 256 点 16bit 正弦表(占 512B 静态)
static const int16_t SIN_T[256] = {
    0, 804, 1607, 2410, 3211, 4011, 4807, 5601, 6392, 7179, 7961, 8739, 9511, 10278,
    11038, 11792, 12539, 13278, 14009, 14732, 15446, 16151, 16845, 17530, 18204, 18867,
    19519, 20159, 20787, 21402, 22004, 22594, 23169, 23731, 24278, 24811, 25329, 25831,
    26318, 26789, 27244, 27683, 28105, 28510, 28898, 29268, 29621, 29955, 30272, 30571,
    30851, 31113, 31356, 31580, 31785, 31971, 32137, 32284, 32412, 32520, 32609, 32678,
    32727, 32757, 32767, 32757, 32727, 32678, 32609, 32520, 32412, 32284, 32137, 31971,
    31785, 31580, 31356, 31113, 30851, 30571, 30272, 29955, 29621, 29268, 28898, 28510,
    28105, 27683, 27244, 26789, 26318, 25831, 25329, 24811, 24278, 23731, 23169, 22594,
    22004, 21402, 20787, 20159, 19519, 18867, 18204, 17530, 16845, 16151, 15446, 14732,
    14009, 13278, 12539, 11792, 11038, 10278, 9511, 8739, 7961, 7179, 6392, 5601, 4807,
    4011, 3211, 2410, 1607, 804, 0, -804, -1607, -2410, -3211, -4011, -4807, -5601,
    -6392, -7179, -7961, -8739, -9511, -10278, -11038, -11792, -12539, -13278, -14009,
    -14732, -15446, -16151, -16845, -17530, -18204, -18867, -19519, -20159, -20787,
    -21402, -22004, -22594, -23169, -23731, -24278, -24811, -25329, -25831, -26318,
    -26789, -27244, -27683, -28105, -28510, -28898, -29268, -29621, -29955, -30272,
    -30571, -30851, -31113, -31356, -31580, -31785, -31971, -32137, -32284, -32412,
    -32520, -32609, -32678, -32727, -32757, -32767, -32757, -32727, -32678, -32609,
    -32520, -32412, -32284, -32137, -31971, -31785, -31580, -31356, -31113, -30851,
    -30571, -30272, -29955, -29621, -29268, -28898, -28510, -28105, -27683, -27244,
    -26789, -26318, -25831, -25329, -24811, -24278, -23731, -23169, -22594, -22004,
    -21402, -20787, -20159, -19519, -18867, -18204, -17530, -16845, -16151, -15446,
    -14732, -14009, -13278, -12539, -11792, -11038, -10278, -9511, -8739, -7961, -7179,
    -6392, -5601, -4807, -4011, -3211, -2410, -1607, -804,
};

typedef struct { int hz; int ms; } note_t;

// 完成:上行音阶(明亮);失败:下行(低沉);暂停:两声短促
static const note_t S_FINISH[] = { { 523, 120 }, { 659, 120 }, { 784, 120 }, { 1047, 220 }, { 0, 0 } };
static const note_t S_FAIL[]   = { { 523, 150 }, { 392, 150 }, { 330, 150 }, { 262, 260 }, { 0, 0 } };
static const note_t S_PAUSE[]  = { { 659, 90 },  { 0, 60 },   { 659, 130 }, { 0, 0 } };

static TaskHandle_t s_task;
static volatile int s_req = -1;   // -1=空闲
static volatile bool s_started;

static void play_notes(const note_t *seq)
{
    int16_t *buf = malloc(CHUNK * sizeof(int16_t));
    if (!buf) return;

    for (const note_t *n = seq; n->ms > 0; n++) {
        if (n->hz <= 0) { vTaskDelay(pdMS_TO_TICKS(n->ms)); continue; }
        int total = SAMPLE_RATE * n->ms / 1000;
        int period = SAMPLE_RATE / n->hz;
        int phase = 0;
        while (total > 0) {
            int cnt = total < CHUNK ? total : CHUNK;
            for (int i = 0; i < cnt; i++) {
                int16_t v = SIN_T[phase];
                // 线性淡入淡出(首/尾各 8ms)避免爆音
                int t = i;
                if (t < 128) v = (int16_t)((int32_t)v * t / 128);
                if (total - cnt + i > SAMPLE_RATE * n->ms / 1000 - 128)
                    v = (int16_t)((int32_t)v * (SAMPLE_RATE * n->ms / 1000 - (total - cnt + i)) / 128);
                buf[i] = v;
                phase = (phase + 1) % period;
            }
            bsp_audio_write(buf, (size_t)cnt * sizeof(int16_t));
            total -= cnt;
        }
    }
    free(buf);
}

static void notify_task(void *arg)
{
    (void)arg;
    bsp_audio_set_format(SAMPLE_RATE, 16, 1);
    bsp_audio_set_volume(80);
    for (;;) {
        if (s_req >= 0) {
            int k = s_req;
            s_req = -1;
            switch (k) {
            case NOTIFY_FINISH: play_notes(S_FINISH); break;
            case NOTIFY_FAIL:   play_notes(S_FAIL);   break;
            case NOTIFY_PAUSE:  play_notes(S_PAUSE);  break;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
}

void app_notify_init(void)
{
    if (s_started) return;
    s_started = true;
    s_req = -1;
    // 栈需容纳 bsp_audio_set_format 内部日志打印,过小会爆栈
    xTaskCreate(notify_task, "app_notify", 8192, NULL, 3, &s_task);
}

void app_notify_trigger(notify_kind_t kind)
{
    app_notify_init();
    if (kind >= NOTIFY_FINISH && kind <= NOTIFY_PAUSE) s_req = (int)kind;
}
