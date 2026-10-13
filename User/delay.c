/**
  **************************************************************************
  * @file     delay.c
  * @brief    延时实现：SysTick 1ms 中断提供毫秒基准（同时作为 LVGL 心跳源），
  *           DWT 周期计数器提供微秒级忙等延时
  *
  * 实现说明：
  *   SysTick 配置为 HCLK 时钟源、1ms 重装载值并开启中断，每次中断调用
  *   delay_tick_isr()（内部再由 SysTick_Handler 调用 lv_tick_inc(1) 提供
  *   LVGL 心跳）。delay_ms 基于毫秒计数环回比较实现，不受单次上限约束。
  *   delay_us 使用 Cortex-M4 内核 DWT->CYCCNT 周期计数器忙等，不占用
  *   SysTick 资源，微秒级时序（如复位脉冲）由它保证。
  **************************************************************************
  */

#include "delay.h"
#include "lvgl.h"

/* 系统毫秒计数器（SysTick 中断中递增） */
static volatile uint32_t s_ms_count = 0;

/*********************************************************************
 * 函 数 名: delay_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 配置 SysTick 产生 1ms 中断, 并使能 DWT 周期计数器
 * 说    明: 依赖全局 system_core_clock 变量, 必须在系统时钟配置完成后调用;
 *           SysTick 中断优先级设为最低, 不抢占任何外设中断
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 重构为中断型毫秒延时 + DWT 微秒延时, 支撑 LVGL 心跳
 **********************************************************************/
void delay_init(void)
{
  /* ---- Step1: 使能 DWT 周期计数器(CYCCNT), 供 delay_us 使用 ---- */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /* ---- Step2: 配置 SysTick 为 HCLK 时钟源, 重装载 1ms, 开启中断 ---- */
  SysTick->LOAD = (system_core_clock / 1000U) - 1U;
  SysTick->VAL  = 0;                              /* 写任意值清零当前计数值 */
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk      /* 时钟源 = HCLK(240MHz) */
                | SysTick_CTRL_TICKINT_Msk       /* 使能 1ms 中断 */
                | SysTick_CTRL_ENABLE_Msk;       /* 使能 SysTick */

  /* ---- Step3: SysTick 异常优先级设为最低 ---- */
  SCB->SHP[11] = 0xFFU;
}

/*********************************************************************
 * 函 数 名: delay_tick_isr
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: SysTick 1ms 中断服务处理: 递增毫秒计数并给 LVGL 提供心跳
 * 说    明: 本函数在 SysTick_Handler 中断上下文中调用, 保持精简
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 新增函数
 **********************************************************************/
void delay_tick_isr(void)
{
  s_ms_count++;                                   /* 毫秒计数递增 */
  lv_tick_inc(1);                                 /* LVGL 心跳 +1ms */
}

/*********************************************************************
 * 函 数 名: delay_get_ms
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: 系统启动以来的毫秒计数
 * 函数功能: 获取当前毫秒时间戳
 * 说    明: 32 位计数 49.7 天回绕, 一般演示场景足够
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 新增函数
 **********************************************************************/
uint32_t delay_get_ms(void)
{
  return s_ms_count;
}

/*********************************************************************
 * 函 数 名: delay_us
 * 入口参数: nus     延时微秒数(0~约 1789s, 超长延时请用 delay_ms)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: DWT 周期计数器忙等延时, 精度 1 个系统时钟周期(约 4.2ns)
 * 说    明: CYCCNT 为 32 位无符号计数, 环回比较自动处理回绕
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 重构为 DWT 实现(原 SysTick 重装载方式与 1ms 中断冲突)
 **********************************************************************/
void delay_us(uint32_t nus)
{
  uint32_t start = DWT->CYCCNT;                   /* 记录起始周期数 */
  uint32_t cycles = nus * (system_core_clock / 1000000U);

  while((DWT->CYCCNT - start) < cycles)
  {
    /* 忙等: 差值无符号环回比较, CYCCNT 回绕时仍正确 */
  }
}

/*********************************************************************
 * 函 数 名: delay_ms
 * 入口参数: nms     延时毫秒数
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 基于 SysTick 毫秒计数的非阻塞轮询延时
 * 说    明: 差值环回比较, 计数回绕时依然正确; 延时期间中断照常响应
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 重构为中断计数实现(原查询 SysTick 方式与 LVGL 心跳冲突)
 **********************************************************************/
void delay_ms(uint32_t nms)
{
  uint32_t start = s_ms_count;

  while((s_ms_count - start) < nms)
  {
    /* 忙等: 等待毫秒计数推进 */
  }
}
