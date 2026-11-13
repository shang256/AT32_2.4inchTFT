/**
  **************************************************************************
  * @file     app_ui.h
  * @brief    应用层交互演示 UI 接口（编码器联动：旋转调背光 / 点按切页面）
  **************************************************************************
  */

#ifndef __APP_UI_H
#define __APP_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* ---------------- 页面定义 ---------------- */
// UI 页面集合: 页面创建统一在 app_ui_page_create 完成,
// 加载/切换由状态机(app_state.c)的 on_enter 回调驱动
typedef enum
{
  APP_UI_PAGE_LOGO = 0,     /* 开机 LOGO 页(开机流程约 1.5s 自动跳过) */
  APP_UI_PAGE_DASH,         /* 骑行仪表盘页(默认运行页) */
  APP_UI_PAGE_COMPASS,      /* 指南针页(M 键循环切换) */
  APP_UI_PAGE_NUM
} app_ui_page_id_t;

/* ---------------- 页面切换动画方向 ---------------- */
// 页面加载动画方向: 状态机按跳转方向选择(仪表->指南针左滑, 反之右滑)
typedef enum
{
  APP_UI_ANIM_NONE = 0,     /* 无动画直接加载(LOGO/首次进入) */
  APP_UI_ANIM_LEFT,         /* 左滑过场(进入指南针页) */
  APP_UI_ANIM_RIGHT         /* 右滑过场(返回仪表盘页) */
} app_ui_page_anim_t;

void app_ui_page_create(void);                                  /* 创建全部页面控件(在 main 调度器启动前调用一次) */
void app_ui_page_show(app_ui_page_id_t page, app_ui_page_anim_t anim);  /* 加载指定页面(含切换动画, 仅状态机/LVGL 任务调用) */
void app_ui_encoder_rotate(int32_t delta);                      /* 编码器旋转事件: 调节背光亮度(仅 LVGL 任务调用) */
void app_ui_power_hint(int32_t hold_ms);                        /* 长按关机进度提示(>=800 显示提示, >=3000 显示关机, 0 隐藏, 仅 LVGL 任务调用) */
void app_ui_sgp30_update(uint32_t eco2, uint32_t tvoc, uint32_t valid);  /* SGP30 数据刷新(仅 LVGL 任务调用) */
void app_ui_env_update(int32_t temp_x10, int32_t hum_x10, int32_t press_x10,
                       int32_t altitude, int32_t aht_valid, int32_t bmp_valid);  /* 环境传感器(AHT20/BMP280)数据刷新, 仅 LVGL 任务调用 */
void app_ui_hr_update(int32_t hr_bpm, int32_t spo2, int32_t finger_on, int32_t valid);  /* MAX30102 心率血氧数据刷新, 仅 LVGL 任务调用 */
void app_ui_compass_update(int32_t heading_x10, int32_t raw_x, int32_t raw_y,
                           int32_t raw_z, int32_t valid);  /* QMC6309 指南针数据刷新(航向角/指针/方位名), 仅 LVGL 任务调用 */

#ifdef __cplusplus
}
#endif

#endif /* __APP_UI_H */
