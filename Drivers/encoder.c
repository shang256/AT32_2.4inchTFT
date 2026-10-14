/**
  **************************************************************************
  * @file     encoder.c
  * @brief    旋转编码器驱动实现（TMR7 1ms 同步采样 + 状态表四倍频解码）
  *
  * 实现说明：
  *   1. TMR7 配置为 1ms 更新中断（基础定时器，无引脚占用）。每次中断
  *      同步采样 CH_A/CH_B 电平，经 16 项查找表完成 4 倍频正交解码。
  *      同步采样方式下，采样间隔内的多次触点抖动只会留下"最终电平"，
  *      不会像 EXINT 双边沿中断那样把每个抖动边沿都计成一步——这是
  *      手拧 EC11 类机械编码器最可靠的解码方式（不丢步、不乱跳）；
  *   2. PUSH 按键采用计数消抖：连续 8 次采样（约 8ms）电平一致才更新
  *      稳定状态，沿事件由应用层轮询时检测；
  *   3. PB3/PB4 释放 JTAG 依赖 IOMUX 时钟（已在前置步骤使能），
  *      释放后 SWD 调试口保留不受影响。
  **************************************************************************
  */

/* 官方库 exint_init_type 结构存在自然填充, AC6 会提示 -Wpadded,
   属第三方库代码风格问题, 需在包含官方头文件之前屏蔽 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wpadded"
#endif

#include "encoder.h"
#include <stddef.h>

/* PUSH 消抖所需连续一致采样次数(1ms/次, 约 5ms) */
#define ENC_PUSH_STABLE_CNT   5U

/* ---- 正交解码查找表: index = (上次状态<<2)|本次状态, 值为增量 ----
   状态编码: bit1=CH_A 电平, bit0=CH_B 电平; 表值 +1/-1 为顺/逆时针 */
static const int8_t s_qdec_table[16] =
{
  0, -1,  1,  0,
  1,  0,  0, -1,
  -1,  0,  0,  1,
  0,  1, -1,  0
};

static volatile int32_t  s_delta;        // 累计旋转增量(中断写/任务读清零)
static volatile uint8_t  s_ab_state;     // 上一次 A/B 相状态(查表索引)
static volatile uint8_t  s_push_stable;  // 消抖后的稳定按键状态(1=按下)
static volatile uint8_t  s_push_candidate; // 消抖候选电平(连续一致判断)
static volatile uint8_t  s_push_cnt;    // 候选电平连续出现次数
static volatile uint8_t  s_pin_a;       // 调试用: 三路引脚最新电平
static volatile uint8_t  s_pin_b;
static volatile uint8_t  s_pin_push;

/*********************************************************************
 * 函 数 名: encoder_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 初始化编码器引脚与 TMR7 1ms 采样中断
 * 说    明: 1) IOMUX 时钟必须先于 SWJTAG 重映射使能, 否则写入无效,
 *              PB3/PB4 将保持 JTAG 功能导致编码器完全无响应;
 *           2) 需在 delay_init(使能 DWT)与系统时钟配置完成后调用;
 *           3) TMR7 挂 APB1(120MHz), 定时器时钟倍频至 240MHz,
 *              240 分频 = 1MHz 计数, 周期 1000 = 1ms 采样节拍
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 由 EXINT 双边沿中断方案重构为 TMR7 同步采样方案
 *               (解决 EXINT 对机械抖动敏感与链路环节多的问题)
 **********************************************************************/
