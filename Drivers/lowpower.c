/**
  **************************************************************************
  * @file     lowpower.c
  * @brief    低功耗管理实现（DEEPSLEEP 停机模式 + EXINT3 按键唤醒）
  *
  * 实现说明：
  *   1. 停机(DEEPSLEEP)进入路径：vrsel=1(调压器低功耗, 更省电) +
  *      lpsel=0(选择停机而非待机) + SLEEPDEEP=1 + WFI；内核/外设时钟
  *      停止，但 1.2V 域保持供电——SRAM、GPIO 状态、外设寄存器、
  *      CRM 外设门控全部保持；
  *   2. 唤醒源：EXINT3(PB3, PUSH 按下下降沿)。唤醒后内核以 HICK
  *      8MHz 继续执行(EXINT3 ISR 先清标志)，随后软件重新使能 PLL
  *      切回 240MHz——**不可调用含 crm_reset() 的完整时钟配置**，
  *      否则会关闭所有外设时钟门控(TMR7/SPI1/LCD 等全部失效)；
  *   3. 唤醒后必须清除 SLEEPDEEP 位——防止调度器空闲路径的 WFI
  *      误入停机导致假死；
  *   4. STANDBY 系列为备用方案（PA0 WKUP 唤醒），实现保留。
  **************************************************************************
  */

/* 官方库 exint_init_type 结构存在自然填充, AC6 会提示 -Wpadded,
   属第三方库代码风格问题, 需在包含官方头文件之前屏蔽 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wpadded"
#endif

#include "lowpower.h"
#include "delay.h"

/* 唤醒有效性确认: PB3 须持续按下(低电平)达到该时长(ms), 否则视为毛刺重睡 */
#define LOWPOWER_WAKE_CONFIRM_MS   50U

/*********************************************************************
 * 函 数 名: lowpower_exint3_prepare
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 配置 PB3 为 EXINT3 下降沿中断, 作为停机唤醒源(幂等)
 * 说    明: 按键低有效(按下接地), 按下产生下降沿唤醒; 中断服务
 *           只负责唤醒与清标志(见 at32f403a_407_it.c), 业务由
 *           唤醒后的任务循环处理
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(停机模式需求)
 **********************************************************************/
static void lowpower_exint3_prepare(void)
{
  exint_init_type exint_struct;

  /* ---- Step1: 将 PB3 映射到 EXINT 线 3(源选择经 IOMUX, 需其时钟) ---- */
  crm_periph_clock_enable(LOWPOWER_WAKE_CLK, TRUE);
  gpio_exint_line_config(LOWPOWER_WAKE_PORT_SOURCE, LOWPOWER_WAKE_PIN_SOURCE);

  /* ---- Step2: EXINT3 下降沿触发(按下=接地=下降沿), 中断模式 ---- */
  exint_default_para_init(&exint_struct);
  exint_struct.line_enable  = TRUE;
  exint_struct.line_mode    = EXINT_LINE_INTERRUPT;
  exint_struct.line_polarity = EXINT_TRIGGER_FALLING_EDGE;
  exint_struct.line_select  = EXINT_LINE_3;
  exint_init(&exint_struct);

  /* ---- Step3: 清残留标志(外设级 + NVIC 级)并使能 NVIC ----
     NVIC pending 残留会令 WFI 立即返回(假唤醒), 而 nvic_irq_enable
     不清 pending, 必须显式清除 */
  exint_flag_clear(EXINT_LINE_3);
  NVIC_ClearPendingIRQ(EXINT3_IRQn);
  nvic_irq_enable(EXINT3_IRQn, 6, 0);
}

/*********************************************************************
 * 函 数 名: lowpower_exint3_teardown
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 唤醒后关闭 EXINT3 中断(运行期采样由 TMR7 负责, 无需 EXINT)
 * 说    明: 幂等, 每次停机唤醒后调用, 下次停机时重新 prepare
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(停机模式需求)
 **********************************************************************/
