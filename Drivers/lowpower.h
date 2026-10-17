/**
  **************************************************************************
  * @file     lowpower.h
  * @brief    低功耗管理接口（DEEPSLEEP 停机模式 + EXINT 按键唤醒）
  *
  * 模式选型说明：
  *   1. DEEPSLEEP（停机，本工程当前方案）：1.2V 域保持供电，SRAM/
  *      GPIO/外设寄存器全部保持，任意 EXINT 引脚可唤醒——PUSH 可
  *      保持在 PB3（EXINT3，按下=下降沿唤醒），**无需改线**；唤醒后
  *      从 WFI 处继续执行（非复位），仅需软件恢复 PLL 到 240MHz；
  *   2. STANDBY（待机，备用方案）：功耗更低（?A 级）但唤醒脚固定为
  *      WKUP=PA0（上升沿），PUSH 必须改线且按键一脚改接 3.3V 才能
  *      "按下唤醒"；唤醒等效复位。若未来切换，改用本模块保留的
  *      standby 系列 API 并调整 app_main 调用流程。
  *
  * 整机电源逻辑（DEEPSLEEP 版）：
  *   上电/复位 -> 默认停机(关机态, 熄屏) -> PUSH 按下(EXINT3 下降沿)
  *   -> 唤醒(恢复 240MHz) -> 开机运行 -> PUSH 长按 3s -> 熄屏停机
  *   -> 再按 PUSH 唤醒 -> 原界面原状态恢复显示(循环)。
  **************************************************************************
  */

#ifndef __LOWPOWER_H
#define __LOWPOWER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/* ---------------- 唤醒引脚定义(须与 encoder.h 的 ENC_PUSH_* 保持一致) ----------------
 * PB3 = EXINT3: PUSH 按键(低有效, 按下接地)产生下降沿唤醒停机模式 */
#define LOWPOWER_WAKE_PORT          GPIOB                  /* 唤醒按键输入端口 */
#define LOWPOWER_WAKE_PIN           GPIO_PINS_3            /* 唤醒按键输入引脚(PB3) */
#define LOWPOWER_WAKE_CLK           CRM_GPIOB_PERIPH_CLOCK /* 唤醒端口时钟 */
#define LOWPOWER_WAKE_PORT_SOURCE   GPIO_PORT_SOURCE_GPIOB /* EXINT 源端口 */
#define LOWPOWER_WAKE_PIN_SOURCE    GPIO_PINS_SOURCE3      /* EXINT 源引脚号 */

void    lowpower_init(void);              /* 使能 PWC 时钟(寄存器访问前置) */
void    lowpower_boot_wait(void);         /* 上电默认关机态: 停机等待按键唤醒, 返回=开机 */
void    lowpower_power_down(void);        /* 运行中关机: 进停机, 按键唤醒后恢复时钟并返回 */

/* ---- STANDBY 备用方案(未来改线 PA0 后启用), 当前未被应用层调用 ---- */
uint8_t lowpower_is_wakeup_reset(void);   /* 本次复位是否 WKUP 唤醒: 1=是 0=普通上电/复位 */
void    lowpower_clear_flags(void);       /* 清除 WKUP 唤醒/待机标志 */
void    lowpower_enter_standby(void);     /* 进入 STANDBY(不返回), PA0 上升沿唤醒复位 */

#ifdef __cplusplus
}
#endif

#endif /* __LOWPOWER_H */
