/**
  **************************************************************************
  * @file     app_main.c
  * @brief    应用层入口：硬件初始化 + FreeRTOS 任务框架（入口 main）
  *
  * 分层说明（本工程三层架构）：
  *   Application（本层）  : 业务任务编排与 UI 逻辑
  *   Middleware（中间层） : FreeRTOS 配置 / LVGL 配置与显示适配
  *   Drivers（驱动层）    : LCD / 背光 PWM / 编码器 / 延时 / 中断分发
  *
  * 任务划分：
  *   1. encoder_task（优先级 3）: 20ms 轮询编码器驱动, 将旋转/按键事件
  *      投递到 FreeRTOS 队列 —— 不直接触碰 LVGL（保证线程安全）；
  *   2. lvgl_task（优先级 2）  : LVGL 刷图专用线程, 周期执行
  *      lv_timer_handler(), 并从队列取出编码器事件驱动 UI 联动。
  **************************************************************************
  */

#include "app_main.h"
#include "app_ui.h"
#include "delay.h"
#include "lcd_st7789.h"
#include "backlight.h"
#include "encoder.h"
#include "lvgl.h"
#include "lv_port_disp.h"

/* 全局: 编码器事件队列(驱动层事件 -> LVGL 线程) */
QueueHandle_t g_encoder_queue = NULL;

/* 任务栈大小(字: 4 字节) */
#define TASK_STACK_LVGL    (uint16_t)(1024)   /* LVGL 任务 4KB 栈 */
#define TASK_STACK_ENCODER (uint16_t)(256)    /* 编码器任务 1KB 栈 */

/*********************************************************************
 * 函 数 名: system_clock_config
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 配置系统时钟至 240MHz（HICK 8MHz 经二分频 4MHz 入 PLL, 60 倍频）
 * 说    明: apb2=120MHz(SPI1 时钟源, SPI 二分频后 60MHz);
 *           apb1=120MHz(TMR3 时钟源, 240 分频后 1MHz 计数);
 *           F403A 零等待区(ZW)Flash 架构下无需手动设置 Flash 等待周期
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 从原工程迁移, 逻辑未变
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

  /* ---- Step6: 更新全局系统时钟变量(delay/backlight 等驱动依赖此值) ---- */
  system_core_clock_update();
}

/*********************************************************************
 * 函 数 名: encoder_task
 * 入口参数: arg     任务参数(未使用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 编码器任务: 20ms 轮询驱动层, 将旋转增量与按键事件投递队列
 * 说    明: 本任务只与驱动层/队列交互, 不触碰任何 LVGL 接口,
 *           避免跨线程访问 UI 造成竞态
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(FreeRTOS 引入)
 **********************************************************************/
__NO_RETURN static void encoder_task(void *arg)
{
  app_encoder_msg_t msg;                          // 待投递的事件
  (void)arg;

  while(1)
  {
    /* ---- Step1: 读取驱动层累计增量与按键沿事件 ---- */
    msg.delta = encoder_get_delta();
    msg.push_pressed = encoder_push_rising_edge();

    /* ---- Step2: 有事件才投递, 空闲时休眠 20ms ---- */
    if((msg.delta != 0) || (msg.push_pressed != 0))
    {
      xQueueSend(g_encoder_queue, &msg, 0);      // 队列满则丢弃(旧事件会被新值覆盖, 无碍)
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

/*********************************************************************
 * 函 数 名: lvgl_task
 * 入口参数: arg     任务参数(未使用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: LVGL 刷图专用线程: 周期执行 lv_timer_handler 驱动
 *           刷新/动画/输入处理, 并转发编码器事件给 UI 层
 * 说    明: LVGL 非线程安全, 所有 UI 访问都收敛在本任务
 *           (编码器事件经队列从 encoder_task 送入)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(FreeRTOS 引入)
 **********************************************************************/
__NO_RETURN static void lvgl_task(void *arg)
{
  app_encoder_msg_t msg;                          // 从队列收到的事件
  (void)arg;

  while(1)
  {
    /* ---- Step1: 非阻塞收取编码器事件(可能一次积压多条, 全部处理) ---- */
    while(xQueueReceive(g_encoder_queue, &msg, 0) == pdTRUE)
    {
      app_ui_encoder_event(msg.delta, msg.push_pressed);
    }

    /* ---- Step2: 执行 LVGL 刷新(脏区绘制 + flush 到屏幕) ---- */
    lv_timer_handler();

    /* ---- Step3: 5ms 周期, 平衡响应速度与 CPU 占用 ---- */
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/*********************************************************************
 * 函 数 名: main
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: int(正常情况下不会返回)
 * 函数功能: 程序入口: 驱动层初始化 -> LVGL 初始化 -> 创建任务 -> 启动调度器
 * 说    明: 调度器启动后 main 上下文结束, 之后全部逻辑运行在任务中
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 由裸机大循环架构重构为 FreeRTOS 双任务架构
 **********************************************************************/
int main(void)
{
  /* ---- Step1: 系统时钟配置到 240MHz, 使能 DWT 延时基准 ---- */
  system_clock_config();
  delay_init();

  /* ---- Step2: 驱动层初始化: 屏幕 / 背光 PWM / 编码器 ---- */
  LCD_Init();
  backlight_init();                               /* 初始亮度 100% */
  encoder_init();

  /* ---- Step3: 中间层初始化: LVGL 内核与显示适配 ---- */
  lv_init();
  lv_port_disp_init();

  /* ---- Step4: 应用层初始化: 创建交互演示 UI(必须在 LVGL 任务内首次刷新前) ---- */
  app_ui_init();

  /* ---- Step5: 创建编码器事件队列与两个应用任务 ---- */
  g_encoder_queue = xQueueCreate(8, sizeof(app_encoder_msg_t));
  xTaskCreate(encoder_task, "ENC", TASK_STACK_ENCODER, NULL, 3, NULL);
  xTaskCreate(lvgl_task,    "LVGL", TASK_STACK_LVGL,    NULL, 2, NULL);

  /* ---- Step6: 启动 FreeRTOS 调度器(不再返回) ---- */
  vTaskStartScheduler();

  /* 调度器启动失败(堆不足等), 停在此处便于调试定位 */
  while(1)
  {
  }
}

/*********************************************************************
 * 函 数 名: vApplicationMallocFailedHook
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: FreeRTOS 堆内存分配失败钩子
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(FreeRTOS 引入)
 **********************************************************************/
__NO_RETURN void vApplicationMallocFailedHook(void)
{
  while(1)
  {
  }
}

/*********************************************************************
 * 函 数 名: vApplicationStackOverflowHook
 * 入口参数: xTask       发生溢出的任务句柄
 *           pcTaskName  任务名
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 任务栈溢出钩子(死循环挂起, 便于调试器查看任务名)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(FreeRTOS 引入)
 **********************************************************************/
__NO_RETURN void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  (void)pcTaskName;
  while(1)
  {
  }
}
