// Allocation-free telemetry adapter, shared by firmware and host regression.
#include "app_telemetry.h"
#include "spirit_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void copy_string(char *out, const char *in, size_t n)
{
    if (n) { size_t k = strlen(in); if (k >= n) k = n - 1; memcpy(out, in, k); out[k] = 0; }
}
#define strlcpy copy_string
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
            p = jskip_ws(p, end);
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
        p = jskip_ws(p, obj_end);
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
    return true;
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



bool app_telemetry_fresh(bool valid, uint64_t receipt_ms, uint64_t now_ms)
{
    return valid && now_ms >= receipt_ms && now_ms - receipt_ms <= 90000;
}

void app_telemetry_invalidate(bambu_state_t *st)
{
    st->nozzle_valid = st->nozzle_target_valid = st->bed_valid = false;
    st->bed_target_valid = st->chamber_valid = false;
    st->pct_valid = st->remain_valid = false;
    st->extruder.current_known = st->extruder.target_known = false;
    for (size_t i = 0; i < st->heads.count; i++) st->heads.heads[i].temperature_valid = false;
    for (int i = 0; i < BAMBU_AMS_MAX; i++) {
        st->ams[i].humidity_updated_ms = st->ams[i].humidity_raw_updated_ms = st->ams[i].temp_updated_ms = UINT64_MAX;
        for (int j = 0; j < BAMBU_TRAY_MAX; j++) {
            st->ams[i].tray[j].updated_ms = UINT64_MAX;
            st->ams[i].tray[j].pct_valid = false;
        }
    }
    st->external_tray.updated_ms = UINT64_MAX;
    st->external_tray.pct_valid = false;
}

// Packed protocol integers must be unsigned/integral, never atoi-clamped.
static bool ju32(const char *v, const char *ve, uint32_t *out)
{
    if (!v || !ve || v >= ve || ve - v > 24) return false;
    char buf[25]; size_t n = (size_t)(ve - v);
    memcpy(buf, v, n); buf[n] = 0;
    char *end; double number = strtod(buf, &end);
    if (end != buf + n || !isfinite(number) || number < 0 || number > UINT32_MAX || floor(number) != number) return false;
    *out = (uint32_t)number; return true;
}

static void thermal_field(const char *pr, int plen, const char *one, const char *two,
                          int *value, bool *valid, uint64_t *stamp, uint64_t now_ms)
{
    const char *ve; const char *v = jpath(pr, plen, one, &ve);
    if (!v && two) v = jpath(pr, plen, two, &ve);
    if (!v) return;
    uint32_t number;
    if (!ju32(v, ve, &number) || number > 1000) { *valid = false; return; }
    *value = (int)number; *valid = true; *stamp = now_ms;
}

static void merge_temperatures(bambu_state_t *st, const char *pr, int plen, uint64_t now_ms)
{
    thermal_field(pr, plen, "nozzle_temper", "nozzle_temperature", &st->nozzle_t, &st->nozzle_valid, &st->nozzle_ms, now_ms);
    thermal_field(pr, plen, "nozzle_target_temper", "nozzle_target_temperature", &st->nozzle_target, &st->nozzle_target_valid, &st->nozzle_target_ms, now_ms);
    thermal_field(pr, plen, "bed_temper", "bed_temperature", &st->bed_t, &st->bed_valid, &st->bed_ms, now_ms);
    thermal_field(pr, plen, "bed_target_temper", "bed_target_temperature", &st->bed_target, &st->bed_target_valid, &st->bed_target_ms, now_ms);
    thermal_field(pr, plen, "device.ctc.info.temp", "ctc_info.temp", &st->chamber_t, &st->chamber_valid, &st->chamber_ms, now_ms);
    thermal_field(pr, plen, "chamber_temper", "chamber_temperature", &st->chamber_t, &st->chamber_valid, &st->chamber_ms, now_ms);
    const char *ee, *ex = jpath(pr, plen, "device.extruder", &ee);
    if (!ex) return;
    int elen = (int)(ee - ex);
    const char *ve, *v = jpath(ex, elen, "state", &ve);
    uint32_t word;
    if (ju32(v, ve, &word)) st->extruder = nozzle_decode_state_bits(word);
    for (int i = 0; i < 32; i++) {
        char path[20]; snprintf(path, sizeof(path), "info[%d]", i);
        const char *he, *head = jpath(ex, elen, path, &he);
        if (!head) break;
        int hlen = (int)(he - head);
        v = jpath(head, hlen, "id", &ve);
        if (!ju32(v, ve, &word) || word >= NOZZLE_EXTRUDER_NONE) continue;
        nozzle_head_patch_t patch = {.id = word};
        v = jpath(head, hlen, "temp", &ve);
        patch.has_temp = ju32(v, ve, &patch.packed_temp);
        v = jpath(head, hlen, "hnow", &ve);
        patch.has_nozzle_id = ju32(v, ve, &word) && word <= UINT8_MAX;
        patch.nozzle_id = word;
        st->structured_heads = true;
        nozzle_head_upsert(&st->heads, &patch, now_ms);
    }
}

