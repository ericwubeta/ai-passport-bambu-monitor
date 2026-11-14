#pragma once
#define LV_CONF_H 1
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
#define LV_USE_OS LV_OS_NONE
// Host pointer/object sizes differ from ESP32-C3; this is not a RAM acceptance test.
#define LV_MEM_SIZE 131072
#define LV_CACHE_DEF_SIZE 0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_ASSERT_HANDLER abort();
#define LV_ASSERT_HANDLER_INCLUDE <stdlib.h>
