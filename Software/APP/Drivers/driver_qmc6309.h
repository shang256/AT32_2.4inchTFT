/**
  **************************************************************************
  * @file     driver_qmc6309.h
  * @brief    QMC6309 三轴地磁传感器驱动接口（软件模拟 I2C, SCL=PA2 / SDA=PA3）
  *
  * 器件说明(QST 矽睿, 电子罗盘用磁力计):
  *   - I2C 7 位从机地址 0x1A, 与 SGP30/AHT20/BMP280/MAX30102 共享总线;
  *   - Chip ID 寄存器 0x00, 读回 0x90 作为在线验证依据;
  *   - 数据寄存器 0x01~0x06: X/Y/Z 各 16 位有符号, 小端(低字节在前);
  *   - 状态寄存器 0x09: bit0 DRDY(读后自动清), bit1 OVFL, bit4 NVM 加载完成;
  *   - Ctrl1(0x0A): 工作模式/过采样/LPF 深度;
  *     Ctrl2(0x0B): 软复位/输出速率/量程/Set-Reset 偏移校正;
  *   - 配置: 连续模式 + 50Hz ODR + ±8 高斯量程(灵敏度最高)。
  *
  * 方位角(指南针)计算:
  *   - 驱动内含 CORDIC 定点 atan2(无浮点), 输出 0.1° 单位(0~3599);
  *   - 内建简单硬铁校准: 运行时持续跟踪 X/Y 的 min/max 并去中心偏移,
  *     设备水平放置旋转数圈后指向收敛;
  *   - 未做倾角补偿(纯磁力计无加速度计), 假设设备水平使用。
  **************************************************************************
  */

#ifndef __DRIVER_QMC6309_H
#define __DRIVER_QMC6309_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/** 驱动返回状态 */
typedef enum
{
  QMC6309_OK = 0,                                     /* 成功 */
  QMC6309_ERR                                        /* 失败(离线/I2C 错误/未初始化) */
} qmc6309_status_t;

/** 三轴磁场原始数据(有符号, LSB) */
typedef struct
{
  int16_t x;                                          /* X 轴原始值 */
  int16_t y;                                          /* Y 轴原始值 */
  int16_t z;                                          /* Z 轴原始值 */
} qmc6309_data_t;

qmc6309_status_t qmc6309_init(void);                  /* 初始化(验证 Chip ID + 连续模式配置) */
qmc6309_status_t qmc6309_read(qmc6309_data_t *data);  /* 读取三轴原始值(DRDY 就绪时) */
int32_t qmc6309_heading(const qmc6309_data_t *data);  /* 计算方位角(0.1°, 0~3599, 北=0) */

#ifdef __cplusplus
}
#endif

#endif /* __DRIVER_QMC6309_H */
