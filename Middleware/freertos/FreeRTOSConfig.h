/**
  **************************************************************************
  * @file     FreeRTOSConfig.h
  * @brief    FreeRTOS 内核配置（AT32F403ACGU7 @ 240MHz, Cortex-M4F）
  *
  * 说明：
  *   1. SysTick/PendSV/SVC 由 FreeRTOS 接管（PendSV/SVC 通过本文件宏
  *      重定向到 port.c 的 xPortPendSVHandler/xPortSVCHandler）；
  *   2. LVGL 心跳直接使用 RTOS tick（见 lv_conf.h 的 LV_TICK_CUSTOM 配置）；
  *   3. 内存管理使用 heap_4（FreeRTOS\Source\portable\MemMang\heap_4.c）。
  **************************************************************************
  */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ==== 基本参数 ==== */

// 系统主频: 240MHz（与 main.c 中 system_clock_config 一致）
#define configCPU_CLOCK_HZ               (240000000UL)

// 心跳节拍: 1kHz, 1 tick = 1ms
#define configTICK_RATE_HZ               ((TickType_t)1000)

// 32 位 tick 计数（不使用 16 位）
#define configUSE_16_BIT_TICKS           0

// 任务最低栈大小(字), 空闲任务使用
#define configMINIMAL_STACK_SIZE         ((uint16_t)128)

// 任务名最大长度(含结束符)
#define configMAX_TASK_NAME_LEN          (16)

// 优先级数量(0~7, 空闲任务固定占用 0)
#define configMAX_PRIORITIES             (8)

// 时间片调度(同优先级任务轮转)
#define configUSE_TIME_SLICING           1

// 抢占式调度
#define configUSE_PREEMPTION              1

// 空闲任务让出(空闲任务不占用时间片)
#define configIDLE_SHOULD_YIELD          1

// 空闲任务栈溢出时让出: 关闭
#define configIDLE_SHOULD_YIELD_2        0 /* 未使用的占位 */

/* ==== 内存管理 ==== */

// heap_4 堆大小: 20KB(任务栈/TCB/队列均从此分配)
#define configTOTAL_HEAP_SIZE           ((size_t)(20 * 1024))

// 堆不足时返回 NULL(由应用检查)
#define configAPPLICATION_ALLOCATED_HEAP 0

// 使用 heap_4 时需要 malloc/free 钩子的支持定义
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION  0

/* ==== 中断优先级 ==== */

// AT32 使用 4 位中断优先级(16 级)
#define configPRIO_BITS                  4

// 最低中断优先级 = 15(数值越大优先级越低)
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY        15

// 可调用 FreeRTOS API 的最高中断优先级(5, 即优先级 5~15 的中断才可调用 API)
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY   5

// 内核中断优先级(SysTick/PendSV = 最低)
#define configKERNEL_INTERRUPT_PRIORITY         (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << 4)

// API 屏蔽中断优先级阈值
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << 4)

// 端口优化任务选择指令(CLZ 加速)
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1

/* ==== 调试与安全钩子 ==== */

// 栈溢出检测: 方式2(精确检测, 开销略高)
#define configCHECK_FOR_STACK_OVERFLOW          2

// 内存分配失败钩子使能
#define configUSE_MALLOC_FAILED_HOOK            1

// 空闲任务钩子/心跳钩子不使用
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0

/* ==== 功能裁剪 ==== */

// 不使用 FreeRTOS 软件定时器(LVGL 自带定时器机制)
#define configUSE_TIMERS                        0

// 不使用协程
#define configUSE_CO_ROUTINE                    0

// 队列注册表(调试用)
#define configQUEUE_REGISTRY_SIZE               0

// 不使用 tickless idle
#define configUSE_TICKLESS_IDLE                 0

// 不使用新库可重入结构
#define configUSE_NEWLIB_REENTRANT              0

/* ==== 需要包含的 API 函数 ==== */

#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    0
#define INCLUDE_vTaskDelayUntil                 0
#define INCLUDE_vTaskDelay                     1
#define INCLUDE_xTaskGetSchedulerState          0
#define INCLUDE_xTaskGetCurrentTaskHandle      0
#define INCLUDE_uxTaskGetStackHighWaterMark     0
#define INCLUDE_eTaskGetState                   0

/* ==== 中断向量名重定向 ==== */
/* port.c 提供 xPortPendSVHandler/xPortSVCHandler,
   通过宏重命名为启动向量表期望的 PendSV_Handler/SVC_Handler,
   SysTick_Handler 在 at32f403a_407_it.c 中显式调用 xPortSysTickHandler */

#define vPortSVCHandler                         SVC_Handler
#define xPortPendSVHandler                      PendSV_Handler

/* ==== 断言与钩子 ==== */

/* 钩子函数原型由 task.h 按 configCHECK_FOR_STACK_OVERFLOW /
   configUSE_MALLOC_FAILED_HOOK 开关自动声明, 实现在 app_main.c */

// 断言: 死循环挂起, 便于调试器定位
#define configASSERT(x)  do { if((x) == 0) { taskDISABLE_INTERRUPTS(); for(;;); } } while(0)

#endif /* FREERTOS_CONFIG_H */