static void merge_tray(bambu_tray_t *slot, const char *tr, int len, uint64_t now_ms)
{
    const char *v, *ve; char color[12], name[32];
    bool changed = false;
    v = jpath(tr, len, "tray_type", &ve);
    if (v && jstr(v, ve, slot->tray_type, sizeof(slot->tray_type))) changed = true;
    v = jpath(tr, len, "tray_id_name", &ve);
    if (v && jstr(v, ve, name, sizeof(name))) { copy_utf8(name, slot->tray_name, sizeof(slot->tray_name)); changed = true; }
    v = jpath(tr, len, "tray_color", &ve);
    if (v && jstr(v, ve, color, sizeof(color))) {
        slot->tray_color = parse_color(color);
        slot->color_valid = strlen(color) >= 8 ? strtoul(color + 6, NULL, 16) != 0 : strlen(color) == 6;
        changed = true;
    }
    v = jpath(tr, len, "remain", &ve);
    if (v) {
        slot->tray_pct = jint(v, ve, -1);
        slot->pct_valid = slot->tray_pct >= 0 && slot->tray_pct <= 100;
        slot->pct_updated_ms = now_ms; changed = true;
    }
    v = jpath(tr, len, "tray_weight", &ve);
    if (v) { slot->tray_weight = jint(v, ve, 0); changed = true; }
    if (changed) { slot->reported = true; slot->updated_ms = now_ms; }
    slot->tray_remain = slot->pct_valid && slot->tray_weight > 0 ? (slot->tray_weight * slot->tray_pct + 50) / 100 : 0;
    slot->is_used = slot->tray_type[0] || slot->tray_name[0] || slot->color_valid || slot->tray_weight > 0 || slot->tray_pct > 0;
}

