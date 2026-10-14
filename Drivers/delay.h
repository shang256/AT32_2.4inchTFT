/**
  **************************************************************************
  * @file     delay.h
  * @brief    延时接口（DWT 周期计数器忙等实现，不占用 SysTick）
  *
  * 说明：
  *   FreeRTOS 接管 SysTick 后，SysTick 归调度器所有；本模块改用
  *   Cortex-M4 内核 DWT->CYCCNT 周期计数器实现精确延时，仅供硬件
  *   初始化等裸机阶段使用。任务上下文中请使用 vTaskDelay。
  **************************************************************************
  */

#ifndef __DELAY_H
#define __DELAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

void delay_init(void);          /* 使能 DWT 周期计数器（时钟配置完成后调用一次） */
void delay_us(uint32_t nus);    /* 微秒级忙等延时 */
void delay_ms(uint32_t nms);    /* 毫秒级忙等延时（内部按 1ms 分段） */

#ifdef __cplusplus
}
#endif

#endif /* __DELAY_H */
