/**
  **************************************************************************
  * @file     encoder.h
  * @brief    旋转编码器驱动接口（PUSH=PA0 / CH_A=PB4 / CH_B=PB5）
  *
  * 硬件说明：
  *   1. PUSH 接 PB3——该脚同时复用为停机(DEEPSLEEP)模式的唤醒源
  *      EXINT3(按下=下降沿唤醒, 按键一脚接 GND, MCU 上拉), 无需改线;
  *      (若未来改用 STANDBY 待机, 唤醒脚固定为 WKUP=PA0, 需改线,
  *       见 lowpower.h 说明);
  *   2. PB3(JTDO)/PB4(NJTRST) 复位后默认为 JTAG 功能，驱动初始化时先
  *      使能 IOMUX 时钟再通过 SWJTAG_MUX_010 切换为"仅保留 SWD"释放
  *      这两个引脚；
  *   3. CH_A/CH_B 为推挽/开漏输出、外部上拉到 3.3V（空闲高电平），
  *      MCU 侧配置为上拉输入（与外部上拉并存不冲突）；
  *   4. 解码方式：TMR7 1ms 中断同步采样 A/B 电平，查表完成 4 倍频正交
  *      解码——相比 EXINT 双边沿中断，同步采样天然滤除机械触点抖动，
  *      手拧 EC11 类编码器不丢步、不乱跳；PUSH 采用连续 5 次采样一致
  *      的计数式消抖(约 5ms)，并维护连续按下时长(长按关机判定)。
  **************************************************************************
  */

#ifndef __ENCODER_H
#define __ENCODER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/* ---------------- PUSH 按键(PB3, 兼作停机唤醒 EXINT3) ----------------
 * 极性配置: 0=按下为低电平(按键一脚接 GND, MCU 上拉, 现行硬件接法,
 *           停机唤醒由 EXINT3 捕捉按下下降沿, "按下即唤醒");
 *           1=按下为高电平(按键一脚接 3.3V, MCU 下拉)
 *           ——仅当未来切回 STANDBY(WKUP=PA0 上升沿)时才需要改为 1 */
#define ENC_PUSH_ACTIVE_HIGH     0U

/* 长按关机判定阈值(ms): 连续按下达到该时长产生一次长按事件 */
#define ENC_PUSH_LONG_PRESS_MS  3000U

/* PUSH 引脚定义(PB3 = 停机唤醒脚 EXINT3, 按下产生下降沿) */
#define ENC_PUSH_PORT          GPIOB          /* PUSH 按键输入端口 */
#define ENC_PUSH_PIN           GPIO_PINS_3    /* PUSH 按键输入引脚(PB3) */
#define ENC_PUSH_CLK           CRM_GPIOB_PERIPH_CLOCK /* PUSH 端口时钟 */

/* ---------------- 旋转相位引脚定义(可修改以适配不同接线) ---------------- */
#define ENC_CH_A_PORT          GPIOB          /* CH_A 输入端口 */
#define ENC_CH_A_PIN           GPIO_PINS_4    /* CH_A 输入引脚(PB4) */
#define ENC_CH_A_CLK           CRM_GPIOB_PERIPH_CLOCK /* CH_A 端口时钟 */

#define ENC_CH_B_PORT          GPIOB          /* CH_B 输入端口 */
#define ENC_CH_B_PIN           GPIO_PINS_5    /* CH_B 输入引脚(PB5) */
#define ENC_CH_B_CLK           CRM_GPIOB_PERIPH_CLOCK /* CH_B 端口时钟 */

void encoder_init(void);                 /* 初始化引脚与 TMR7 采样中断 */
int32_t encoder_get_delta(void);         /* 读取并清零累计旋转增量(4倍频计步) */
uint8_t encoder_push_pressed(void);      /* 按键状态(消抖后): 1=按下 0=松开 */
uint8_t encoder_push_rising_edge(void);  /* 按键"松开->按下"边沿事件(读后清零): 1=有 */
uint32_t encoder_push_hold_ms(void);     /* 当前连续按下时长(ms), 松开清零 */
uint8_t encoder_push_long_press(void);   /* "长按达 ENC_PUSH_LONG_PRESS_MS"事件(读后清零): 1=有 */
void encoder_get_pin_levels(uint8_t *pin_a, uint8_t *pin_b, uint8_t *pin_push); /* 调试: 读取三路引脚实时电平(1=高) */
void encoder_tick_isr(void);             /* TMR7 1ms 采样中断处理(由向量表调用) */

#ifdef __cplusplus
}
#endif

#endif /* __ENCODER_H */
