/**
  **************************************************************************
  * @file     app_main.h
  * @brief    应用层入口接口（FreeRTOS 任务框架）
  **************************************************************************
  */

#ifndef __APP_MAIN_H
#define __APP_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* I2C 总线互斥锁(软件 I2C 非线程安全, SGP30/AHT20/BMP280 多任务共享
   同一条总线, 各传感器任务访问前须 take、访问后 give) */
extern SemaphoreHandle_t g_i2c_mutex;

/* 编码器事件队列项: 编码器任务 -> LVGL 任务 单向传递
   (统一使用 int32 字段, 避免混合宽度带来的结构体填充) */
typedef struct
{
  int32_t  delta;          /* 旋转增量(4 倍频计步, 0 表示本项仅为按键事件) */
  int32_t  push_pressed;   /* "松开->按下"沿事件: 1=发生了按下 */
  int32_t  hold_ms;        /* 当前连续按下时长(ms): >0 供 UI 显示关机进度, 0=取消提示 */
  int32_t  wakeup;         /* 停机唤醒开机事件: 1=按键唤醒了停机, 状态机重走 LOGO 流程 */
} app_encoder_msg_t;

/* 编码器事件队列(应用层共享, 由 app_main 创建) */
extern QueueHandle_t g_encoder_queue;

/* SGP30 传感器数据队列项: SGP30 任务 -> LVGL 任务 单向传递
   (统一使用 32 位字段, 避免混合宽度带来的结构体填充) */
typedef struct
{
  uint32_t  eco2;         /* eCO2 浓度(ppm) */
  uint32_t  tvoc;         /* TVOC 浓度(ppb) */
  uint32_t  valid;        /* 1=数据有效 0=读取失败(UI 显示 "--") */
} app_sensor_msg_t;

/* SGP30 传感器数据队列(应用层共享, 由 app_main 创建) */
extern QueueHandle_t g_sensor_queue;

/* 环境传感器数据队列项: 环境传感器任务 -> LVGL 任务 单向传递
   (AHT20 温湿度 + BMP280 气压, 各自带在线标志, 可独立显示 "--";
   统一使用 int32 字段, 避免混合宽度带来的结构体填充) */
typedef struct
{
  int32_t  temp_x10;      /* AHT20 温度(0.1°C, 如 253=25.3°C) */
  int32_t  hum_x10;       /* AHT20 湿度(0.1%RH, 如 482=48.2%) */
  int32_t  press_x10;     /* BMP280 气压(0.1hPa, 如 10132=1013.2hPa) */
  int32_t  altitude;      /* BMP280 换算海拔(米, 如 125=125m) */
  int32_t  aht_valid;     /* 1=AHT20 在线且数据有效 0=离线(显示 "--") */
  int32_t  bmp_valid;     /* 1=BMP280 在线且数据有效 0=离线(显示 "--") */
} app_env_msg_t;

/* 环境传感器数据队列(应用层共享, 由 app_main 创建) */
extern QueueHandle_t g_env_queue;

/* 心率血氧数据队列项: MAX30102 任务 -> LVGL 任务 单向传递
   (统一使用 int32 字段, 避免混合宽度带来的结构体填充) */
typedef struct
{
  int32_t  hr_bpm;       /* 心率(BPM), 0=信号不足 */
  int32_t  spo2;         /* 血氧饱和度(%), 0=信号不足 */
  int32_t  finger_on;    /* 1=检测到手指 0=未放置(提示放置手指) */
  int32_t  valid;        /* 1=MAX30102 在线 0=离线(显示 "--") */
} app_hr_msg_t;

/* 心率血氧数据队列(应用层共享, 由 app_main 创建) */
extern QueueHandle_t g_hr_queue;

/* 指南针数据队列项: QMC6309 -> LVGL 任务 单向传递
   (统一使用 int32 字段, 避免混合宽度带来的结构体填充) */
typedef struct
{
  int32_t  heading_x10;   /* 磁航向角(0.1°, 0~3599, 北=0 顺时针递增) */
  int32_t  raw_x;         /* X 轴原始值(调试/自检显示用) */
  int32_t  raw_y;         /* Y 轴原始值(调试/自检显示用) */
  int32_t  raw_z;         /* Z 轴原始值(调试/自检显示用) */
  int32_t  valid;         /* 1=QMC6309 在线且数据有效 0=离线(显示 "--") */
} app_compass_msg_t;

/* 指南针数据队列(应用层共享, 由 app_main 创建) */
extern QueueHandle_t g_compass_queue;

#ifdef __cplusplus
}
#endif

#endif /* __APP_MAIN_H */
