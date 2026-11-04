/**
  **************************************************************************
  * @file     bmp280.c
  * @brief    BMP280 气压传感器驱动实现（软件模拟 I2C, SCL=PA2 / SDA=PA3）
  *
  * 参考 Bosch BMP280 数据手册, 采用 I2C 主机模式通信：
  *   - 设备地址：0x76（SDO 接地, 可通过宏改为 0x77）
  *   - 校准参数：0x88 起连续 24 字节（dig_T1~T3, dig_P1~P9）
  *   - 配置寄存器：0xF4=0x27（温度/气压 x1 过采样 + normal 模式）
  *                0xF5=0x00（IIR 滤波关, t_sb=0.5ms, 4 线 SPI 无关）
  *   - 数据寄存器：0xF7 起读 6 字节（气压 20bit + 温度 20bit）
  *
  * 补偿算法：严格按照 Bosch 数据手册的定点公式实现（int32/int64），
  *   温度补偿结果 t_fine 作为气压补偿的中间量；最终气压为 Pa，
  *   放大 10 倍(Pa/10)输出, 即 0.1hPa 定点。
  **************************************************************************
  */

#include <stddef.h>
#include <stdint.h>
#include "bmp280.h"
#include "sw_i2c.h"
#include "delay.h"

/** BMP280 7 位从机地址（SDO 接 GND 为 0x76, 接 VCC 为 0x77） */
#define BMP280_I2C_ADDR          0x76U
/** 芯片 ID 寄存器 */
#define BMP280_REG_CHIP_ID       0xD0U
/** 期望芯片 ID 值 */
#define BMP280_CHIP_ID_VAL       0x58U
/** 校准参数起始寄存器（连续 24 字节） */
#define BMP280_REG_CALIB         0x88U
/** 控制测量寄存器 */
#define BMP280_REG_CTRL_MEAS     0xF4U
/** 配置寄存器 */
#define BMP280_REG_CONFIG        0xF5U
/** 数据起始寄存器（连续 6 字节） */
#define BMP280_REG_DATA          0xF7U

/** ctrl_meas: 温度 x1 过采样 + 气压 x1 过采样 + normal 模式 */
#define BMP280_CTRL_MEAS_VAL     0x27U
/** config: t_sb=0.5ms + filter 关 + spi3w 关 */
#define BMP280_CONFIG_VAL        0x00U

/** 模块是否已初始化 */
static uint8_t s_initialized = 0U;

/* 校准参数（signed 类型与数据手册一致, 用于定点补偿） */
static uint16_t s_dig_T1;   /*!< 温度校准 T1 */
static int16_t  s_dig_T2;   /*!< 温度校准 T2 */
static int16_t  s_dig_T3;   /*!< 温度校准 T3 */
static uint16_t s_dig_P1;   /*!< 气压校准 P1 */
static int16_t  s_dig_P2;   /*!< 气压校准 P2 */
static int16_t  s_dig_P3;   /*!< 气压校准 P3 */
static int16_t  s_dig_P4;   /*!< 气压校准 P4 */
static int16_t  s_dig_P5;   /*!< 气压校准 P5 */
static int16_t  s_dig_P6;   /*!< 气压校准 P6 */
static int16_t  s_dig_P7;   /*!< 气压校准 P7 */
static int16_t  s_dig_P8;   /*!< 气压校准 P8 */
static int16_t  s_dig_P9;   /*!< 气压校准 P9 */

/** 温度精细值（气压补偿中间量） */
static int32_t s_t_fine;

/*********************************************************************
 * 函 数 名: bmp280_read_regs
 * 入口参数: reg    起始寄存器地址
 *           buf    读出数据缓冲区
 *           len    读字节数
 * 出口参数: buf    存放读到的数据
 * 返 回 值: bmp280_status_t  BMP280_OK=成功
 * 函数功能: 从指定寄存器起连续读取（先写寄存器地址再读）
 * 说    明: BMP280 为"寄存器地址 + 连续读"时序, 先写 1 字节地址
 *           再启动读事务, 与 sw_i2c 的 write/read 分开接口对应
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月04日
 *     修改内容: 新增函数(BMP280 驱动需求)
 **********************************************************************/
static bmp280_status_t bmp280_read_regs(uint8_t reg, uint8_t *buf, uint16_t len)
{
  if(sw_i2c_write(BMP280_I2C_ADDR, &reg, 1U) != 0U)
  {
    return BMP280_ERR;
  }
  if(sw_i2c_read(BMP280_I2C_ADDR, buf, len) != 0U)
  {
    return BMP280_ERR;
  }
  return BMP280_OK;
}