static void lowpower_exint3_teardown(void)
{
  exint_init_type exint_struct;

  /* ---- Step1: 清除唤醒标志 ---- */
  exint_flag_clear(EXINT_LINE_3);

  /* ---- Step2: 禁用 NVIC 与 EXINT3 线 ---- */
  nvic_irq_disable(EXINT3_IRQn);
  exint_default_para_init(&exint_struct);
  exint_struct.line_select = EXINT_LINE_3;
  exint_struct.line_enable = FALSE;
  exint_init(&exint_struct);
}

/*********************************************************************
 * 函 数 名: lowpower_resume_clock_240m
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 停机唤醒后恢复系统时钟到 240MHz
 * 说    明: 停机时硬件自动关闭 PLL, 唤醒后系统时钟回退 HICK(8MHz);
 *           此处重新使能 PLL 并切回 240MHz。仅操作时钟源与切换,
 *           **不做 crm_reset**(外设时钟门控在停机期间全部保持,
 *           reset 会导致 TMR7/SPI1/GPIO 时钟全部关闭)。
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(停机模式需求)
 **********************************************************************/
static void lowpower_resume_clock_240m(void)
{
  /* ---- Step1: 重新配置并使能 PLL(HICK/2=4MHz x 60 = 240MHz) ---- */
  crm_pll_config(CRM_PLL_SOURCE_HICK, CRM_PLL_MULT_60, CRM_PLL_OUTPUT_RANGE_GT72MHZ);
  crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
  while(crm_flag_get(CRM_PLL_STABLE_FLAG) != SET)
  {
  }

  /* ---- Step2: 顺滑模式下切回 PLL(总线分频寄存器停机保持, 无需重设) ---- */
  crm_auto_step_mode_enable(TRUE);
  crm_sysclk_switch(CRM_SCLK_PLL);
  while(crm_sysclk_switch_status_get() != CRM_SCLK_PLL)
  {
  }
  crm_auto_step_mode_enable(FALSE);

  /* ---- Step3: 更新全局时钟变量(delay/backlight 等依赖) ---- */
  system_core_clock_update();
}

/*********************************************************************
 * 函 数 名: lowpower_deepsleep_wait
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 进入停机模式并等待按键唤醒, 唤醒后恢复 240MHz 时钟返回
 * 说    明: 调用前必须已配置 EXINT3 唤醒源(lowpower_exint3_prepare);
 *           返回时系统已恢复 240MHz, 调用方只需恢复业务外设
 *           (如背光)并继续原流程
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(停机模式需求)
 **********************************************************************/
static void lowpower_deepsleep_wait(void)
{
  /* ---- Step1: 停机模式配置: 低功耗调压器 + 选择停机(非待机) ---- */
  PWC->ctrl_bit.vrsel = 1;                       /* 调压器低功耗模式(更省电) */
  PWC->ctrl_bit.lpsel = 0;                       /* 0=DEEPSLEEP, 1=STANDBY */

  /* ---- Step2: SLEEPDEEP=1, 清运行中断 pending 后 WFI 进入停机 ----
     TMR7(1ms)/SysTick(1ms) 若在 WFI 执行瞬间恰好 pending, 会令
     WFI 立即返回(假唤醒, 停机失败); 清 pending 后两者下一次中断
     还有约 1ms 间隔, 足以先完成停机(停机后时钟冻结) */
  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
  NVIC_ClearPendingIRQ(TMR7_GLOBAL_IRQn);
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;            /* 清 SysTick pending */
  __WFI();

  /* ---- Step3: 唤醒: 立即清 SLEEPDEEP(防空闲路径 WFI 误入停机) ---- */
  SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;

  /* ---- Step4: 恢复 240MHz 系统时钟(PLL 停机时被硬件关闭) ---- */
  lowpower_resume_clock_240m();
}

