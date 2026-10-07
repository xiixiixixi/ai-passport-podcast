#ifndef LV_CONF_H
#define LV_CONF_H
#ifndef __ASSEMBLER__
#include <stdio.h>
#include <stdlib.h>
/* A host regression must fail instead of spinning forever on an LVGL assert. */
#define LV_ASSERT_HANDLER do { fprintf(stderr, "LVGL assertion: %s:%d\n", __FILE__, __LINE__); abort(); } while(0);
#endif
#define LV_COLOR_DEPTH 16
#define LV_MEM_SIZE (24U * 1024U)
#define LV_FONT_FMT_TXT_LARGE 1
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_USE_LOG 0
#define LV_USE_OS LV_OS_NONE
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_USE_FONT_PLACEHOLDER 1
#endif
