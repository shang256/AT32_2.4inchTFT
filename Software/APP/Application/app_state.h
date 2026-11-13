/**
  **************************************************************************
  * @file     app_state.h
  * @brief    应用层状态机接口（空状态/开机/LOGO/仪表盘/指南针 页面编排）
  *
  * 设计说明（状态表驱动 + 按键函数指针注册）：
  *   1. 状态机只做"页面编排 + 按键语义分发", 页面控件创建与数据刷新
  *      仍归属 app_ui 模块 —— 状态跳转时通过 on_enter 回调调用
  *      app_ui_page_show() 加载对应页面(含切换动画);
  *   2. 按键扫描完全留在驱动层(driver_encoder 的 TMR7 1ms 采样),
  *      本模块不解析按键电平, 只消费经 FreeRTOS 队列送入的语义事件,
  *      并按"当前状态注册的按键触发函数指针"分发 —— 底层扫描与
  *      应用层逻辑彻底分离;
  *   3. 状态流转: 空状态(停机) -> 按键唤醒开机 -> 开机 LOGO(约 1.5s,
  *      短按可跳过) -> 骑行仪表盘 <-> 指南针(短按 M 键固定顺序循环),
  *      长按 3s 关机回到空状态;
  *   4. 线程约束: 全部 API 仅允许 LVGL 任务调用(页面加载含 LVGL
  *      接口), 停机/唤醒的硬件动作仍由 encoder_task 执行。
  **************************************************************************
  */

#ifndef __APP_STATE_H
#define __APP_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ---------------- 状态定义 ---------------- */
// 应用状态机状态集合: 上电默认空状态(停机), 唤醒后按固定顺序流转
typedef enum
{
  APP_STATE_OFF = 0,        /* 空状态: 停机关机态(等待按键唤醒) */
  APP_STATE_POWER_ON,       /* 开机过渡态: 吞掉唤醒沿, 短暂延时后进 LOGO */
  APP_STATE_LOGO,           /* 开机 LOGO 态: 约 1.5s 自动跳转, 短按可跳过 */
  APP_STATE_DASH,           /* 骑行仪表盘态(默认运行页) */
  APP_STATE_COMPASS,        /* 指南针态(M 键循环切换页) */
  APP_STATE_NUM             /* 状态总数(数组边界) */
} app_state_id_t;

/* ---------------- 按键语义事件定义 ---------------- */
// 按键语义事件: 由 lvgl_task 将底层按键消息解析后送入状态机分发
typedef enum
{
  APP_KEY_SHORT_PRESS = 0,  /* 短按(按下沿): 运行页为 M 键切页, LOGO 页为跳过 */
  APP_KEY_LONG_PRESS,       /* 长按达 3s 阈值: 关机(encoder_task 执行停机序列) */
  APP_KEY_HOLD_PROGRESS     /* 长按保持中(hold_ms 携带进度, 0=松开取消) */
} app_state_key_event_t;

/* ---------------- 状态表结构(函数指针注册) ---------------- */
// 状态操作表: 每个状态注册进入/退出/按键触发三个回调函数指针,
// app_state.c 的状态表按状态下标注册, 新增页面只需扩展表项
typedef struct
{
  void (*on_enter)(void);                                          /* 状态进入: 加载对应 UI 页面(仅 LVGL 任务) */
  void (*on_exit)(void);                                           /* 状态退出: 清理/复位(仅 LVGL 任务) */
  void (*key_handler)(app_state_key_event_t evt, uint32_t hold_ms);/* 本状态按键触发函数指针(仅 LVGL 任务) */
} app_state_ops_t;

void app_state_init(void);                                               /* 初始化状态机并走首次开机流程(POWER_ON->LOGO) */
void app_state_key_event(app_state_key_event_t evt, uint32_t hold_ms);   /* 按键语义事件分发到当前状态注册的处理函数(仅 LVGL 任务) */
void app_state_wakeup(void);                                             /* 停机唤醒开机事件: 空状态 -> POWER_ON -> LOGO(仅 LVGL 任务) */
void app_state_tick(void);                                               /* 周期调度: 开机过渡延时/LOGO 到时自动跳转(仅 LVGL 任务) */
app_state_id_t app_state_get(void);                                      /* 查询当前状态(调试/联动用) */

#ifdef __cplusplus
}
#endif

#endif /* __APP_STATE_H */
