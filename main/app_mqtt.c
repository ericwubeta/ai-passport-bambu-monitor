// main/app_mqtt.c —— 拓竹云(中国区)MQTT:订阅 device/<sn>/report,
// 每 30 秒发布 pushall 拉全量状态,轻量 JSON 扫描(游标式,内存 O(1))
// 解析进受互斥锁保护的快照。全量报文可达 11KB+,cJSON 建树会超 C3 堆。
//
// 连接参数(已实测可用):cn.mqtt.bambulab.com:8883 TLS,
// username = u_<USER_ID>,password = accessToken,client_id 随机 bbl_xxx。
#include "app_mqtt.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "app_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"

#include "secrets.h"

static const char *TAG = "app_mqtt";

#define BROKER_HOST "cn.mqtt.bambulab.com"
#define PUSH_PERIOD_US (30ULL * 1000000ULL)

static esp_mqtt_client_handle_t s_cli;
static SemaphoreHandle_t s_lock;
static bambu_state_t s_state[BAMBU_MACHINE_MAX];   // 每台机器独立状态槽
static int64_t s_conn_since;      // 云连接建立的时间戳(us)
static bool s_ever_data[BAMBU_MACHINE_MAX];

// 凭据:NVS(配网写入)优先,空则回落 secrets.h。app_mqtt_start 时解析一次。
static char s_uid[40];            // "u_<USER_ID>"
static char s_tok[260];           // accessToken
static char s_sn[BAMBU_MACHINE_MAX][34];   // 各打印机序列号
static char s_name[BAMBU_MACHINE_MAX][17]; // 自定义名称(可空)
static int  s_mcount;             // 有效机器数 1..4

static const char *sn_at(int i) { return s_sn[i]; }

// topic → 机器下标:device/<SN>/report 形态,匹配配置的 SN 列表
static int idx_from_topic(const char *topic, int tlen)
{
    const char *p = strstr(topic, "device/");
    if (!p) return -1;
    p += 7;
    const char *slash = memchr(p, '/', (size_t)(topic + tlen - p));
    int snlen = slash ? (int)(slash - p) : (int)(topic + tlen - p);
    for (int i = 0; i < s_mcount; i++) {
        if ((int)strlen(s_sn[i]) == snlen && !strncmp(s_sn[i], p, (size_t)snlen))
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------- 工具

static void state_reset_online_part(bambu_state_t *st)
{
    st->gcode_state[0] = 0;
    st->pct = 0;
    st->remain_min = 0;
    st->layer = st->total_layer = 0;
    st->nozzle_t = st->nozzle_target = 0;
    st->bed_t = st->bed_target = 0;
    st->chamber_t = 0;
    st->fan_cooling_pct = 0;
    st->spd_lvl = 0;
    st->wifi_signal = 0;
    st->job[0] = 0;
    st->printer_ip = 0;
    st->ams_count = 0;
    st->ams_active = false;
    memset(st->ams, 0, sizeof(st->ams));
    st->hms_count = 0;
    memset(st->hms, 0, sizeof(st->hms));
}

// 任务名保留 UTF-8(中文字体已嵌入):只剥掉控制字符,截断时保证
// 不会从多字节汉字中间切断(否则产生非法 UTF-8 序列导致乱码)。
static void sanitize_job(const char *in, char *out, int n)
{
    int w = 0;
    while (*in && w < n - 1) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20) { in++; continue; }                 // 剥控制字符
        int clen = 1;
        if (c >= 0xE0) clen = 3;                          // 3 字节中文
        else if (c >= 0xC0) clen = 2;                     // 2 字节
        if (w + clen >= n) break;                         // 放不下整字则截断
        for (int i = 0; i < clen && in[i]; i++) out[w++] = in[i];
        in += clen;
    }
    out[w] = 0;
    while (w > 0 && (unsigned char)out[w - 1] < 0x20) out[--w] = 0; // 尾随清理
    if (w == 0) snprintf(out, n, "等待任务");
}

static uint32_t parse_color(const char *hex)
{
    // 形如 "FF0000FF"(RGBA);只取前 6 位
    if (!hex || strlen(hex) < 6) return 0;
    char buf[7] = { hex[0], hex[1], hex[2], hex[3], hex[4], hex[5], 0 };
    return (uint32_t)strtoul(buf, NULL, 16);
}

