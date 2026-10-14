/**
  **************************************************************************
  * @file     lv_port_disp.h
  * @brief    LVGL v8 显示适配层接口（ST7789V 240x240, 硬件 SPI1）
  **************************************************************************
  */

#ifndef __LV_PORT_DISP_H
#define __LV_PORT_DISP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

void lv_port_disp_init(void);    /* 初始化显示适配层（需在 lv_init 之后调用） */

#ifdef __cplusplus
}
#endif

#endif /* __LV_PORT_DISP_H */
