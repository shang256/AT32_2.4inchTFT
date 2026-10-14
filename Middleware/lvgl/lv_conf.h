/**
 ***********************************************************************
 * @file     lv_conf.h
 * @brief    LVGL v8.3 图形库配置文件（针对 AT32F403ACGU7 + ST7789V 240x240）
 *
 * 说明：
 *   1. 颜色深度 16bit RGB565，且开启字节序交换（LV_COLOR_16_SWAP=1），
 *      使显存内像素按"高字节在前"排列，SPI 发送时可按字节流直接顺序输出；
 *   2. 使用 LVGL 内置 TLSF 内存池（48KB），不依赖 libc 的 malloc；
 *   3. 开启性能监视器（LV_USE_PERF_MONITOR=1），屏幕右下角实时显示
 *      "FPS: 帧率  CPU: 占用率"，用于观察刷新帧率；
 *   4. LVGL 心跳通过 LV_TICK_CUSTOM 直接取自 FreeRTOS 的 RTOS tick，
 *      无需再调用 lv_tick_inc（FreeRTOS 接管 SysTick 后由调度器驱动）。
 ***********************************************************************
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* 抑制 LVGL 第三方头文件在 ARMCLANG(AC6) 下产生的非关键告警
   （-Wpadded 结构体填充 / -Wsign-conversion 符号隐式转换 /
     -Wextra-semi-stmt 多余分号 / -Wdisabled-macro-expansion 递归宏），
   这些均为 LVGL v8.3 源码风格问题, 不影响功能正确性 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wpadded"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wextra-semi-stmt"
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wcast-align"
#endif

#include <stdint.h>

/* ==== 颜色配置 ==== */

// 颜色深度: 16bit RGB565
#define LV_COLOR_DEPTH 16

// 16bit 颜色字节序交换: 1=交换（适配 SPI 高字节在前的传输顺序）
#define LV_COLOR_16_SWAP 1

#define LV_COLOR_SCREEN_TRANSP 0

/* ==== 内存配置 ==== */

// 使用 LVGL 内置内存池（TLSF），不依赖标准库 malloc
#define LV_MEM_CUSTOM 0

// 内存池大小: 32KB（主控共 96KB SRAM，需为 FreeRTOS 堆/绘制缓冲/任务栈预留；
// 交互演示 UI 对象少，32KB 足够，不足时可适当上调并压缩 FreeRTOS 堆）
#define LV_MEM_SIZE (32U * 1024U)

#define LV_MEM_ADR 0
#define LV_MEM_POOL 0

// memcpy/memset 直接使用标准库实现，速度更快
#define LV_MEMCPY_MEMSET_STD 1

/* ==== 心跳与刷新周期 ==== */

// 显示刷新周期: 16ms（决定 LVGL 定时刷新屏幕的最大频率, 即 FPS 上限 62）
// 注: 60MHz SPI 下全屏传输约 15.5ms, 周期小于单帧耗时会掉帧, 16ms 与之匹配
#define LV_DISP_DEF_REFR_PERIOD 16

// 输入设备读取周期: 30ms（本演示无输入设备，不生效）
#define LV_INDEV_DEF_READ_PERIOD 30

// 每英寸像素数（影响部分控件的默认尺寸计算）
#define LV_DPI_DEF 130

/* ==== 性能监视器 ==== */

// 开启性能监视器: 屏幕右下角实时显示 FPS（刷新帧率）与 CPU 占用率
#define LV_USE_PERF_MONITOR 1

// 性能监视器显示位置: 右下角
#define LV_USE_PERF_MONITOR_POS LV_ALIGN_BOTTOM_RIGHT

/* ==== 日志与断言（全部关闭，节省资源） ==== */

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_USER_DATA 1

/* ==== 字体配置 ==== */

// 默认字体: Montserrat 14
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// 启用的内置字体（未启用的字体源码不参与编译，节省空间）
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK 0
#define LV_FONT_UNSCII_8 0
#define LV_FONT_UNSCII_16 0

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_USE_FONT_LOADER 0

/* ==== 文本配置 ==== */

#define LV_TXT_ENC_UTF8 1
#define LV_TXT_BREAK_CHARS " ,.:;-"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/* ==== 控件启用配置（未启用的控件源码不参与编译） ==== */

#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 0
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SWITCH 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 1

/* ==== 扩展组件（extra） ==== */

// 布局: benchmark 汇总表格使用 flex 列布局, grid 不需要
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

// 默认主题: 启用深色主题（demo 背景更美观）
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1

// 其余主题关闭
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO 0

// 扩展库/扩展控件全部关闭
#define LV_USE_SNAPSHOT 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT 0
#define LV_USE_MSG 0
#define LV_USE_IME_PINYIN 0

// extra 控件全部关闭(v8.3 默认为 1, 必须显式关闭, 否则其头文件依赖检查报错)
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CALENDAR_HEADER_ARROW 0
#define LV_USE_CALENDAR_HEADER_DROPDOWN 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_METER 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

/* ==== 官方演示 Demo（当前使用 benchmark 性能基准测试） ==== */

// 官方性能基准测试: 循环运行典型绘制场景, 实时显示每个场景 FPS, 结束后给出汇总评分表
#define LV_USE_DEMO_BENCHMARK 1

// 其余官方演示保持关闭
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_MUSIC 0
#define LV_USE_DEMO_STRESS 0

/* ==== 文件系统（未使用，全部关闭） ==== */

#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0

/* ==== 绘制加速（SW 软件渲染，无 GPU） ==== */

#define LV_USE_GPU_ARM2D 0
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_SWM341_DMA2D 0
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL 0

/* ==== 图片缓存 ==== */

#define LV_IMG_CACHE_DEF_SIZE 1

/* ==== 杂项 ==== */

// 系统心跳直接取自 FreeRTOS tick(1ms/格), 无需任何地方再调用 lv_tick_inc
// (FreeRTOS 接管 SysTick, LVGL 通过 LV_TICK_CUSTOM 机制自动取 xTaskGetTickCount;
//  include 经 lv_rtos_tick.h 中转, 保证 FreeRTOS.h 先于 task.h 的包含顺序)
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "lv_rtos_tick.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (xTaskGetTickCount())

#endif /* LV_CONF_H */