void encoder_init(void)
{
  gpio_init_type gpio_init_struct;       // GPIO 配置结构
  uint8_t curr = 0;                      // 上电时的 A/B 相初始状态

  /* ---- Step1: 使能 IOMUX 与编码器端口时钟 ----
     关键: IOMUX(复用选择)外设有独立时钟, 不使能则 SWJTAG 重映射
     写入无效, PB3/PB4 将保持 JTAG 功能导致编码器完全无响应 */
  crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(ENC_CH_A_CLK, TRUE);
  crm_periph_clock_enable(ENC_CH_B_CLK, TRUE);
  crm_periph_clock_enable(ENC_PUSH_CLK, TRUE);
  crm_periph_clock_enable(CRM_TMR7_PERIPH_CLOCK, TRUE);

  /* ---- Step2: 切换 SWJTAG 为"仅 SWD"模式, 释放 PB3(JTDO)/PB4(NJTRST) ---- */
  gpio_pin_remap_config(SWJTAG_MUX_010, TRUE);

//  /* ---- Step2.1: 复位调试跟踪块, 释放 SWO(=PB3) 引脚 ----
//     关键修复: PB3 同时是 Cortex-M4 的 SWO 调试探针。调试器(如 Keil 的
//     Debug Trace 选项)开启过 Trace 时, TPIU 的 SWO 输出配置在系统复位
//     后仍残留(调试域寄存器不受复位影响), PB3 被持续驱动为高电平,
//     表现为"编码器 A 相读不到信号、电平恒定不变"。
//     处理: 先关再开 DEMCR 的 TRCENA 位, 使 TPIU/ITM/DWT 跟踪块掉电
//     复位、清除残留的 SWO 配置; 随后重启 DWT 周期计数器(delay 模块
//     依赖)保证延时功能不受影响。调试器 SWD 功能不受 TRCENA 控制,
//     此操作不影响在线调试。 */
//  CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;   /* 跟踪块掉电复位 */
//  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;    /* 重新上电(TPIU 恢复默认) */
//  DWT->CYCCNT = 0;                                    /* 重启 DWT 周期计数器 */
//  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /* ---- Step3: 三路引脚配置为上拉输入(外部上拉到 3.3V, 空闲高电平) ---- */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
  gpio_init_struct.gpio_pull = GPIO_PULL_UP;
  gpio_init_struct.gpio_pins = ENC_CH_A_PIN;
  gpio_init(ENC_CH_A_PORT, &gpio_init_struct);
  gpio_init_struct.gpio_pins = ENC_CH_B_PIN;
  gpio_init(ENC_CH_B_PORT, &gpio_init_struct);
  gpio_init_struct.gpio_pins = ENC_PUSH_PIN;
  gpio_init(ENC_PUSH_PORT, &gpio_init_struct);

  /* ---- Step4: 用当前 A/B 电平初始化解码状态, 避免第一跳误计 ---- */
  if(gpio_input_data_bit_read(ENC_CH_A_PORT, ENC_CH_A_PIN) != RESET){ curr |= 0x02; }
  if(gpio_input_data_bit_read(ENC_CH_B_PORT, ENC_CH_B_PIN) != RESET){ curr |= 0x01; }
  s_ab_state = curr;

  /* ---- Step5: 用当前 PUSH 真实电平初始化消抖状态(防止上电虚假按下) ---- */
  s_pin_push = (gpio_input_data_bit_read(ENC_PUSH_PORT, ENC_PUSH_PIN) != RESET) ? 1U : 0U;
  s_push_candidate = s_pin_push;
  s_push_cnt = ENC_PUSH_STABLE_CNT;              /* 已一致的初始化计数 */
  s_push_stable = (s_pin_push != 0U) ? 0U : 1U;  /* 高电平=松开(低有效) */

  /* ---- Step6: 配置 TMR7 为 1ms 周期更新中断(编码器采样节拍) ---- */
  tmr_base_init(TMR7, 1000U - 1U, 240U - 1U);   /* 240MHz/240=1MHz, 1000=1ms */
  tmr_clock_source_div_set(TMR7, TMR_CLOCK_DIV1);
  tmr_cnt_dir_set(TMR7, TMR_COUNT_UP);
  tmr_flag_clear(TMR7, TMR_OVF_FLAG);           /* 清除可能残留的更新标志 */
  tmr_interrupt_enable(TMR7, TMR_OVF_INT, TRUE);
  tmr_counter_enable(TMR7, TRUE);

  /* ---- Step7: 使能 TMR7 中断(优先级 6: 不调用任何 FreeRTOS API,
       无需受 MAX_SYSCALL 优先级限制; 数值大于 5 不抢占内核临界区) ---- */
  nvic_irq_enable(TMR7_GLOBAL_IRQn, 6, 0);
}

/*********************************************************************
 * 函 数 名: encoder_tick_isr
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 1ms 采样处理: A/B 相查表四倍频解码 + PUSH 计数消抖
 * 说    明: 由 TMR7_GLOBAL_IRQHandler 调用; 保持精简, 不调用任何
 *           FreeRTOS API(优先级高于 syscall 阈值的安全做法)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(EXINT 方案重构为同步采样)
 **********************************************************************/
