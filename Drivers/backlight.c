/**
  **************************************************************************
  * @file     backlight.c
  * @brief    LCD 背光 PWM 驱动实现（TMR3_CH4 @ PB1，1kHz，20%~100%）
  *
  * 实现说明：
  *   TMR3 挂在 APB1（120MHz），APB1 预分频为 2 时定时器时钟自动倍频
  *   至 240MHz；计数时钟取 240 分频 = 1MHz，周期 1000 计数 = 1kHz PWM，
  *   占空比分辨率 0.1%（比较值 = 亮度 x 10）。
  **************************************************************************
  */

#include "backlight.h"

/* PWM 周期计数值(1MHz 计数时钟下即 1000 -> 1kHz) */
#define BL_PWM_PERIOD          1000U

/* 当前亮度百分比(缓存, 供 backlight_get_percent 返回) */
static uint8_t s_percent = BACKLIGHT_MAX_PERCENT;

/*********************************************************************
 * 函 数 名: backlight_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 初始化 TMR3_CH4 于 PB1 输出 1kHz PWM, 初始亮度 100%
 * 说    明: PB1 为 TMR3_CH4 默认映射, 无需 gpio_pin_remap_config;
 *           需在系统时钟配置完成后调用
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(背光由固定 GPIO 改为 PWM 调光)
 **********************************************************************/
void backlight_init(void)
{
  gpio_init_type gpio_init_struct;               // GPIO 配置结构
  tmr_output_config_type oc_struct;              // 定时器输出通道配置结构

  /* ---- Step1: 使能 GPIOB 与 TMR3 外设时钟 ---- */
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_TMR3_PERIPH_CLOCK, TRUE);

  /* ---- Step2: PB1 配置为复用推挽输出(TMR3_CH4 默认映射) ---- */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_mode           = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull           = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init_struct.gpio_pins           = GPIO_PINS_1;
  gpio_init(GPIOB, &gpio_init_struct);

  /* ---- Step3: 定时器基础配置: 240MHz/240 = 1MHz 计数, 周期 1000 -> 1kHz ---- */
  tmr_base_init(TMR3, BL_PWM_PERIOD - 1U, 240U - 1U);
  tmr_clock_source_div_set(TMR3, TMR_CLOCK_DIV1);
  tmr_cnt_dir_set(TMR3, TMR_COUNT_UP);           /* 向上计数 */

  /* ---- Step4: CH4 输出通道配置: PWM 模式 A, 高电平有效 ---- */
  tmr_output_default_para_init(&oc_struct);
  oc_struct.oc_mode         = TMR_OUTPUT_CONTROL_PWM_MODE_A;
  oc_struct.oc_idle_state   = FALSE;
  oc_struct.occ_idle_state  = FALSE;
  oc_struct.oc_polarity     = TMR_OUTPUT_ACTIVE_HIGH;
  oc_struct.occ_polarity    = TMR_OUTPUT_ACTIVE_HIGH;
  oc_struct.oc_output_state = TRUE;              /* 使能 CH4 输出 */
  oc_struct.occ_output_state = FALSE;            /* 互补通道未用 */
  tmr_output_channel_config(TMR3, TMR_SELECT_CHANNEL_4, &oc_struct);
  tmr_output_channel_buffer_enable(TMR3, TMR_SELECT_CHANNEL_4, TRUE);

  /* ---- Step5: 初始亮度 100%(比较值满周期, 输出常高) ---- */
  tmr_channel_value_set(TMR3, TMR_SELECT_CHANNEL_4,
                        (uint32_t)s_percent * BL_PWM_PERIOD / 100U);

  /* ---- Step6: 启动定时器计数 ---- */
  tmr_counter_enable(TMR3, TRUE);
}

/*********************************************************************
 * 函 数 名: backlight_set_percent
 * 入口参数: percent     目标亮度百分比(自动限幅到 20~100)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 设置背光亮度占空比
 * 说    明: 比较值 = 亮度 x 10(PWM 周期 1000 计数), 写 C4 比较寄存器即时生效
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数
 **********************************************************************/
void backlight_set_percent(uint8_t percent)
{
  /* ---- Step1: 亮度限幅(低于 20% 可能闪烁, 高于 100% 无意义) ---- */
  if(percent < BACKLIGHT_MIN_PERCENT)
  {
    percent = BACKLIGHT_MIN_PERCENT;
  }
  if(percent > BACKLIGHT_MAX_PERCENT)
  {
    percent = BACKLIGHT_MAX_PERCENT;
  }

  /* ---- Step2: 换算为比较值并写入 C4 通道 ---- */
  s_percent = percent;
  tmr_channel_value_set(TMR3, TMR_SELECT_CHANNEL_4,
                        (uint32_t)percent * BL_PWM_PERIOD / 100U);
}

/*********************************************************************
 * 函 数 名: backlight_get_percent
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: 当前亮度百分比(20~100)
 * 函数功能: 读取缓存的当前亮度设置值
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数
 **********************************************************************/
uint8_t backlight_get_percent(void)
{
  return s_percent;
}
