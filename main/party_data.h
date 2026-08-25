// main/party_data.h —— 酒桌游戏题库 + 抽签纯逻辑。
// 纯 C,不依赖 LVGL / ESP,便于在主机上用 cc 直接单测。
#pragma once

#include <string.h>
#include <stdio.h>

// 随机数:真机上用 esp_random(真随机),主机测试用 rand()
#ifdef ESP_PLATFORM
#include "esp_random.h"
static inline int party_rand(int n) { return n <= 1 ? 0 : (int)(esp_random() % (uint32_t)n); }
#else
#include <stdlib.h>
static inline int party_rand(int n) { return n <= 1 ? 0 : rand() % n; }
#endif

// ---------- 真心话(12) ----------
static const char *const TRUTH[] = {
    "最近一次说谎是什么时候",
    "你做过最丢脸的事是什么",
    "暗恋过在场的某个人吗",
    "手机里最不想被看到的照片",
    "做过最疯狂的一件事是什么",
    "你最害怕失去什么",
    "有没有偷偷喜欢过朋友的对象",
    "最不想让爸妈知道的事",
    "你最有罪恶感的一次花钱",
    "有没有对在场的人说过谎",
    "最想删掉的记忆是什么",
    "你现在最想见的人是谁",
};
#define TRUTH_N ((int)(sizeof(TRUTH) / sizeof(TRUTH[0])))

// ---------- 大冒险(12) ----------
static const char *const DARE[] = {
    "学一种动物叫三声",
    "给微信置顶发一句我爱你",
    "做十个俯卧撑",
    "学机器人说话一分钟",
    "用最肉麻的声音夸旁边的人",
    "闭眼单脚站三十秒",
    "发朋友圈我是全宇宙最帅",
    "模仿一个明星的签名",
    "用方言介绍你自己",
    "和左边的人十指相扣十秒",
    "表演一个表情让全场猜",
    "大声唱一句最土的歌",
};
#define DARE_N ((int)(sizeof(DARE) / sizeof(DARE[0])))

// ---------- 国王令(10,含 %d 编号占位) ----------
// slots: 0=无编号 1=一个编号 2=两个编号
typedef struct {
    const char *fmt;
    int slots;
} king_cmd_t;

static const king_cmd_t KING_CMDS[] = {
    { "国王点 %d 号喝一杯", 1 },
    { "国王让 %d 号和 %d 号干杯", 2 },
    { "全体轮流敬国王一杯", 0 },
    { "国王点 %d 号讲一个笑话", 1 },
    { "国王命 %d 号做五个深蹲", 1 },
    { "国王让 %d 号喝两杯", 1 },
    { "国王点两位玩家划拳定输赢", 0 },
    { "全体为国王干一杯", 0 },
    { "国王点 %d 号学动物叫", 1 },
    { "国王让 %d 号喂 %d 号喝酒", 2 },
};
#define KING_N ((int)(sizeof(KING_CMDS) / sizeof(KING_CMDS[0])))

// ---------- 命运签(16) ----------
static const char *const FATE[] = {
    "喝一杯",
    "免喝一次",
    "指定一个人喝",
    "连喝三杯",
    "全场干杯",
    "你喝半杯",
    "跳过这一轮",
    "左边的人喝",
    "右边的人喝",
    "自己喝两杯",
    "抽到酒神签 免喝",
    "喝一小口",
    "指定两个人干杯",
    "全场女生喝",
    "全场男生喝",
    "喝三小口",
};
#define FATE_N ((int)(sizeof(FATE) / sizeof(FATE[0])))

// ---------- 抽签逻辑 ----------
static inline const char *party_pick_truth(void) { return TRUTH[party_rand(TRUTH_N)]; }
static inline const char *party_pick_dare(void)  { return DARE[party_rand(DARE_N)]; }
static inline const char *party_pick_fate(void)  { return FATE[party_rand(FATE_N)]; }

// 生成国王令文本到 buf(cap 字节)。nplayers 为当前玩家人数(编号 1..nplayers)
static inline void party_king_text(char *buf, size_t cap, int nplayers) {
    const king_cmd_t *k = &KING_CMDS[party_rand(KING_N)];
    int a = 1 + party_rand(nplayers);
    int b = 1 + party_rand(nplayers);
    if (k->slots == 2)      snprintf(buf, cap, k->fmt, a, b);
    else if (k->slots == 1) snprintf(buf, cap, k->fmt, a);
    else                    snprintf(buf, cap, "%s", k->fmt);
}

// 俄罗斯转盘:返回 1 = 中弹,0 = 空枪(1/6 概率中弹)
static inline int party_roulette_shot(void) { return party_rand(6) == 0; }

// 随机抽一位酒神(1..nplayers)
static inline int party_pick_winner(int nplayers) { return 1 + party_rand(nplayers); }
