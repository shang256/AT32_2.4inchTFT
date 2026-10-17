/**
  **************************************************************************
  * @file     at32f403a_407_it.h
  * @brief    中断服务函数声明与向量分发接口
  **************************************************************************
  */

#ifndef __AT32F403A_407_IT_H
#define __AT32F403A_407_IT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
void TMR7_GLOBAL_IRQHandler(void);   /* 编码器 1ms 同步采样(TMR7) */
void EXINT3_IRQHandler(void);       /* 停机唤醒中断(PB3 PUSH 下降沿, 仅停机期使能) */

#ifdef __cplusplus
}
#endif

#endif /* __AT32F403A_407_IT_H */
