/**
  **************************************************************************
  * @file     driver_aht20.c
  * @brief    AHT20 温湿度传感器驱动实现（软件模拟 I2C, SCL=PA2 / SDA=PA3）
  *
  * 参考 Aosong AHT20 数据手册, 采用 I2C 主机模式通信：
  *   - 设备地址：0x38
  *   - 初始化命令：0xBE 0x08 0x00（需等待约 10ms）
  *   - 触发测量命令：0xAC 0x33 0x00（需等待约 80ms）
  *   - 读回 6 字节：状态(1) + 湿度(20bit) + 温度(20bit)，无 CRC
  *
  * 数值约定：湿度/温度均放大 10 倍定点返回（避免浮点）：
  *   - 湿度(%) = raw * 100 / 2^20，放大 10 倍即 raw * 1000 / 2^20；
  *   - 温度(°C) = raw * 200 / 2^20 - 50，放大 10 倍即 raw * 2000 / 2^20 - 500。
  **************************************************************************
  */

#include <stddef.h>
#include "driver_aht20.h"
#include "driver_sw_i2c.h"
#include "driver_delay.h"

/** AHT20 7 位从机地址 */
#define AHT20_I2C_ADDR           0x38U
/** 初始化命令（0xBE 0x08 0x00） */
#define AHT20_CMD_INIT           0xBEU
/** 软复位命令（0xBA, 复位后需重新初始化校准） */
#define AHT20_CMD_SOFT_RESET     0xBAU
/** 触发测量命令（0xAC 0x33 0x00） */
#define AHT20_CMD_TRIG           0xACU
/** 读状态命令 */
#define AHT20_CMD_STATUS         0x71U

/** 上电稳定等待时间（ms） */
#define AHT20_POWER_ON_MS        40U
/** 软复位后等待时间（ms） */
#define AHT20_SOFT_RESET_WAIT_MS 20U
/** 初始化命令后等待时间（ms） */
#define AHT20_INIT_WAIT_MS       10U
/** 初始化后轮询校准状态的次数（10ms x N 次, 覆盖较慢模块的校准周期） */
#define AHT20_INIT_RETRY_MAX     10U
/** 触发测量后等待时间（ms） */
#define AHT20_MEASURE_WAIT_MS    80U

/** 状态寄存器：校准完成标志（1=已校准, 0=未校准） */
#define AHT20_STATUS_CAL_MASK    0x08U
/** 状态寄存器：忙标志（1=测量进行中） */
#define AHT20_STATUS_BUSY_MASK   0x80U

/** 模块是否已初始化 */
static uint8_t s_initialized = 0U;

/*********************************************************************
 * 函 数 名: aht20_read_status
 * 入口参数: status  存放读回的状态字节
 * 出口参数: status  写入状态字节
 * 返 回 值: aht20_status_t  AHT20_OK=成功
 * 函数功能: 读取 AHT20 状态寄存器（先写 0x71 命令再读 1 字节）
 * 说    明: AHT20 为"命令后读"时序, 与 SGP30 相同; 状态 bit3=校准标志,
 *           bit7=忙标志, 其余位保留
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月04日
 *     修改内容: 新增函数(AHT20 驱动需求)
 **********************************************************************/
static aht20_status_t aht20_read_status(uint8_t *status)
{
  const uint8_t cmd = AHT20_CMD_STATUS;

  /* ---- Step1: 写入读状态命令 ---- */
  if(sw_i2c_write(AHT20_I2C_ADDR, &cmd, 1U) != 0U)
  {
    return AHT20_ERR;
  }

  /* ---- Step2: 读回 1 字节状态 ---- */
  if(sw_i2c_read(AHT20_I2C_ADDR, status, 1U) != 0U)
  {
    return AHT20_ERR;
  }

  return AHT20_OK;
}

/*********************************************************************
 * 函 数 名: aht20_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: aht20_status_t  AHT20_OK=成功
 * 函数功能: 初始化软件 I2C 并完成 AHT20 上电校准
 * 说    明: 1. 先检测总线空闲电平, 便于快速定位接线/上拉问题;
 *           2. 上电稳定 40ms 后软复位(0xBA)再发初始化命令(0xBE), 等待;
 *           3. 读状态验证校准完成(bit3=1 表示已校准), 失败返回错误供调用方重试。
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月04日
 *     修改内容: 新增函数(AHT20 驱动需求)
 **********************************************************************/