// UTF-8 安全复制截断(不会从多字节汉字中间切断)
static void copy_utf8(const char *in, char *out, int n)
{
    int w = 0;
    while (*in && w < n - 1) {
        unsigned char c = (unsigned char)*in;
        int clen = (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (w + clen >= n) break;                // 放不下整字则截断
        for (int i = 0; i < clen && in[i]; i++) out[w++] = in[i];
        in += clen;
    }
    out[w] = 0;
}

// ---------------------------------------------------------------- 解析
//
// 说明:全量 report 报文(含 AMS/2D 等)可达 10~15KB。cJSON 会先把整棵树建进
// 堆,ESP32-C3(无 PSRAM)连接后仅剩 ~40KB 堆,解析必然内存不足失败。因此这里
// 改用游标式轻量扫描:内存 O(1),按点分路径直接定位字段取值,不受堆限制。

// 跳过空白
static const char *jskip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// 跳过 "..." 字符串(处理转义),返回结束引号之后
static const char *jskip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"') return p;
    p++;
    while (p < end) {
        if (*p == '\\') { p += (p + 1 < end) ? 2 : 1; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

// 跳过任意 JSON 值,返回值结束之后(指向 ',' / '}' / ']' / end)
static const char *jskip_value(const char *p, const char *end)
{
    if (p >= end) return p;
    switch (*p) {
    case '{': {
        p = jskip_ws(p + 1, end);
        if (p < end && *p == '}') return p + 1;
        for (;;) {
            p = jskip_ws(p, end);
            if (p >= end || *p != '"') return p;
            p = jskip_string(p, end);
            p = jskip_ws(p, end);
            if (p < end && *p == ':') p++;
            p = jskip_ws(p, end);
            p = jskip_value(p, end);
            p = jskip_ws(p, end);
            if (p >= end) return p;
            if (*p == ',') { p++; continue; }
            return (p < end && *p == '}') ? p + 1 : p;
        }
    }
    case '[': {
        p = jskip_ws(p + 1, end);
        if (p < end && *p == ']') return p + 1;
        for (;;) {
            p = jskip_value(p, end);
            p = jskip_ws(p, end);
            if (p >= end) return p;
            if (*p == ',') { p++; continue; }
            return (p < end && *p == ']') ? p + 1 : p;
        }
    }
    case '"':
        return jskip_string(p, end);
    default:
        while (p < end && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        return p;
    }
}

// 在对象(指向 '{')中按键定位值;*out_end 输出值结束之后。找不到返回 NULL。
static const char *jfind_key(const char *obj, const char *obj_end,
                             const char *key, int klen, const char **out_end)
{
    const char *p = jskip_ws(obj, obj_end);
    if (p >= obj_end || *p != '{') return NULL;
    p++;
    for (;;) {
        p = jskip_ws(p, obj_end);
        if (p >= obj_end || *p != '"') return NULL;
        const char *ks = p + 1;
        const char *ke = jskip_string(p, obj_end) - 1;   // 键结束引号
        size_t l = (size_t)(ke - ks);
        if (l == (size_t)klen && memcmp(ks, key, klen) == 0) {
            p = jskip_ws(ke + 1, obj_end);
            if (p < obj_end && *p == ':') p++;
            p = jskip_ws(p, obj_end);
            if (out_end) *out_end = jskip_value(p, obj_end);
            return p;
        }
        p = jskip_ws(ke + 1, obj_end);
        if (p < obj_end && *p == ':') p++;
        p = jskip_value(p, obj_end);
        p = jskip_ws(p, obj_end);
        if (p >= obj_end) return NULL;
        if (*p == ',') { p++; continue; }
        return NULL;
    }
}

// 按点分路径定位值,如 "print" / "mc_percent" / "ams.ams[0].tray[1].remain"。
// base 指向根对象 '{';*out_end 输出值结束之后;找不到返回 NULL。
static const char *jpath(const char *base, int len, const char *path, const char **out_end)
{
    const char *end = base + len;
    const char *obj = base;
    const char *tok = path;

    for (;;) {
        char key[40];
        int klen = 0;
        int idx = -1;
        while (*tok && *tok != '.' && *tok != '[' && klen < (int)sizeof(key) - 1)
            key[klen++] = *tok++;
        key[klen] = 0;
        if (*tok == '[') {
            idx = 0;
            for (tok++; *tok >= '0' && *tok <= '9'; tok++) idx = idx * 10 + (*tok - '0');
            if (*tok == ']') tok++;
        }

        const char *ve = NULL;
        const char *val = jfind_key(obj, end, key, klen, &ve);
        if (!val) return NULL;

        if (idx >= 0) {                          // 数组元素:必须先取第 idx 个元素,
            const char *p = jskip_ws(val, ve);   // 再判断路径是否结束(否则
            if (p >= ve || *p != '[') return NULL; // "ams.ams[0]" 会返回整个数组)
            p++;
            for (int i = 0; i < idx; i++) {
                p = jskip_ws(p, ve);
                if (p >= ve) return NULL;
                p = jskip_value(p, ve);
                p = jskip_ws(p, ve);
                if (p < ve && *p == ',') p++;
            }
            p = jskip_ws(p, ve);
            if (p >= ve || *p == ']') return NULL;
            val = p;
            ve = jskip_value(p, ve);
        }

        if (*tok == 0) { if (out_end) *out_end = ve; return val; }

        obj = jskip_ws(val, ve);                 // 嵌套对象/数组元素须为对象才能继续
        end = ve;
        if (obj >= end || *obj != '{') return NULL;
        if (*tok == '.') tok++;
    }
}

// 值区间转 int(兼容带引号的字符串数值,如 X2D/AMS2 的 "26.2" / "42")
static int jint(const char *v, const char *ve, int def)
{
    if (!v || v >= ve) return def;
    if (*v == '"') {                             // 剥掉字符串引号,否则 atoi 见到 '"' 返回 0
        v++;
        if (v < ve && ve[-1] == '"') ve--;
        if (v >= ve) return def;
    }
    char buf[24];
    int n = (int)(ve - v);
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    memcpy(buf, v, n);
    buf[n] = 0;
    return atoi(buf);
}

// 值区间若是字符串则复制内容(跳过转义)到 out,返回 true
static bool jstr(const char *v, const char *ve, char *out, int n)
{
    if (!v || v >= ve || *v != '"') return false;
    const char *p = v + 1;
    const char *e = ve - 1;                       // 指向结束引号
    int w = 0;
    while (p < e && w < n - 1) {
        if (*p == '\\' && p + 1 < e) {
            char nx = p[1];
            if (nx == 'u') { p += 6; continue; }              // 跳过 \uXXXX
            if (nx == '"' || nx == '\\' || nx == '/' || nx == 'b' ||
                nx == 'f' || nx == 'n' || nx == 'r' || nx == 't') { p += 2; continue; }
        }
        out[w++] = *p++;
    }
    out[w] = 0;
    return w > 0;
}

// 提取 int 字段:路径存在才更新(增量报文缺字段时保留旧值)
#define JUPDATE_INT(pr, plen, path, dst) do { \
        const char *_e = NULL; \
        const char *_v = jpath((pr), (plen), (path), &_e); \
        if (_v) (dst) = jint(_v, _e, (dst)); \
    } while (0)
// 同字段多个命名,任一匹配即更新
#define JUPDATE_INT2(pr, plen, p1, p2, dst) do { \
        const char *_e = NULL; \
        const char *_v = jpath((pr), (plen), (p1), &_e); \
        if (!_v) _v = jpath((pr), (plen), (p2), &_e); \
        if (_v) (dst) = jint(_v, _e, (dst)); \
    } while (0)
// 四字段名兼容(X2D 与老机型命名不同)
#define JUPDATE_INT4(pr, plen, p1, p2, p3, p4, dst) do { \
        const char *_e = NULL; \
        const char *_v = jpath((pr), (plen), (p1), &_e); \
        if (!_v) _v = jpath((pr), (plen), (p2), &_e); \
        if (!_v) _v = jpath((pr), (plen), (p3), &_e); \
        if (!_v) _v = jpath((pr), (plen), (p4), &_e); \
        if (_v) (dst) = jint(_v, _e, (dst)); \
    } while (0)

static void parse_report(int idx, const char *data, int len)
{
    bambu_state_t *const st = &s_state[idx];
    // X2D 全量快照(含 AMS/2D 信息)可达 10~15KB,buffer 已配 20KB。
    // 轻量扫描不建树,内存 O(1),不受堆限制。
    if (len > 18 * 1024) return;

    const char *pe = NULL;
    const char *pr = jpath(data, len, "print", &pe);
    if (!pr || pr >= pe) return;      // 无 print(增量空报文等)
    int plen = (int)(pe - pr);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        const char *v, *ve;

        v = jpath(pr, plen, "gcode_state", &ve);
        if (v) {
            char gs[10];
            if (jstr(v, ve, gs, sizeof(gs))) {
                bool changed = strcmp(st->gcode_state, gs) != 0;
                strlcpy(st->gcode_state, gs, sizeof(st->gcode_state));
                // 状态切换到 IDLE / FINISH / FAILED 且不再是打印中:
                // 清空残留的旧进度/层数/温度/任务,避免界面出现"空闲 + 53%"这种矛盾。
                if (changed && (strcmp(gs, "IDLE") == 0 || strcmp(gs, "FINISH") == 0 ||
                                strcmp(gs, "FAILED") == 0)) {
                    st->pct = 0;
                    st->remain_min = 0;
                    st->layer = 0;
                    st->total_layer = 0;
                    st->nozzle_t = 0;
                    st->nozzle_target = 0;
                    st->bed_t = 0;
                    st->bed_target = 0;
                    st->chamber_t = 0;
                    st->job[0] = 0;
                }
            }
        }

        // 增量推送:报文里出现字段才更新,否则保留旧值,避免温度/进度被清零。
        JUPDATE_INT(pr, plen, "mc_percent", st->pct);
        JUPDATE_INT(pr, plen, "mc_remaining_time", st->remain_min);
        JUPDATE_INT(pr, plen, "layer_num", st->layer);
        JUPDATE_INT(pr, plen, "total_layer_num", st->total_layer);
        // 温度字段不同型号有不同命名,两种都试
        JUPDATE_INT2(pr, plen, "nozzle_temper", "nozzle_temperature", st->nozzle_t);
        JUPDATE_INT2(pr, plen, "nozzle_target_temper", "nozzle_target_temperature", st->nozzle_target);
        JUPDATE_INT2(pr, plen, "bed_temper", "bed_temperature", st->bed_t);
        JUPDATE_INT2(pr, plen, "bed_target_temper", "bed_target_temperature", st->bed_target);
        // 腔温:X2D 实测字段为 print.device.ctc.info.temp(CTC=腔温控制,藏于 device 子对象,
        // print 一层枚举不到);老机型为 print 一层 chamber_temper,H2D 另有 ctc_info.temp
        JUPDATE_INT4(pr, plen, "device.ctc.info.temp", "ctc_info.temp",
                     "chamber_temper", "chamber_temperature", st->chamber_t);
        // 风扇:原始值 0~15,存成百分比;字段缺失时反算原值再更新,保证幂等
        {
            int fan_raw = st->fan_cooling_pct * 15 / 100;
            JUPDATE_INT(pr, plen, "cooling_fan_speed", fan_raw);
            st->fan_cooling_pct = fan_raw * 100 / 15;
        }
        JUPDATE_INT(pr, plen, "spd_lvl", st->spd_lvl);
        JUPDATE_INT(pr, plen, "wifi_signal", st->wifi_signal);

        v = jpath(pr, plen, "subtask_name", &ve);
        if (!v) v = jpath(pr, plen, "gcode_file", &ve);   // X2D 部分固件用 gcode_file
        if (v) {
            char jb[64];
            if (jstr(v, ve, jb, sizeof(jb))) sanitize_job(jb, st->job, sizeof(st->job));
        }

        v = jpath(pr, plen, "net.info[0].ip", &ve);
        if (v) st->printer_ip = jint(v, ve, 0);

        // ---------- AMS:只在报文携带 AMS 数据时刷新 ----------
        // 增量 push_status 报文没有 AMS 段;若无条件清零,AMS 页会在增量到达时
        // 闪没(全量 pushall 每 30 秒才来一次),故先探测再清。
        {
            const char *ae0 = NULL;
            bool has_ams = jpath(pr, plen, "ams.ams[0]", &ae0) != NULL;
            if (!has_ams) has_ams = jpath(pr, plen, "ams[0]", &ae0) != NULL;

            if (has_ams) {
            int tray_now = 255;
            v = jpath(pr, plen, "ams.tray_now", &ve);
            if (v) tray_now = jint(v, ve, 255);   // jint 已兼容字符串/数字两种值

            memset(st->ams, 0, sizeof(st->ams));
            st->ams_count = 0;
            st->ams_active = false;

            for (int i = 0; i < BAMBU_AMS_MAX; i++) {
                char path[24];
                snprintf(path, sizeof(path), "ams.ams[%d]", i);
                const char *ae = NULL;
                const char *au = jpath(pr, plen, path, &ae);
                if (!au || au >= ae) {            // 兼容 print.ams 直接是数组的固件
                    snprintf(path, sizeof(path), "ams[%d]", i);
                    ae = NULL;
                    au = jpath(pr, plen, path, &ae);
                }
                if (!au || au >= ae) break;
                int alen = (int)(ae - au);

                bambu_ams_t *a = &st->ams[i];
                v = jpath(au, alen, "humidity", &ve);     if (v) a->humidity = jint(v, ve, -1);
                v = jpath(au, alen, "humidity_raw", &ve); if (v) a->humidity_raw = jint(v, ve, 0);
                v = jpath(au, alen, "temp", &ve);         if (v) a->temp = jint(v, ve, 0);

                int used_slots = 0;
                for (int t = 0; t < BAMBU_TRAY_MAX; t++) {
                    char tp[16];
                    snprintf(tp, sizeof(tp), "tray[%d]", t);
                    const char *te = NULL;
                    const char *tr = jpath(au, alen, tp, &te);
                    if (!tr || tr >= te) break;
                    int tlen = (int)(te - tr);

                    bambu_tray_t *slot = &a->tray[t];
                    char type[8], color[12], name[32];
                    v = jpath(tr, tlen, "tray_type", &ve);
                    if (v && jstr(v, ve, type, sizeof(type)))
                        strlcpy(slot->tray_type, type, sizeof(slot->tray_type));
                    v = jpath(tr, tlen, "tray_color", &ve);
                    if (v && jstr(v, ve, color, sizeof(color))) {
                        slot->tray_color = parse_color(color);
                        // 8 位 RGBA(X2D/AMS2):alpha 非 00 才算有料色,否则
                        // 纯黑料 "000000FF" 会被当成"无色"误判成空槽
                        slot->color_valid = (strlen(color) >= 8)
                            ? ((strtoul(color + 6, NULL, 16) & 0xFF) != 0)
                            : (slot->tray_color != 0);
                    }
                    // remain = 剩余百分比(0~100),空槽为 -1;tray_weight = 整卷标称克数
                    int pct = -1, weight = 0;
                    v = jpath(tr, tlen, "remain", &ve);
                    if (v) pct = jint(v, ve, -1);
                    v = jpath(tr, tlen, "tray_weight", &ve);
                    if (v) weight = jint(v, ve, 0);
                    slot->tray_pct    = pct;
                    slot->tray_weight = weight;
                    // 真实余量(克)= 整卷克数 × 剩余百分比(四舍五入);
                    // 空槽(pct<0)或克数未知 → 0 克
                    slot->tray_remain = (pct > 0 && weight > 0)
                                        ? (weight * pct + 50) / 100 : 0;
                    // X2D/AMS2 无 tray_type,料名/槽名在 tray_id_name
                    v = jpath(tr, tlen, "tray_id_name", &ve);
                    if (v && jstr(v, ve, name, sizeof(name)))
                        copy_utf8(name, slot->tray_name, sizeof(slot->tray_name));
                    // 有料判定:看有无耗材信息(材质/颜色/料名/标称克数),**不能只看 remain**。
                    // 手动耗材或无 RFID 料卷 remain=-1 但材质颜色都在,仍算有料,
                    // 只是余量未知(UI 显示 "--"),不能当成空槽把材质信息丢掉。
                    // 真·空槽是连 tray_type/tray_color/tray_id_name 都没有的那种。
                    slot->is_used = slot->tray_type[0] != 0 || slot->color_valid ||
                                    slot->tray_name[0] != 0 || weight > 0 || pct > 0;
                    if (slot->is_used) used_slots++;
                }
                // 在位判定:有料槽数据或湿度/温度读数才计入(拓竹 ams.ams 数组常
                // 固定 4 个元素,未接的 AMS 是空壳,不能算在位,否则 UI 显示 4 台)
                a->present = (used_slots > 0) || (a->humidity > 0) || (a->temp > 0);
                if (a->present) st->ams_count = i + 1;
            }

                // tray_now 是全局槽号(AMS1=0..3,AMS2=4..7),据此标记活动槽
                if (tray_now >= 0 && tray_now < BAMBU_AMS_MAX * BAMBU_TRAY_MAX) {
                    int ai = tray_now / BAMBU_TRAY_MAX, ti = tray_now % BAMBU_TRAY_MAX;
                    if (ai < st->ams_count && st->ams[ai].present) {
                        st->ams[ai].tray[ti].active = true;
                        st->ams_active = true;
                    }
                }
            }   // if (has_ams)

            // 无 AMS → 退回外部料筒 vt_tray,塞进 ams[0] 并标记 present=false 便于 UI 区分
            if (st->ams_count == 0) {
                const char *vte = NULL;
                const char *vt = jpath(pr, plen, "vt_tray", &vte);
                if (vt && vt < vte) {
                    int vtlen = (int)(vte - vt);
                    bambu_ams_t *a = &st->ams[0];
                    a->present = false;                 // 表示"外部料筒"
                    a->humidity = -1;
                    char type[8], color[12];
                    v = jpath(vt, vtlen, "tray_type", &ve);
                    if (v && jstr(v, ve, type, sizeof(type)))
                        strlcpy(a->tray[0].tray_type, type, sizeof(a->tray[0].tray_type));
                    v = jpath(vt, vtlen, "tray_color", &ve);
                    if (v && jstr(v, ve, color, sizeof(color))) {
                        a->tray[0].tray_color = parse_color(color);
                        a->tray[0].color_valid = (a->tray[0].tray_color != 0);
                    }
                    // 同上:remain 是百分比,真实克数需乘 tray_weight
                    {
                        int pct = -1, weight = 0;
                        v = jpath(vt, vtlen, "remain", &ve);
                        if (v) pct = jint(v, ve, -1);
                        v = jpath(vt, vtlen, "tray_weight", &ve);
                        if (v) weight = jint(v, ve, 0);
                        a->tray[0].tray_pct    = pct;
                        a->tray[0].tray_weight = weight;
                        a->tray[0].tray_remain = (pct > 0 && weight > 0)
                                                 ? (weight * pct + 50) / 100 : 0;
                    }
                    a->tray[0].is_used = true;
                    a->tray[0].active = true;
                    st->ams_count = 1;
                    st->ams_active = false;
                }
            }
        }

        // ---------- HMS 错误码:数组元素取 code/ecode/hms_code ----------
        // 只在报文携带 hms 字段时刷新(增量报文无 hms 段,避免误清告警)
        {
            const char *he = NULL;
            const char *hms = jpath(pr, plen, "hms", &he);
            if (hms && hms < he) {
                st->hms_count = 0;
                const char *hp = jskip_ws(hms, he);
                bool is_arr = (hp < he && *hp == '[');
                for (int i = 0; i < BAMBU_HMS_MAX; i++) {
                    char path[40];
                    snprintf(path, sizeof(path), is_arr ? "hms[%d]" : "hms", i);
                    const char *ie = NULL;
                    const char *item = jpath(pr, plen, path, &ie);
                    if (!item || item >= ie) break;
                    int ilen = (int)(ie - item);

                    uint32_t code = 0;
                    const char *ce = NULL;
                    const char *c = jpath(item, ilen, "code", &ce);
                    if (!c) c = jpath(item, ilen, "ecode", &ce);
                    if (!c) c = jpath(item, ilen, "hms_code", &ce);
                    if (c && c < ce) {
                        if (*c == '"') {                 // hex 字符串
                            char hb[16];
                            if (jstr(c, ce, hb, sizeof(hb))) code = (uint32_t)strtoul(hb, NULL, 16);
                        } else {
                            code = (uint32_t)jint(c, ce, 0);
                        }
                    }
                    if (code) {
                        st->hms[st->hms_count].code = code;
                        st->hms_count++;
                    }
                }
            }
        }

        s_ever_data[idx] = true;
        xSemaphoreGive(s_lock);
    }

    // 节流打印提取摘要,便于确认解析与内存状态(INFO 级)
    {
        static int64_t s_last_summary;
        int64_t now = esp_timer_get_time();
        if (now - s_last_summary > 10 * 1000000) {
            s_last_summary = now;
            ESP_LOGI(TAG,
                     "m%d 摘要: state=%s pct=%d%% nz=%d/%d bed=%d/%d cham=%d ams=%d hms=%d "
                     "(A0 h=%d/%d u=%d) (A1 h=%d/%d u=%d) (A2 h=%d/%d u=%d) (A3 h=%d/%d u=%d) "
                     "job=%s heap=%d",
                     idx, st->gcode_state, st->pct,
                     st->nozzle_t, st->nozzle_target,
                     st->bed_t, st->bed_target,
                     st->chamber_t,
                     st->ams_count, st->hms_count,
                     st->ams[0].humidity, st->ams[0].humidity_raw, st->ams[0].tray[0].is_used,
                     st->ams[1].humidity, st->ams[1].humidity_raw, st->ams[1].tray[0].is_used,
                     st->ams[2].humidity, st->ams[2].humidity_raw, st->ams[2].tray[0].is_used,
                     st->ams[3].humidity, st->ams[3].humidity_raw, st->ams[3].tray[0].is_used,
                     st->job, (int)esp_get_free_heap_size());
        }
    }
}

