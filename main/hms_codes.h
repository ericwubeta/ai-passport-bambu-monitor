// main/hms_codes.h —— HMS 错误码 → 中文描述查询。
#pragma once

#include <stddef.h>
#include <stdint.h>

// 查询错误码对应中文描述。命中返回表内 const 串;未命中写入 buf
// "HMS 0xXXXXXXXX" 并返回 buf。buf 至少 48 字节。
const char *hms_lookup(uint32_t code, char *buf, size_t n);
