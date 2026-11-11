/**
  **************************************************************************
  * @file     lv_rtos_tick.h
  * @brief    LVGL 心跳桥接头文件：按 FreeRTOS 要求的顺序包含内核头
  *
  * 说明：
  *   FreeRTOS 要求 FreeRTOS.h 必须在 task.h 之前包含；LVGL 的
  *   LV_TICK_CUSTOM_INCLUDE 只接受一个头文件名，故用本文件中转。
  **************************************************************************
  */

#ifndef __LV_RTOS_TICK_H
#define __LV_RTOS_TICK_H

#include "FreeRTOS.h"
#include "task.h"

#endif /* __LV_RTOS_TICK_H */
