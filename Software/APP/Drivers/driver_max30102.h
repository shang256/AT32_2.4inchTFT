/**
  **************************************************************************
  * @file     driver_max30102.h
  * @brief    MAX30102 心率血氧传感器驱动接口（软件模拟 I2C, SCL=PA2 / SDA=PA3）
  *
  * 器件说明:
  *   - I2C 从机地址 0x57(7 位), 与 SGP30/AHT20/BMP280 共享同一软件总线;
  *   - Part ID 寄存器(0xFF)读回 0x15 作为在线验证依据;
  *   - SpO2 模式(MODE=0x03): 红/红外双 LED 交替发光, FIFO 每样本
  *     LED1(红光, 3 字节) + LED2(红外, 3 字节), ADC 18 位(0~262143);
  *   - 驱动内含定点心率/血氧算法(直流滤除 + 动态阈值峰检测),
  *     无浮点运算, 输出整数 BPM 与 SpO2%。
  *
  * 调用约定(与本项目其余传感器驱动一致):
  *   1. max30102_init() 上电初始化(含 Part ID 验证), 失败可周期重试;
  *   2. max30102_read() 以固定周期(500ms, 匹配 FIFO 深度)调用一次,
  *      内部完成 FIFO 批量读取 + 信号处理, 输出最新结果;
  *   3. 两个接口均操作共享 I2C 总线, 调用方必须持有 g_i2c_mutex。
  **************************************************************************
  */

#ifndef __DRIVER_MAX30102_H
#define __DRIVER_MAX30102_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/** 驱动返回状态 */
typedef enum
{
  MAX30102_OK = 0,                                    /* 成功 */
  MAX30102_ERR                                       /* 失败(离线/I2C 错误/未初始化) */
} max30102_status_t;

/** 采样与算法输出结果 */
typedef struct
{
  uint8_t  valid;                                     /* 1=模块在线且本次通信成功 */
  uint8_t  finger_on;                                 /* 1=检测到手指(红外 DC 高于阈值) */
  uint8_t  hr_bpm;                                   /* 心率(BPM), 0=信号不足暂无结果 */
  uint8_t  spo2;                                     /* 血氧饱和度(%), 0=信号不足暂无结果 */
} max30102_result_t;

max30102_status_t max30102_init(void);               /* 初始化(验证 Part ID + 配置采样) */
max30102_status_t max30102_read(max30102_result_t *result);  /* 周期读取并输出心率/血氧 */

#ifdef __cplusplus
}
#endif

#endif /* __DRIVER_MAX30102_H */