/*********************************************************************
 * 函 数 名: bmp280_write_reg
 * 入口参数: reg    寄存器地址
 *           val    写入值
 * 出口参数: 无
 * 返 回 值: bmp280_status_t  BMP280_OK=成功
 * 函数功能: 写单个寄存器（地址 + 1 字节数据）
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月04日
 *     修改内容: 新增函数(BMP280 驱动需求)
 **********************************************************************/
static bmp280_status_t bmp280_write_reg(uint8_t reg, uint8_t val)
{
  const uint8_t buf[2] = { reg, val };

  if(sw_i2c_write(BMP280_I2C_ADDR, buf, 2U) != 0U)
  {
    return BMP280_ERR;
  }
  return BMP280_OK;
}

/*********************************************************************
 * 函 数 名: bmp280_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: bmp280_status_t  BMP280_OK=成功
 * 函数功能: 校验芯片 ID、读取校准参数并配置传感器进入 normal 模式
 * 说    明: 1. 先检测总线空闲电平, 再读芯片 ID 做在线判定;
 *           2. 从 0x88 起读 24 字节校准参数, 按小端组合为 12 个系数;
 *           3. 写入 ctrl_meas/config 使传感器开始连续测量。
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月04日
 *     修改内容: 新增函数(BMP280 驱动需求)
 **********************************************************************/
bmp280_status_t bmp280_init(void)
{
  uint8_t calib[24] = {0U};
  uint8_t chip_id = 0U;

  /* ---- Step1: 初始化 PA2/PA3 软件 I2C 并检查总线空闲电平 ---- */
  sw_i2c_init();
  if(sw_i2c_bus_idle_ok() == 0U)
  {
    return BMP280_ERR;                           /* 总线被拉低: 接线/上拉异常 */
  }

  /* ---- Step2: 校验芯片 ID（在线判定） ---- */
  if(bmp280_read_regs(BMP280_REG_CHIP_ID, &chip_id, 1U) != BMP280_OK)
  {
    return BMP280_ERR;
  }
  if(chip_id != BMP280_CHIP_ID_VAL)
  {
    return BMP280_ERR;                           /* 非 BMP280 或地址错误 */
  }

  /* ---- Step3: 读取 24 字节校准参数并按小端组合 ---- */
  if(bmp280_read_regs(BMP280_REG_CALIB, calib, 24U) != BMP280_OK)
  {
    return BMP280_ERR;
  }
  s_dig_T1 = (uint16_t)(((uint16_t)calib[1]  << 8) | calib[0]);
  s_dig_T2 = (int16_t) (((uint16_t)calib[3]  << 8) | calib[2]);
  s_dig_T3 = (int16_t) (((uint16_t)calib[5]  << 8) | calib[4]);
  s_dig_P1 = (uint16_t)(((uint16_t)calib[7]  << 8) | calib[6]);
  s_dig_P2 = (int16_t) (((uint16_t)calib[9]  << 8) | calib[8]);
  s_dig_P3 = (int16_t) (((uint16_t)calib[11] << 8) | calib[10]);
  s_dig_P4 = (int16_t) (((uint16_t)calib[13] << 8) | calib[12]);
  s_dig_P5 = (int16_t) (((uint16_t)calib[15] << 8) | calib[14]);
  s_dig_P6 = (int16_t) (((uint16_t)calib[17] << 8) | calib[16]);
  s_dig_P7 = (int16_t) (((uint16_t)calib[19] << 8) | calib[18]);
  s_dig_P8 = (int16_t) (((uint16_t)calib[21] << 8) | calib[20]);
  s_dig_P9 = (int16_t) (((uint16_t)calib[23] << 8) | calib[22]);

  /* ---- Step4: 配置传感器: 过采样 x1 + normal 连续测量 ---- */
  if(bmp280_write_reg(BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_VAL) != BMP280_OK)
  {
    return BMP280_ERR;
  }
  if(bmp280_write_reg(BMP280_REG_CONFIG, BMP280_CONFIG_VAL) != BMP280_OK)
  {
    return BMP280_ERR;
  }

  /* ---- Step5: 等待 normal 模式首轮测量完成(x1 过采样约 9.5ms) ---- */
  delay_ms(10U);

  s_initialized = 1U;

  return BMP280_OK;
}

/*********************************************************************
 * 函 数 名: bmp280_compensate_temp
 * 入口参数: adc_T  温度原始值（20bit）
 * 出口参数: 无
 * 返 回 值: int32_t  温度（0.01°C, 仅用于校验, 不作为对外输出）
 * 函数功能: 按 Bosch 数据手册公式做温度补偿, 并更新 s_t_fine
 * 说    明: t_fine 是气压补偿的必要中间量; 本驱动对外只输出气压,
 *           温度补偿结果不单独暴露
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月04日
 *     修改内容: 新增函数(BMP280 驱动需求)
 **********************************************************************/
