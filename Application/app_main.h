/**
  **************************************************************************
  * @file     app_main.h
  * @brief    应用层入口接口（FreeRTOS 任务框架）
  **************************************************************************
  */

#ifndef __APP_MAIN_H
#define __APP_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* 编码器事件队列项: 编码器任务 -> LVGL 任务 单向传递
   (统一使用 int32 字段, 避免混合宽度带来的结构体填充) */
typedef struct
{
  int32_t  delta;          /* 旋转增量(4 倍频计步, 0 表示本项仅为按键事件) */
  int32_t  push_pressed;   /* "松开->按下"沿事件: 1=发生了按下 */
  int32_t  hold_ms;        /* 当前连续按下时长(ms): >0 供 UI 显示关机进度, 0=取消提示 */
} app_encoder_msg_t;

/* 编码器事件队列(应用层共享, 由 app_main 创建) */
extern QueueHandle_t g_encoder_queue;

/* SGP30 传感器数据队列项: SGP30 任务 -> LVGL 任务 单向传递
   (统一使用 32 位字段, 避免混合宽度带来的结构体填充) */
typedef struct
{
  uint32_t  eco2;         /* eCO2 浓度(ppm) */
  uint32_t  tvoc;         /* TVOC 浓度(ppb) */
  uint32_t  valid;        /* 1=数据有效 0=读取失败(UI 显示 "--") */
} app_sensor_msg_t;

/* SGP30 传感器数据队列(应用层共享, 由 app_main 创建) */
extern QueueHandle_t g_sensor_queue;

#ifdef __cplusplus
}
#endif

#endif /* __APP_MAIN_H */