static void merge_materials(bambu_state_t *st, const char *pr, int plen, uint64_t now_ms)
{
    const char *ve, *v = jpath(pr, plen, "ams.ams_exist_bits", &ve);
    int bits = v ? jint(v, ve, -1) : -1;
    // Only an explicit membership bitmap removes a previously learned AMS.
    if (bits >= 0) {
        for (int i = 0; i < BAMBU_AMS_MAX; i++) {
            bambu_ams_t *a = &st->ams[i];
            if (a->present && !(bits & (1u << a->id))) memset(a, 0, sizeof(*a));
        }
        st->ams_updated_ms = now_ms;
    }
    for (int i = 0; i < 16; i++) {
        char path[24]; snprintf(path, sizeof(path), "ams.ams[%d]", i);
        const char *ae, *au = jpath(pr, plen, path, &ae);
        if (!au) { snprintf(path, sizeof(path), "ams[%d]", i); au = jpath(pr, plen, path, &ae); }
        if (!au) break;
        int alen = (int)(ae - au);
        v = jpath(au, alen, "id", &ve);
        int id = v ? jint(v, ve, -1) : -1;
        if (id < 0 || id > 15 || (bits >= 0 && !(bits & (1u << id)))) continue;
        // An id-only shell is not evidence of an attached AMS.
        const char *probe_end;
        if (!jpath(au, alen, "tray[0]", &probe_end) && !jpath(au, alen, "humidity", &probe_end) && !jpath(au, alen, "humidity_raw", &probe_end) && !jpath(au, alen, "temp", &probe_end)) continue;
        int slot = -1;
        for (int a = 0; a < BAMBU_AMS_MAX; a++) if (st->ams[a].present && st->ams[a].id == id) slot = a;
        if (slot < 0) for (int a = 0; a < BAMBU_AMS_MAX; a++) if (!st->ams[a].present) { slot = a; break; }
        if (slot < 0) continue;
        bambu_ams_t *a = &st->ams[slot];
        if (!a->present) {
            memset(a, 0, sizeof(*a)); a->humidity = a->humidity_raw = a->temp = -1;
            a->humidity_updated_ms = a->humidity_raw_updated_ms = a->temp_updated_ms = UINT64_MAX;
            for (int j = 0; j < BAMBU_TRAY_MAX; j++) { a->tray[j].tray_pct = -1; a->tray[j].updated_ms = UINT64_MAX; }
        }
        a->id = (uint8_t)id; a->present = true;
        v = jpath(au, alen, "humidity", &ve);
        if (v) { a->humidity = jint(v, ve, -1); a->humidity_updated_ms = now_ms; }
        v = jpath(au, alen, "humidity_raw", &ve);
        if (v) { a->humidity_raw = jint(v, ve, -1); a->humidity_raw_updated_ms = now_ms; }
        v = jpath(au, alen, "temp", &ve);
        if (v) { a->temp = jint(v, ve, -1); a->temp_updated_ms = now_ms; }
        for (int j = 0; j < BAMBU_TRAY_MAX; j++) {
            char tp[16]; snprintf(tp, sizeof(tp), "tray[%d]", j);
            const char *te, *tr = jpath(au, alen, tp, &te);
            if (!tr) break;
            int tlen = (int)(te - tr);
            v = jpath(tr, tlen, "id", &ve);
            int tid = v ? jint(v, ve, -1) : -1;
            if (tid >= 0 && tid < BAMBU_TRAY_MAX) merge_tray(&a->tray[tid], tr, tlen, now_ms);
        }
        st->ams_updated_ms = now_ms;
    }
    // Compact holes after explicit detach, retaining protocol IDs.
    int count = 0;
    for (int a = 0; a < BAMBU_AMS_MAX; a++) if (st->ams[a].present) st->ams[count++] = st->ams[a];
    for (int a = count; a < BAMBU_AMS_MAX; a++) memset(&st->ams[a], 0, sizeof(st->ams[a]));
    st->ams_count = count;
    v = jpath(pr, plen, "ams.tray_now", &ve);
    if (v) {
        int active = jint(v, ve, 255); st->ams_active = false;
        for (int a = 0; a < count; a++) for (int t = 0; t < BAMBU_TRAY_MAX; t++) {
            st->ams[a].tray[t].active = active == st->ams[a].id * BAMBU_TRAY_MAX + t;
            if (st->ams[a].tray[t].active) st->ams_active = true;
        }
    }
    if (!count) {
        const char *te, *tr = jpath(pr, plen, "vt_tray", &te);
        if (tr) {
            merge_tray(&st->external_tray, tr, (int)(te - tr), now_ms);
            st->external_tray.active = true;
            st->ams_active = false; st->ams_updated_ms = now_ms;
        }
        st->ams[0].tray[0] = st->external_tray;
    }
}


static bool complete_json(const char *data, int len)
{
    char stack[24]; unsigned depth = 0; bool quoted = false, escaped = false;
    bool closed = false;
    for (int i=0; i<len; i++) {
        char c=data[i];
        if (quoted) {
            if (escaped) escaped=false;
            else if (c=='\\') escaped=true;
            else if (c=='"') quoted=false;
            continue;
        }
        if (closed && c!=' ' && c!='\t' && c!='\r' && c!='\n') return false;
        if (c=='"') quoted=true;
        else if (c=='{' || c=='[') {
            if (depth==sizeof(stack)) return false;
            stack[depth++]=c;
        } else if (c=='}' || c==']') {
            if (!depth || stack[--depth] != (c=='}' ? '{' : '[')) return false;
            if (!depth) closed=true;
        }
    }
    return closed && !quoted && !depth;
}

// get_version replies carry the human-readable product name in the module list,
// e.g. "Bambu Lab X2D". Older synthetic fixtures used project_name; keep it as
// a fallback. Only printer-family product names are accepted, so AMS/accessory
// modules cannot overwrite the printer model.
static bool normalize_model_name(const char *name, char *out, int n)
{
    if (!name || !name[0]) return false;
    const char *p = name;
    if (!strncmp(p, "Bambu Lab ", 10)) p += 10;
    char norm[24];
    copy_string(norm, p, sizeof(norm));
    if (!strcmp(norm, "X1-Carbon")) copy_string(norm, "X1C", sizeof(norm));
    if (spirit_model_family(norm) != 2 || !strcmp(norm, "C11") || !strcmp(norm, "P1P")) {
        copy_string(out, norm, n);
        return true;
    }
    return false;
}

static bool merge_model_module(const char *data, int len, const char *base, char *out, int n)
{
    char path[48];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "%smodule[%d]", base, i);
        const char *end = NULL;
        const char *item = jpath(data, len, path, &end);
        if (!item || item >= end) break;
        int ilen = (int)(end - item);
        static const char *const keys[] = {"product_name", "project_name"};
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            const char *ve = NULL;
            const char *v = jpath(item, ilen, keys[k], &ve);
            char name[24];
            if (v && jstr(v, ve, name, sizeof(name)) && normalize_model_name(name, out, n)) return true;
        }
    }
    return false;
}