aht20_status_t aht20_init(void)
{
  const uint8_t soft_reset = AHT20_CMD_SOFT_RESET;   /* 0xBA 软复位 */
  const uint8_t cmd[3] =
  {
    AHT20_CMD_INIT,       /* 0xBE */
    0x08U,                /* 校准使能 */
    0x00U                 /* 保留 */
  };
  uint8_t status = 0U;
  uint8_t retry;                                 /* 校准完成轮询计数 */

  /* ---- Step1: 初始化 PA2/PA3 软件 I2C 并检查总线空闲电平 ---- */
  sw_i2c_init();
  if(sw_i2c_bus_idle_ok() == 0U)
  {
    return AHT20_ERR;                            /* 总线被拉低: 接线/上拉异常 */
  }

  /* ---- Step2: 上电稳定等待(AHT20 上电后需 >40ms 才可通信) ---- */
  delay_ms(AHT20_POWER_ON_MS);

  /* ---- Step3: 软复位, 恢复到上电初始状态(解决残留异常状态导致校准失败) ---- */
  sw_i2c_write(AHT20_I2C_ADDR, &soft_reset, 1U);
  delay_ms(AHT20_SOFT_RESET_WAIT_MS);

  /* ---- Step4: 发送初始化命令 ---- */
  if(sw_i2c_write(AHT20_I2C_ADDR, cmd, 3U) != 0U)
  {
    return AHT20_ERR;
  }

  /* ---- Step5: 轮询状态直到校准完成(bit3=1 表示已校准)或超时 ---- */
  for(retry = 0U; retry < AHT20_INIT_RETRY_MAX; retry++)
  {
    delay_ms(AHT20_INIT_WAIT_MS);
    if(aht20_read_status(&status) != AHT20_OK)
    {
      return AHT20_ERR;
    }
    if((status & AHT20_STATUS_CAL_MASK) != 0U)
    {
      s_initialized = 1U;
      return AHT20_OK;
    }
  }

  return AHT20_ERR;                              /* 超时仍未校准完成 */
}

/*********************************************************************
 * 函 数 名: aht20_read
 * 入口参数: data  存放测量结果的指针(不得为 NULL)
 * 出口参数: data  写入温湿度定点值(×10)
 * 返 回 值: aht20_status_t  AHT20_OK=成功
 * 函数功能: 触发一次测量并解析温湿度结果
 * 说    明: 触发后需等待 80ms 测量周期; 读回 6 字节为
 *           状态 + 湿度(20bit) + 温度(20bit), 忙标志置位时失败;
 *           湿度/温度按 20bit 原始值换算并放大 10 倍定点输出
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月04日
 *     修改内容: 新增函数(AHT20 驱动需求)
 **********************************************************************/
aht20_status_t aht20_read(aht20_data_t *data)
{
  const uint8_t cmd[3] =
  {
    AHT20_CMD_TRIG,       /* 0xAC */
    0x33U,                /* 测量数据 + 温度 */
    0x00U                 /* 保留 */
  };
  uint8_t  buf[6] = {0U};
  uint32_t hum_raw;       /* 湿度原始 20bit */
  uint32_t temp_raw;      /* 温度原始 20bit */

  if((s_initialized == 0U) || (data == NULL))
  {
    return AHT20_ERR;
  }

  /* ---- Step1: 发送触发测量命令 ---- */
  if(sw_i2c_write(AHT20_I2C_ADDR, cmd, 3U) != 0U)
  {
    return AHT20_ERR;
  }

  /* ---- Step2: 等待测量完成(数据手册测量周期约 80ms) ---- */
  delay_ms(AHT20_MEASURE_WAIT_MS);

  /* ---- Step3: 读回 6 字节: 状态 + 湿度 + 温度 ---- */
  if(sw_i2c_read(AHT20_I2C_ADDR, buf, 6U) != 0U)
  {
    return AHT20_ERR;
  }

  /* ---- Step4: 校验忙标志(bit7=1 表示数据未就绪) ---- */
  if((buf[0] & AHT20_STATUS_BUSY_MASK) != 0U)
  {
    return AHT20_ERR;
  }

  /* ---- Step5: 拼接 20bit 原始值 ----
     湿度: buf[1]<<12 | buf[2]<<4 | buf[3]>>4
     温度: (buf[3]&0x0F)<<16 | buf[4]<<8 | buf[5] */
  hum_raw  = ((uint32_t)buf[1] << 12)
           | ((uint32_t)buf[2] << 4)
           | ((uint32_t)buf[3] >> 4);
  temp_raw = ((uint32_t)(buf[3] & 0x0FU) << 16)
           | ((uint32_t)buf[4] << 8)
           |  (uint32_t)buf[5];

  /* ---- Step6: 换算为定点 ×10 结果 ----
     温度先按无符号算出 0~2000(0~200°C 未减 50 的中间量), 再转有符号
     减 500(即 -50°C), 避免负温时无符号下溢 */
  data->hum_x10  = (uint16_t)((hum_raw  * 1000U) / 1048576U);            /* %RH ×10 */
  data->temp_x10 = (int16_t)((int32_t)((temp_raw * 2000U) / 1048576U) - 500);   /* °C ×10 */

  return AHT20_OK;
}
