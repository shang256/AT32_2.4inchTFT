/**
  **************************************************************************
  * @file     sgp30.h
  * @brief    SGP30 空气质量传感器驱动接口（eCO2 / TVOC, 软件模拟 I2C）
  *
  * 移植说明：
  *   原驱动基于 ESP-IDF I2C API(esp_err.h/driver/i2c.h), 本工程将其移植
  *   到 AT32F403A: I2C 事务改走 Drivers/sw_i2c(SCL=PA2, SDA=PA3),
  *   协议逻辑(命令字/时序/CRC)与 Sensirion 数据手册完全一致:
  *     - 器件 7 位地址: 0x58
  *     - Init_air_quality   命令 0x2003, 发送后等待 10ms
  *     - Measure_air_quality 命令 0x2008, 等待 12ms 后读 6 字节:
  *       eCO2(2B)+CRC + TVOC(2B)+CRC, 数据大端
  *     - CRC-8: 多项式 0x31, 初值 0xFF
  *   使用要点(数据手册): 初始化后需以 1s 周期持续测量以维护基线;
  *   首个约 15s 内输出固定值 eCO2=400ppm / TVOC=0ppb(预热期, 属正常)。
  **************************************************************************
  */

#ifndef __SGP30_H
#define __SGP30_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/** SGP30 测量结果 */
typedef struct
{
  uint16_t eco2;      /*!< 等效二氧化碳浓度, 单位 ppm */
  uint16_t tvoc;      /*!< 总挥发性有机物浓度, 单位 ppb */
} sgp30_data_t;

/** 驱动返回状态 */
typedef enum
{
  SGP30_OK  = 0,     /*!< 成功 */
  SGP30_ERR = 1      /*!< 失败(总线/应答/CRC 校验异常) */
} sgp30_status_t;

sgp30_status_t sgp30_init(void);                       /* 初始化软件 I2C 并发送 Init_air_quality 命令 */
sgp30_status_t sgp30_read(sgp30_data_t *data);         /* 发送测量命令并读取 eCO2/TVOC 结果 */

#ifdef __cplusplus
}
#endif

#endif /* __SGP30_H */
