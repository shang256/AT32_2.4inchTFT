/**
  **************************************************************************
  * @file     aht20.h
  * @brief    AHT20 温湿度传感器驱动接口（软件模拟 I2C，SCL=PA2 / SDA=PA3）
  *
  * 协议说明（Aosong AHT20 数据手册）：
  *   器件 7 位地址：0x38；
  *   - 初始化：上电稳定约 40ms 后发送 0xBE 0x08 0x00，等待 10ms；
  *   - 触发测量：发送 0xAC 0x33 0x00，等待 80ms 后读 6 字节：
  *     状态(1) + 湿度(20bit) + 温度(20bit)，无 CRC；
  *   - 湿度(%) = raw * 100 / 2^20；温度(°C) = raw * 200 / 2^20 - 50。
  *
  * 数值约定：为避免浮点，所有结果放大 10 倍返回（定点），
  *   例 temp_x10=253 表示 25.3°C，hum_x10=482 表示 48.2%RH。
  **************************************************************************
  */

#ifndef __AHT20_H
#define __AHT20_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/** AHT20 测量结果（定点 ×10） */
typedef struct
{
  int16_t  temp_x10;      /*!< 温度, 0.1°C（253 = 25.3°C, 负温为负值） */
  uint16_t hum_x10;       /*!< 湿度, 0.1%RH（482 = 48.2%） */
} aht20_data_t;

/** 驱动返回状态 */
typedef enum
{
  AHT20_OK  = 0,          /*!< 成功 */
  AHT20_ERR = 1           /*!< 失败（总线/应答/状态异常） */
} aht20_status_t;

aht20_status_t aht20_init(void);                       /* 初始化传感器（发初始化命令并校验状态） */
aht20_status_t aht20_read(aht20_data_t *data);         /* 触发测量并读取温湿度（含 80ms 等待） */

#ifdef __cplusplus
}
#endif

#endif /* __AHT20_H */
