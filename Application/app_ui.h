/**
  **************************************************************************
  * @file     app_ui.h
  * @brief    应用层交互演示 UI 接口（编码器联动：旋转调背光 / 点按翻转按钮）
  **************************************************************************
  */

#ifndef __APP_UI_H
#define __APP_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

void app_ui_init(void);                                          /* 创建演示界面(在 LVGL 上下文调用一次) */
void app_ui_encoder_event(int32_t delta, int32_t push_pressed, int32_t hold_ms);  /* 编码器事件处理(含长按关机进度, 仅 LVGL 任务调用) */
void app_ui_sgp30_update(uint32_t eco2, uint32_t tvoc, uint32_t valid);  /* SGP30 数据刷新(仅 LVGL 任务调用) */
void app_ui_env_update(int32_t temp_x10, int32_t hum_x10, int32_t press_x10,
                       int32_t altitude, int32_t aht_valid, int32_t bmp_valid);  /* 环境传感器(AHT20/BMP280)数据刷新, 仅 LVGL 任务调用 */

#ifdef __cplusplus
}
#endif

#endif /* __APP_UI_H */
