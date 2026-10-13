/**
  **************************************************************************
  * @file     main.c
  * @brief    LVGL v8.3 官方性能基准测试程序（AT32F403ACGU7 + ST7789V 240x240）
  *
  * 演示内容：
  *   运行 LVGL 官方 lv_demo_benchmark 演示，自动循环执行典型绘制场景
  *   （矩形/阴影/边框/文字/圆弧/图片混合/图片变换/模糊等），
  *   每个场景实时显示 FPS 与场景名，全部场景结束后给出汇总评分表格，
  *   可全面评估本平台（240MHz + 硬件 SPI 60MHz）的图形性能。
  *
  * 心跳来源：SysTick 1ms 中断调用 lv_tick_inc(1)（见 at32f403a_407_it.c），
  * 主循环周期调用 lv_timer_handler 驱动 LVGL 刷新与动画。
  **************************************************************************
  */

#include "at32f403a_407.h"
#include "delay.h"
#include "lcd_st7789.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_demos.h"

/*********************************************************************
 * 函 数 名: system_clock_config
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 配置系统时钟至 240MHz（HICK 8MHz 经二分频 4MHz 入 PLL, 60 倍频）
 * 说    明: 配置流程与官方 BSP 的 at32f403a_407_clock_config 示例一致:
 *           sclk  = HICK/2 x 60 = 240MHz
 *           apb2  = 240MHz/2    = 120MHz（SPI1 时钟源, 二分频后 60MHz）
 *           F403A 零等待区(ZW)Flash 架构下官方示例无需手动设置 Flash 等待周期
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 沿用自纯色刷屏演示工程, 未修改时钟逻辑
 **********************************************************************/
static void system_clock_config(void)
{
  /* ---- Step1: 复位时钟管理器(CRM), 恢复默认配置 ---- */
  crm_reset();

  /* ---- Step2: 使能内部高速时钟 HICK(8MHz, 无需外部晶振) ---- */
  crm_clock_source_enable(CRM_CLOCK_SOURCE_HICK, TRUE);
  while(crm_flag_get(CRM_HICK_STABLE_FLAG) != SET)
  {
  }

  /* ---- Step3: 配置 PLL: HICK/2=4MHz 入 PLL, 60 倍频输出 240MHz ---- */
  crm_pll_config(CRM_PLL_SOURCE_HICK, CRM_PLL_MULT_60, CRM_PLL_OUTPUT_RANGE_GT72MHZ);
  crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
  while(crm_flag_get(CRM_PLL_STABLE_FLAG) != SET)
  {
  }

  /* ---- Step4: 总线分频: AHB 不分频, APB1/APB2 二分频 ---- */
  crm_ahb_div_set(CRM_AHB_DIV_1);
  crm_apb1_div_set(CRM_APB1_DIV_2);
  crm_apb2_div_set(CRM_APB2_DIV_2);

  /* ---- Step5: 顺滑模式下切换 PLL 为系统时钟源 ---- */
  crm_auto_step_mode_enable(TRUE);
  crm_sysclk_switch(CRM_SCLK_PLL);
  while(crm_sysclk_switch_status_get() != CRM_SCLK_PLL)
  {
  }
  crm_auto_step_mode_enable(FALSE);

  /* ---- Step6: 更新全局系统时钟变量(delay 模块依赖此值) ---- */
  system_core_clock_update();
}

/*********************************************************************
 * 函 数 名: main
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: int(不会返回)
 * 函数功能: 程序入口: 时钟/屏幕/LVGL 初始化后启动官方 benchmark 演示
 * 说    明: benchmark 每个场景结束时显示该场景 FPS, 全部跑完后显示
 *           汇总评分表; 也可同时观察右下角 LVGL 性能监视器数值
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 由自定义动态演示界面改为运行官方 lv_demo_benchmark
 **********************************************************************/
int main(void)
{
  /* ---- Step1: 系统时钟配置到 240MHz, 并初始化延时/SysTick 心跳 ---- */
  system_clock_config();
  delay_init();

  /* ---- Step2: 初始化 ST7789V 屏幕(GPIO/SPI1/复位/寄存器序列)并开背光 ---- */
  LCD_Init();
  LCD_SetBacklight(1);

  /* ---- Step3: 初始化 LVGL 内核与显示适配层 ---- */
  lv_init();
  lv_port_disp_init();

  /* ---- Step4: 启动 LVGL 官方性能基准测试演示 ---- */
  lv_demo_benchmark();

  /* ---- Step5: LVGL 主循环: 定时处理刷新/动画/定时器 ---- */
  while(1)
  {
    lv_timer_handler();                         /* 处理到期任务(含屏幕刷新) */
    delay_ms(5);                                /* 5ms 轮询间隔, 让出 CPU */
  }
}