// ---------------------------------------------------------------- 事件

static void push_pushall(int idx)
{
    if (!s_cli || idx < 0 || idx >= s_mcount) return;
    char topic[48], payload[40];
    snprintf(topic, sizeof(topic), "device/%s/request", sn_at(idx));
    snprintf(payload, sizeof(payload), "{\"pushing\":{\"command\":\"pushall\"}}");
    esp_mqtt_client_publish(s_cli, topic, payload, 0, 0, 0);
}

static void on_mqtt(void *handler_arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "拓竹云已连接(机器数=%d)", s_mcount);
        s_conn_since = esp_timer_get_time();
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < s_mcount; i++) s_state[i].cloud = BAMBU_CLOUD_ONLINE;
            xSemaphoreGive(s_lock);
        }
        // 每台机器各订阅 report topic,否则收不到推送
        for (int i = 0; i < s_mcount; i++) {
            char rt[48];
            snprintf(rt, sizeof(rt), "device/%s/report", sn_at(i));
            esp_mqtt_client_subscribe(s_cli, rt, 0);
            ESP_LOGI(TAG, "已订阅 %s", rt);
            push_pushall(i);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "拓竹云断开,自动重连");
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < s_mcount; i++) s_state[i].cloud = BAMBU_CLOUD_CONNECTING;
            xSemaphoreGive(s_lock);
        }
        break;
    case MQTT_EVENT_DATA: {
        // 诊断:大报文在 esp-mqtt 里可能分段回调(total_data_len>data_len)
        if (e->total_data_len != e->data_len)
            ESP_LOGW(TAG, ">>> MQTT 分段: data_len=%d total=%d off=%d",
                     e->data_len, e->total_data_len, e->current_data_offset);
        // 按 topic 里的 SN 路由到对应机器的解析槽
        int idx = idx_from_topic(e->topic, e->topic_len);
        if (idx >= 0) parse_report(idx, e->data, e->data_len);
        break;
    }
    default:
        break;
    }
}

