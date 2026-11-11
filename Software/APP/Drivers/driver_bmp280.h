/**
  **************************************************************************
  * @file     driver_bmp280.h
  * @brief    BMP280 气压传感器驱动接口（软件模拟 I2C，SCL=PA2 / SDA=PA3）
  *
  * 协议说明（Bosch BMP280 数据手册）：
  *   器件 7 位地址：0x77（SDO 悬空/接 VCC）或 0x76（SDO 接地），本板用 0x77；
  *   - 上电后从 0x88 起读 24 字节校准参数（dig_T1~T3 / dig_P1~P9）；
  *   - 配置 0xF4=0xB3（温度 x16 / 气压 x8 过采样, normal 模式）、0xF5=0x14（IIR 滤波 x16）；
  *   - 从 0xF7 起读 6 字节原始值（气压 20bit + 温度 20bit），
  *     按数据手册定点补偿公式换算为气压。
  *
  * 数值约定：气压放大 10 倍返回（定点），例 press_x10=10132 表示 1013.2 hPa。
  **************************************************************************
  */

#ifndef __DRIVER_BMP280_H
#define __DRIVER_BMP280_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/** BMP280 测量结果（定点 ×10） */
typedef struct
{
  int32_t press_x10;      /*!< 气压, 0.1hPa（10132 = 1013.2 hPa） */
} bmp280_data_t;

/** 驱动返回状态 */
typedef enum
{
  BMP280_OK  = 0,         /*!< 成功 */
  BMP280_ERR = 1          /*!< 失败（总线/应答/芯片 ID 异常） */
} bmp280_status_t;

bmp280_status_t bmp280_init(void);                     /* 读取校准参数并配置传感器 */
bmp280_status_t bmp280_read(bmp280_data_t *data);      /* 读取原始值并换算为气压 */
int16_t bmp280_altitude(int32_t press_x10);            /* 气压(0.1hPa) -> 海拔(米, 参考 1015.7hPa/25°C) */

#ifdef __cplusplus
}
#endif

#endif /* __DRIVER_BMP280_H */
