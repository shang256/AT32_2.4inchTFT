/**
  **************************************************************************
  * @file     delay.h
  * @brief    延时接口（SysTick 1ms 中断 + DWT 周期计数微秒延时）
  **************************************************************************
  */

#ifndef __DELAY_H
#define __DELAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

void delay_init(void);          /* 初始化延时基准（需在系统时钟配置完成后调用一次） */
void delay_us(uint32_t nus);    /* 微秒级延时（DWT 周期计数忙等） */
void delay_ms(uint32_t nms);    /* 毫秒级延时（SysTick 中断计数） */
uint32_t delay_get_ms(void);    /* 获取系统启动以来的毫秒计数 */
void delay_tick_isr(void);      /* SysTick 1ms 中断处理（由 SysTick_Handler 调用） */

#ifdef __cplusplus
}
#endif

#endif /* __DELAY_H */