static void push_timer_cb(void *arg)
{
    for (int i = 0; i < s_mcount; i++) push_pushall(i);
}

// ---------------------------------------------------------------- API

void app_mqtt_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < BAMBU_MACHINE_MAX; i++) {
        state_reset_online_part(&s_state[i]);
        s_state[i].cloud = BAMBU_CLOUD_OFF;
    }

    // 凭据解析:NVS 优先,空则回落 secrets.h(老机器兼容)。
    device_config_t cfgc;
    config_load(&cfgc);
    if (cfgc.bambu_uid[0]) {
        // NVS 存 "u_<id>" 或纯 id,统一成 "u_<id>"
        if (!strncmp(cfgc.bambu_uid, "u_", 2))
            strlcpy(s_uid, cfgc.bambu_uid, sizeof(s_uid));
        else
            snprintf(s_uid, sizeof(s_uid), "u_%s", cfgc.bambu_uid);
    } else {
        snprintf(s_uid, sizeof(s_uid), "u_%s", CFG_BAMBU_USER_ID);
    }
    if (cfgc.bambu_token[0]) strlcpy(s_tok, cfgc.bambu_token, sizeof(s_tok));
    else                     strlcpy(s_tok, CFG_BAMBU_TOKEN, sizeof(s_tok));

    // 打印机列表:SN/名称按槽位拷贝;NVS 全空则回落 secrets.h 单机。
    s_mcount = 0;
    for (int i = 0; i < BAMBU_MACHINE_MAX; i++) {
        strlcpy(s_sn[i], cfgc.bambu_sn[i], sizeof(s_sn[i]));
        strlcpy(s_name[i], cfgc.bambu_name[i], sizeof(s_name[i]));
        if (s_sn[i][0]) s_mcount = i + 1;
    }
    if (!s_mcount) {
        strlcpy(s_sn[0], CFG_BAMBU_SERIAL, sizeof(s_sn[0]));
        s_mcount = 1;
    }

    // ===== 多机仿真测试开关(发版前必须置 0)=====
    // >0 时为单机环境注入假机器:真机数据仍进槽 0,假机器显示"等待数据",
    // 用于单机环境验证多机概览 UI/按键导航/名称显示。设 1/2/3 看 2/3/4 台布局。