static int32_t bmp280_compensate_temp(int32_t adc_T)
{
  int32_t var1;
  int32_t var2;
  int32_t T;

  var1 = ((((adc_T >> 3) - ((int32_t)s_dig_T1 << 1))) * ((int32_t)s_dig_T2)) >> 11;
  var2 = (((((adc_T >> 4) - ((int32_t)s_dig_T1))
           * ((adc_T >> 4) - ((int32_t)s_dig_T1))) >> 12)
           * ((int32_t)s_dig_T3)) >> 14;
  s_t_fine = var1 + var2;
  T = (s_t_fine * 5 + 128) >> 8;                 /* 0.01°C */

  return T;
}

/*********************************************************************
 * 函 数 名: bmp280_compensate_press
 * 入口参数: adc_P  气压原始值（20bit）
 * 出口参数: 无
 * 返 回 值: uint32_t  气压（Pa）
 * 函数功能: 按 Bosch 数据手册公式做气压补偿
 * 说    明: 64 位定点运算, 须先调用温度补偿以更新 s_t_fine;
 *           分母为 0 时返回 0 由上层判错
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月04日
 *     修改内容: 新增函数(BMP280 驱动需求)
 **********************************************************************/
static uint32_t bmp280_compensate_press(int32_t adc_P)
{
  int64_t var1;
  int64_t var2;
  int64_t p;

  var1 = ((int64_t)s_t_fine) - 128000;
  var2 = var1 * var1 * (int64_t)s_dig_P6;
  var2 = var2 + ((var1 * (int64_t)s_dig_P5) << 17);
  var2 = var2 + (((int64_t)s_dig_P4) << 35);
  var1 = ((var1 * var1 * (int64_t)s_dig_P3) >> 8) + ((var1 * (int64_t)s_dig_P2) << 12);
  var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)s_dig_P1) >> 33;

  if(var1 == 0)
  {
    return 0U;                                   /* 避免除零 */
  }

  p = 1048576 - adc_P;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (((int64_t)s_dig_P9) * (p >> 13) * (p >> 13)) >> 25;
  var2 = (((int64_t)s_dig_P8) * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (((int64_t)s_dig_P7) << 4);

  return (uint32_t)p;                            /* 单位 Pa */
}

/*********************************************************************
 * 函 数 名: bmp280_read
 * 入口参数: data  存放测量结果的指针(不得为 NULL)
 * 出口参数: data  写入气压定点值(×10 hPa)
 * 返 回 值: bmp280_status_t  BMP280_OK=成功
 * 函数功能: 读取原始值并经补偿换算为气压
 * 说    明: 从 0xF7 起读 6 字节(气压 20bit + 温度 20bit), 先做温度
 *           补偿得到 t_fine, 再做气压补偿; 气压 Pa/10 得到 0.1hPa 定点
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月04日
 *     修改内容: 新增函数(BMP280 驱动需求)
 **********************************************************************/
bmp280_status_t bmp280_read(bmp280_data_t *data)
{
  uint8_t  buf[6] = {0U};
  int32_t  adc_P;
  int32_t  adc_T;
  uint32_t press_pa;

  if((s_initialized == 0U) || (data == NULL))
  {
    return BMP280_ERR;
  }

  /* ---- Step1: 读 6 字节原始数据 ---- */
  if(bmp280_read_regs(BMP280_REG_DATA, buf, 6U) != BMP280_OK)
  {
    return BMP280_ERR;
  }

  /* ---- Step2: 拼接 20bit 原始值(气压在前, 温度在后) ---- */
  adc_P = ((int32_t)buf[0] << 12)
        | ((int32_t)buf[1] << 4)
        | ((int32_t)buf[2] >> 4);
  adc_T = ((int32_t)buf[3] << 12)
        | ((int32_t)buf[4] << 4)
        | ((int32_t)buf[5] >> 4);

  /* ---- Step3: 温度补偿(更新 t_fine)后做气压补偿 ---- */
  (void)bmp280_compensate_temp(adc_T);
  press_pa = bmp280_compensate_press(adc_P);

  /* ---- Step4: Pa -> 0.1hPa 定点(Pa/10), 0 视为异常 ---- */
  if(press_pa == 0U)
  {
    return BMP280_ERR;
  }
  data->press_x10 = (int32_t)(press_pa / 10U);

  return BMP280_OK;
}