/*********************************************************************
 * 函 数 名: lowpower_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 使能 PWC 外设时钟(访问 PWC 寄存器的前置条件)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(整机低功耗需求)
 **********************************************************************/
void lowpower_init(void)
{
  /* ---- Step1: 使能 PWC(Power Control)外设时钟 ---- */
  crm_periph_clock_enable(CRM_PWC_PERIPH_CLOCK, TRUE);
}

/*********************************************************************
 * 函 数 名: lowpower_wake_key_held
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: uint8_t  1=唤醒后按键持续按住 0=无按键(毛刺假唤醒)
 * 函数功能: 唤醒有效性确认: 检测 PB3 是否持续低电平(真实按下)
 *           达到 LOWPOWER_WAKE_CONFIRM_MS 毫秒
 * 说    明: 假唤醒根因——长按关机松开后, 按键弹片"弹性后效"
 *           使触点接触电阻在数秒内缓慢爬升, PUSH 线仅有 MCU
 *           内部弱上拉(约 40k, 无外部上拉), PB3 电平缓慢滑落
 *           穿过阈值时产生"伪下降沿"唤醒停机中的芯片(现象:
 *           松手后两三秒屏幕自己亮起); 本函数用"持续低电平"
 *           区分真实按键(可靠接地, 期间恒为低)与回弹毛刺
 *           (电平滑落途中波动/回落为高), 毛刺判定为无效唤醒
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(假唤醒问题修复)
 **********************************************************************/
static uint8_t lowpower_wake_key_held(void)
{
  uint32_t i;

  /* ---- Step1: 逐毫秒采样, 期间出现高电平(松开)即判毛刺 ---- */
  for(i = 0U; i < LOWPOWER_WAKE_CONFIRM_MS; i++)
  {
    if(gpio_input_data_bit_read(LOWPOWER_WAKE_PORT, LOWPOWER_WAKE_PIN) != RESET)
    {
      return 0U;                                 /* 电平回到高: 非持续按下 */
    }
    delay_ms(1U);                                /* DWT 忙等(时钟已恢复) */
  }

  return 1U;                                     /* 持续低电平 50ms: 真实按键 */
}

/*********************************************************************
 * 函 数 名: lowpower_boot_wait
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 上电默认关机态: 完成最小唤醒配置后停机, 等待按键唤醒
 * 说    明: 在 main 早期调用(此时编码器/LCD 驱动尚未初始化),
 *           本函数自行完成唤醒所需的最小前置:
 *           IOMUX 时钟 + SWJTAG 释放 PB3 + PB3 上拉输入 + EXINT3;
 *           按键唤醒后经 50ms 持续低电平确认, 确认通过才返回,
 *           否则(毛刺假唤醒)重新停机; 按复位键/重新上电会
 *           再次进入本函数(默认关机态)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(停机模式需求, 替代 STANDBY 方案的唤醒标志判断);
 *               新增唤醒确认重睡闭环(假唤醒问题修复)
 **********************************************************************/
void lowpower_boot_wait(void)
{
  gpio_init_type gpio_init_struct;

  /* ---- Step1: 最小前置: IOMUX/GPIOB 时钟 + SWJTAG 释放 PB3(JTDO) ---- */
  crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
  gpio_pin_remap_config(SWJTAG_MUX_010, TRUE);   /* 仅保留 SWD, PB3/PB4 释放为 GPIO */

  /* ---- Step2: PB3 配置为上拉输入(PUSH 低有效: 按下接地, 空闲高) ---- */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
  gpio_init_struct.gpio_pull = GPIO_PULL_UP;
  gpio_init_struct.gpio_pins = LOWPOWER_WAKE_PIN;
  gpio_init(LOWPOWER_WAKE_PORT, &gpio_init_struct);

  /* ---- Step3: 停机等待唤醒; 毛刺假唤醒(非持续按下)则重新停机 ---- */
  do
  {
    lowpower_exint3_prepare();
    lowpower_deepsleep_wait();
  } while(lowpower_wake_key_held() == 0U);
  lowpower_exint3_teardown();
}