#ifndef TEST_EXTRA_MACHINES
#define TEST_EXTRA_MACHINES 0
#endif
#if TEST_EXTRA_MACHINES > 0
    for (int i = 0; i < TEST_EXTRA_MACHINES && s_mcount < BAMBU_MACHINE_MAX; i++) {
        snprintf(s_sn[s_mcount], sizeof(s_sn[0]), "TESTFAKE%07d", i + 1);
        snprintf(s_name[s_mcount], sizeof(s_name[0]), "测试%d号机", i + 2);
        s_mcount++;
    }
    ESP_LOGW(TAG, "多机仿真测试:注入 %d 台假机器,共 %d 台(发版前置 0)",
             TEST_EXTRA_MACHINES, s_mcount);
#endif

    bool configured = s_tok[0] && strncmp(s_tok, "paste", 5) != 0;
    if (!configured) {
        ESP_LOGW(TAG, "没有拓竹 token(查 NVS/secrets.h),跳过云连接。进'配网'填写账号。");
        return;
    }

    char cid[20];
    snprintf(cid, sizeof(cid), "bbl_%06X", (unsigned)(esp_timer_get_time() >> 10) & 0xFFFFFF);

    esp_mqtt_client_config_t cfg = { 0 };
    cfg.broker.address.hostname = BROKER_HOST;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    cfg.broker.address.port = 8883;
    cfg.broker.verification.crt_bundle_attach = &esp_crt_bundle_attach;
    cfg.credentials.username = s_uid;
    cfg.credentials.authentication.password = s_tok;
    cfg.credentials.client_id = cid;   // IDF 5.5: client_id 在 credentials 下
    cfg.buffer.size = 13 * 1024;       // 全量 AMS 报文约 11.4KB,留余量;越小越省堆
    cfg.buffer.out_size = 1024;
    cfg.session.keepalive = 60;

    s_cli = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_cli, MQTT_EVENT_ANY, on_mqtt, NULL);
    esp_mqtt_client_start(s_cli);

    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < s_mcount; i++) s_state[i].cloud = BAMBU_CLOUD_CONNECTING;
        xSemaphoreGive(s_lock);
    }

    const esp_timer_create_args_t t = { .name = "bambu_push", .callback = push_timer_cb };
    esp_timer_handle_t h;
    esp_timer_create(&t, &h);
    esp_timer_start_periodic(h, PUSH_PERIOD_US);
    ESP_LOGI(TAG, "MQTT 启动:%s user=%s 机器=%d 台", BROKER_HOST, s_uid, s_mcount);
}