void app_telemetry_merge(bambu_state_t *st, const char *data, int len, uint64_t now_ms)
{
    if (!st || !data || len <= 0 || len > 18 * 1024 || !complete_json(data,len)) return;
    // Version replies are read-only metadata and contain no print object.
    // The standard reply is info.module; module/print.module are kept as
    // compatibility fallbacks seen in other clients' parsers.
    (void)(merge_model_module(data, len, "info.", st->model, sizeof(st->model)) ||
           merge_model_module(data, len, "", st->model, sizeof(st->model)) ||
           merge_model_module(data, len, "print.", st->model, sizeof(st->model)));
    const char *pe = NULL;
    const char *pr = jpath(data, len, "print", &pe);
    if (!pr || pr >= pe) return;
    int plen = (int)(pe - pr);
    const char *v, *ve;
    bool new_job = false;

    v = jpath(pr, plen, "gcode_state", &ve);
    if (v) {
        char gs[10];
        if (jstr(v, ve, gs, sizeof(gs))) {
            bool changed = strcmp(st->gcode_state, gs) != 0;
            bool starting = !strcmp(gs, "RUNNING") || !strcmp(gs, "PREPARE");
            bool was_active = !strcmp(st->gcode_state, "RUNNING") ||
                              !strcmp(st->gcode_state, "PREPARE") ||
                              !strcmp(st->gcode_state, "PAUSE");
            new_job = changed && starting && !was_active;
            if (new_job) {
                st->job_generation++;
                st->task_id[0] = 0;
                st->pct_valid = st->remain_valid = false;
            }
            strlcpy(st->gcode_state, gs, sizeof(st->gcode_state));
            st->state_received = true;
            st->state_updated_ms = now_ms;
            // A reason from an earlier pause must not classify a new state.
            // Explicit print_error below is applied after this transition.
            if (changed) st->print_error = 0;
            // 状态切换到 IDLE / FINISH / FAILED 且不再是打印中:
            // 清空残留的旧进度/层数/温度/任务,避免界面出现"空闲 + 53%"这种矛盾。
            if (changed && (strcmp(gs, "IDLE") == 0 || strcmp(gs, "FINISH") == 0 ||
                            strcmp(gs, "FAILED") == 0)) {
                st->pct = 0;
                st->remain_min = 0;
                st->pct_valid=st->remain_valid=false;
                st->layer = 0;
                st->total_layer = 0;
                st->job[0] = 0;
            }
        }
    }

    // 增量推送:报文里出现字段才更新,否则保留旧值,避免温度/进度被清零。
    v=jpath(pr,plen,"mc_percent",&ve);
    uint32_t number;
    if(v) { st->pct_valid=ju32(v,ve,&number) && number<=100; if(st->pct_valid) {st->pct=(int)number;st->pct_ms=now_ms;} }
    v=jpath(pr,plen,"print_error",&ve);
    if(ju32(v,ve,&number)) st->print_error=number;
    // Track task identity in the MQTT task even while another printer,
    // settings or the overview is visible. A later FINISH must not inherit
    // the acknowledgement of a different job that the UI never observed.
    v = jpath(pr, plen, "subtask_id", &ve);
    if (v && v < ve) {
        char id[32] = {0};
        if (*v == '"') {
            jstr(v, ve, id, sizeof(id));
        } else if (*v >= '0' && *v <= '9' && ve - v < (int)sizeof(id)) {
            size_t length = (size_t)(ve - v);
            memcpy(id, v, length);
            id[length] = 0;
        }
        if (id[0] && strcmp(id, "0") && strcmp(id, st->task_id)) {
            // A late first ID binds the current job; it is not a new job.
            if (st->task_id[0] && !new_job) st->job_generation++;
            strlcpy(st->task_id, id, sizeof(st->task_id));
        }
    }
    v=jpath(pr,plen,"mc_remaining_time",&ve);
    if(v) { st->remain_valid=ju32(v,ve,&number) && number<=1000000; if(st->remain_valid) {st->remain_min=(int)number;st->remain_ms=now_ms;} }
    JUPDATE_INT(pr, plen, "layer_num", st->layer);
    JUPDATE_INT(pr, plen, "total_layer_num", st->total_layer);
    merge_temperatures(st, pr, plen, now_ms);
    v = jpath(pr, plen, "printer_type", &ve);
    if (v) jstr(v, ve, st->model, sizeof(st->model));
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

    merge_materials(st, pr, plen, now_ms);

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

}
