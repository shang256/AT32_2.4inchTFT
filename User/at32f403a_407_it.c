/**
  **************************************************************************
  * @file     at32f403a_407_it.c
  * @brief    中断服务函数（本演示工程未使用任何中断，保留默认空实现）
  *
  * 说明：
  *   延时模块采用 SysTick 查询方式，未开启 SysTick 中断；
  *   启动文件中的中断向量均为弱定义，此处按需覆盖即可。
  **************************************************************************
  */

#include "at32f403a_407_it.h"
#include "delay.h"

/**
  * @brief  不可屏蔽中断服务函数
  */
__NO_RETURN void NMI_Handler(void)
{
  while(1)
  {
  }
}

/**
  * @brief  硬件错误中断服务函数
  */
__NO_RETURN void HardFault_Handler(void)
{
  /* 发生硬件错误时停在此处，便于调试器定位 */
  while(1)
  {
  }
}

/**
  * @brief  存储管理错误中断服务函数
  */
__NO_RETURN void MemManage_Handler(void)
{
  while(1)
  {
  }
}

/**
  * @brief  总线错误中断服务函数
  */
__NO_RETURN void BusFault_Handler(void)
{
  while(1)
  {
  }
}

/**
  * @brief  用法错误中断服务函数
  */
__NO_RETURN void UsageFault_Handler(void)
{
  while(1)
  {
  }
}

/**
  * @brief  系统服务调用中断服务函数
  */
void SVC_Handler(void)
{
}

/**
  * @brief  调试监视中断服务函数
  */
void DebugMon_Handler(void)
{
}

/**
  * @brief  PendSV 中断服务函数
  */
void PendSV_Handler(void)
{
}

/*********************************************************************
 * 函 数 名: SysTick_Handler
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: SysTick 1ms 中断服务: 维护毫秒计数并向 LVGL 提供心跳
 * 说    明: 心跳逻辑封装在 delay_tick_isr() 中, 本函数保持精简
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 接入 LVGL 心跳(原工程未开启 SysTick 中断)
 **********************************************************************/
void SysTick_Handler(void)
{
  delay_tick_isr();                              /* 毫秒计数 + lv_tick_inc(1) */
}
