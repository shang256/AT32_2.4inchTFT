/**
  **************************************************************************
  * @file     sw_i2c.h
  * @brief    软件模拟 I2C 主机接口（SCL=PA2 / SDA=PA3, 开漏输出 + 位操作）
  *
  * 硬件说明：
  *   1. AT32F403A 的 PA2/PA3 不是任何硬件 I2C 外设的复用引脚, 本模块用
  *      GPIO 开漏输出 + DWT 微秒延时模拟标准 I2C 时序(约 100kHz);
  *   2. 开漏输出配合总线上拉电阻实现"线与"——释放引脚=写 1(靠上拉变高),
  *      拉低引脚=写 0; SGP30 模块板载 4.7k~10k 上拉, 可直接挂接;
  *   3. 读取 SDA 电平直接查询 IDR 寄存器(输出模式下 IDR 仍可读, 释放
  *      引脚后读到的是总线真实电平, 天然支持 ACK/数据回读);
  *   4. 无时钟拉伸处理(Sensirion SGP30 不拉伸 SCL), 任何通信失败都会
  *      以错误码返回, 不会挂死。
  **************************************************************************
  */

#ifndef __SW_I2C_H
#define __SW_I2C_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/* ---------------- 引脚定义(可修改以适配不同接线) ---------------- */
#define SW_I2C_SCL_PORT         GPIOA           /* SCL 输出端口 */
#define SW_I2C_SCL_PIN          GPIO_PINS_2     /* SCL 输出引脚(PA2) */
#define SW_I2C_SDA_PORT         GPIOA           /* SDA 输入输出端口 */
#define SW_I2C_SDA_PIN          GPIO_PINS_3     /* SDA 输入输出引脚(PA3) */

void   sw_i2c_init(void);                       /* 初始化引脚并释放总线 */
uint8_t sw_i2c_bus_idle_ok(void);               /* 总线空闲检测: 1=两线均高(上拉正常) 0=异常 */
uint8_t sw_i2c_write(uint8_t dev_addr, const uint8_t *buf, uint16_t len);   /* 写: 地址(7位)+数据, 0=成功 */
uint8_t sw_i2c_read(uint8_t dev_addr, uint8_t *buf, uint16_t len);          /* 读: 地址(7位)+数据, 0=成功 */

#ifdef __cplusplus
}
#endif

#endif /* __SW_I2C_H */
