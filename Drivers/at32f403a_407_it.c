/**
  **************************************************************************
  * @file     at32f403a_407_it.c
  * @brief    中断服务函数（向量入口, 具体处理逻辑分发到各驱动层）
  *
  * 说明：
  *   1. SysTick/PendSV/SVC 由 FreeRTOS 接管：PendSV/SVC 经
  *      FreeRTOSConfig.h 宏重定向（本文件不重复定义），
  *      SysTick 在此调用 xPortSysTickHandler 供调度器计数心跳；
  *   2. TMR7_GLOBAL(编码器 1ms 采样中断) 分发到 encoder.c 处理；
  *      编码器三路引脚(PB3/PB4/PB5)不再使用 EXINT 中断。
  **************************************************************************
  */

#include "at32f403a_407_it.h"
#include "FreeRTOS.h"
#include "task.h"
#include "encoder.h"

/* FreeRTOS port.c 提供的 SysTick 处理函数(定义无对外原型, 此处前置声明) */
extern void xPortSysTickHandler(void);

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
  * @brief  调试监视中断服务函数
  */
void DebugMon_Handler(void)
{
}

/*********************************************************************
 * 函 数 名: SysTick_Handler
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: SysTick 1ms 心跳中断, 转交 FreeRTOS 维护调度节拍
 * 说    明: xPortSysTickHandler 内部自带中断屏蔽保护, 可直接在 ISR 调用
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 转交 FreeRTOS 调度器(LVGL 心跳经 LV_TICK_CUSTOM 直读 tick)
 **********************************************************************/
void SysTick_Handler(void)
{
  /* FreeRTOS 心跳计数 */
  xPortSysTickHandler();
}

/*********************************************************************
 * 函 数 名: TMR7_GLOBAL_IRQHandler
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: TMR7 全局中断(1ms 编码器采样节拍), 分发到编码器驱动
 * 说    明: 编码器解码方式为定时器同步采样, 不再使用 EXINT 中断
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 由 EXINT3/EXINT4/EXINT9_5 分发改为 TMR7 分发
 **********************************************************************/
void TMR7_GLOBAL_IRQHandler(void)
{
  encoder_tick_isr();
}