/*********************************************************************
 * 函 数 名: lowpower_power_down
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 运行中关机: 进入停机模式, 按键唤醒后恢复时钟并返回
 * 说    明: 供长按关机序列调用(调用前应已熄屏并等待按键松开,
 *           停机期间屏幕断背光但 GRAM 保持, 唤醒后界面原地恢复);
 *           按键弹片弹性回弹产生的"伪下降沿"会假唤醒芯片(现象:
 *           松手后数秒屏幕自己亮起)——唤醒后经 50ms 持续低电平
 *           确认, 毛刺则重新停机, 确认通过才返回;
 *           返回后调用方恢复背光/业务并继续任务循环
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(停机模式需求); 新增唤醒确认重睡闭环(假唤醒修复)
 **********************************************************************/
void lowpower_power_down(void)
{
  /* ---- Step1: 停机等待唤醒; 毛刺假唤醒(非持续按下)则重新停机 ---- */
  do
  {
    lowpower_exint3_prepare();                    /* EXINT3 唤醒源(PB3 已配好) */
    lowpower_deepsleep_wait();                    /* 返回时已恢复 240MHz */
  } while(lowpower_wake_key_held() == 0U);

  /* ---- Step2: 有效唤醒, 关闭 EXINT3(运行期采样由 TMR7 负责) ---- */
  lowpower_exint3_teardown();
}

/* ================= 以下为 STANDBY 备用方案(当前未启用) ================= */

/*********************************************************************
 * 函 数 名: lowpower_is_wakeup_reset
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: uint8_t  1=本次复位由 WKUP(PA0)唤醒  0=普通上电/复位
 * 函数功能: 查询 STANDBY 待机唤醒标志(仅 STANDBY 方案使用)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: STANDBY 方案保留实现
 **********************************************************************/
uint8_t lowpower_is_wakeup_reset(void)
{
  return (PWC->ctrlsts_bit.swef != 0U) ? 1U : 0U;
}

/*********************************************************************
 * 函 数 名: lowpower_clear_flags
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 清除 STANDBY 待机唤醒/待机标志(仅 STANDBY 方案使用)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: STANDBY 方案保留实现
 **********************************************************************/
void lowpower_clear_flags(void)
{
  PWC->ctrl_bit.clswef = 1;                      /* 写 1 清唤醒标志 swef */
  PWC->ctrl_bit.clsef = 1;                      /* 写 1 清待机标志 sef */
}

/*********************************************************************
 * 函 数 名: lowpower_enter_standby
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 进入 STANDBY 待机(不返回), PA0 WKUP 上升沿唤醒复位
 * 说    明: STANDBY 方案需 PUSH 改接 PA0 且按键一脚改接 3.3V
 *           (按下=高=上升沿唤醒), 唤醒后等效复位从 main 重新执行;
 *           启用步骤见 lowpower.h 文件头注释
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: STANDBY 方案保留实现
 **********************************************************************/
__NO_RETURN void lowpower_enter_standby(void)
{
  /* ---- Step1: 清除残留的唤醒/待机标志(防止 WFI 假唤醒) ---- */
  lowpower_clear_flags();

  /* ---- Step2: 确认 WKUP(PA0) 上升沿唤醒已使能 ---- */
  PWC->ctrlsts_bit.swpen = 1;

  /* ---- Step3: 选择 STANDBY 模式(LPSEL=1), 并置内核 SLEEPDEEP ---- */
  PWC->ctrl_bit.lpsel = 1;
  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;

  /* ---- Step4: WFI 进入待机(芯片掉电, 唤醒即复位) ---- */
  __WFI();

  /* ---- Step5: 理论兜底(正常唤醒即复位, 不会执行到此处) ---- */
  while(1)
  {
  }
}