void encoder_tick_isr(void)
{
  uint8_t curr = 0;

  /* ---- Step1: 清除更新标志 ---- */
  tmr_flag_clear(TMR7, TMR_OVF_FLAG);

  /* ---- Step2: 采样 A/B 电平并记录(调试接口用) ---- */
  s_pin_a = (gpio_input_data_bit_read(ENC_CH_A_PORT, ENC_CH_A_PIN) != RESET) ? 1U : 0U;
  s_pin_b = (gpio_input_data_bit_read(ENC_CH_B_PORT, ENC_CH_B_PIN) != RESET) ? 1U : 0U;

  /* ---- Step3: 查表四倍频正交解码(bit1=A, bit0=B) ---- */
  curr = (uint8_t)((s_pin_a << 1) | s_pin_b);
  s_delta += s_qdec_table[(s_ab_state << 2) | curr];
  s_ab_state = curr;

  /* ---- Step4: PUSH 采样与计数消抖(连续 8 次一致才更新稳定状态) ---- */
  s_pin_push = (gpio_input_data_bit_read(ENC_PUSH_PORT, ENC_PUSH_PIN) != RESET) ? 1U : 0U;
  if(s_pin_push != s_push_candidate)
  {
    s_push_candidate = s_pin_push;              /* 电平变化, 重新计数 */
    s_push_cnt = 1U;
  }
  else
  {
    if(s_push_cnt < ENC_PUSH_STABLE_CNT)
    {
      s_push_cnt++;
      if(s_push_cnt >= ENC_PUSH_STABLE_CNT)     /* 连续一致达到阈值, 采纳 */
      {
        s_push_stable = (s_pin_push != 0U) ? 0U : 1U; /* 高=松开, 低=按下 */
      }
    }
  }
}

/*********************************************************************
 * 函 数 名: encoder_get_delta
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: 自上次调用以来的累计旋转增量(+ 顺时针 / - 逆时针, 4 倍频计步)
 * 函数功能: 原子读取并清零累计增量
 * 说    明: 用 PRIMASK 短临界区保护"读-清零", 防止与中断写入交错
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(接口沿用自 EXINT 方案)
 **********************************************************************/
int32_t encoder_get_delta(void)
{
  int32_t delta;
  uint32_t primask = __get_PRIMASK();   // 保存当前中断状态

  /* ---- Step1: 关中断, 读取并清零累计值 ---- */
  __disable_irq();
  delta = s_delta;
  s_delta = 0;

  /* ---- Step2: 恢复中断状态 ---- */
  __set_PRIMASK(primask);

  return delta;
}

/*********************************************************************
 * 函 数 名: encoder_push_pressed
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: 1=按键按下(低电平), 0=松开
 * 函数功能: 读取计数消抖后的按键稳定状态
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 消抖方式由"EXINT 跳变+时间窗"改为"采样计数"
 **********************************************************************/
uint8_t encoder_push_pressed(void)
{
  return s_push_stable;
}

/*********************************************************************
 * 函 数 名: encoder_push_rising_edge
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: 1=检测到"松开->按下"边沿(读后自动清零), 0=无
 * 函数功能: 获取按键按下沿事件(供应用层做"单击"类交互)
 * 说    明: 长按期间只触发一次, 不会重复报沿
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(接口沿用自 EXINT 方案)
 **********************************************************************/
uint8_t encoder_push_rising_edge(void)
{
  static uint8_t s_last_pressed = 0;      // 上次轮询时的按键状态(沿检测)
  uint8_t pressed_now;

  /* ---- Step1: 获取消抖后的当前按键状态 ---- */
  pressed_now = encoder_push_pressed();

  /* ---- Step2: "松开->按下"沿判断: 本次按下且上次松开, 才返回事件 ---- */
  if((pressed_now != 0U) && (s_last_pressed == 0U))
  {
    s_last_pressed = pressed_now;
    return 1;
  }

  s_last_pressed = pressed_now;
  return 0;
}

/*********************************************************************
 * 函 数 名: encoder_get_pin_levels
 * 入口参数: pin_a       回写 CH_A 电平(1=高/0=低)
 *           pin_b       回写 CH_B 电平
 *           pin_push    回写 PUSH 电平(1=高/松开)
 * 出口参数: 上述三个出参
 * 返 回 值: void
 * 函数功能: 读取三路引脚实时电平(1ms 采样缓存), 用于界面自检显示
 * 说    明: 硬件排查利器: 上电后 A/B/P 应显示高电平(外部上拉 3.3V);
 *           旋转/按下时对应位应实时翻转——若始终不变说明信号未进 MCU
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(编码器问题排查)
 **********************************************************************/
void encoder_get_pin_levels(uint8_t *pin_a, uint8_t *pin_b, uint8_t *pin_push)
{
  if(pin_a != NULL){ *pin_a = s_pin_a; }
  if(pin_b != NULL){ *pin_b = s_pin_b; }
  if(pin_push != NULL){ *pin_push = s_pin_push; }
}
