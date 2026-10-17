/**
  **************************************************************************
  * @file     backlight.h
  * @brief    LCD 背光 PWM 驱动接口（TMR3_CH4 @ PB1，占空比 20%~100%）
  *
  * 硬件说明：
  *   BLK(背光) 接 PB1，PB1 为 TMR3_CH4 默认映射（无需 remap）；
  *   PWM 频率 1kHz，占空比 20% 时最暗、100% 常亮，低于 20% 可能
  *   出现闪烁或升压电路不稳，故软件限幅下限 20%。
  **************************************************************************
  */

#ifndef __BACKLIGHT_H
#define __BACKLIGHT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

// 背光占空比允许范围(百分比)
#define BACKLIGHT_MIN_PERCENT   20
#define BACKLIGHT_MAX_PERCENT   100

void backlight_init(void);                       /* 初始化 TMR3_CH4 PWM(初始 100% 全亮) */
void backlight_set_percent(uint8_t percent);     /* 设置亮度 20~100, 超范围自动限幅 */
void backlight_off(void);                        /* 停止 PWM 并拉低 PB1 强制熄屏(进待机前调用) */
uint8_t backlight_get_percent(void);            /* 读取当前亮度百分比 */

#ifdef __cplusplus
}
#endif

#endif /* __BACKLIGHT_H */