int app_mqtt_machine_count(void)
{
    return s_mcount;
}

void app_mqtt_request_now(int idx)
{
    push_pushall(idx);
}

// 发布到 device/<sn>/request。拓竹 OpenBambuAPI 命令格式:
//   打印控制 {"print":{"sequence_id":"<n>","command":"pause|resume|stop","param":""}}(QoS 1)
//   腔灯     {"system":{"sequence_id":"<n>","command":"ledctrl","led_node":"chamber_light","led_mode":"on|off"}}
static void publish_request(int idx, const char *payload, int qos)
{
    if (!s_cli) { ESP_LOGW(TAG, "MQTT 未连接,命令丢弃"); return; }
    if (idx < 0 || idx >= s_mcount) { ESP_LOGW(TAG, "机器下标无效,命令丢弃"); return; }
    char topic[48];
    snprintf(topic, sizeof(topic), "device/%s/request", sn_at(idx));
    int id = esp_mqtt_client_publish(s_cli, topic, payload, 0, qos, 0);
    ESP_LOGI(TAG, "发布 %s -> %s(%s)", payload, topic, id < 0 ? "失败" : "成功");
}

void app_mqtt_print_command(int idx, const char *cmd)
{
    if (!cmd || (strcmp(cmd, "pause") && strcmp(cmd, "resume") && strcmp(cmd, "stop"))) return;
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"print\":{\"sequence_id\":\"%u\",\"command\":\"%s\",\"param\":\"\"}}",
             (unsigned)esp_random(), cmd);
    publish_request(idx, payload, 1);
}

void app_mqtt_set_chamber_light(int idx, bool on)
{
    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"system\":{\"sequence_id\":\"%u\",\"command\":\"ledctrl\","
             "\"led_node\":\"chamber_light\",\"led_mode\":\"%s\"}}",
             (unsigned)esp_random(), on ? "on" : "off");
    publish_request(idx, payload, 0);
}

void app_mqtt_snapshot(int idx, bambu_state_t *out)
{
    if (!out) return;
    if (!s_lock || idx < 0 || idx >= BAMBU_MACHINE_MAX) {
        memset(out, 0, sizeof(*out));
        out->cloud = BAMBU_CLOUD_OFF;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state[idx];
    xSemaphoreGive(s_lock);
}

int app_mqtt_uptime_sec(void)
{
    if (s_conn_since == 0) return 0;
    return (int)((esp_timer_get_time() - s_conn_since) / 1000000LL);
}

const char *app_mqtt_sn(int idx)
{
    return (idx >= 0 && idx < BAMBU_MACHINE_MAX) ? s_sn[idx] : "";
}

const char *app_mqtt_name(int idx)
{
    if (idx < 0 || idx >= BAMBU_MACHINE_MAX) return NULL;
    return s_name[idx][0] ? s_name[idx] : NULL;
}
