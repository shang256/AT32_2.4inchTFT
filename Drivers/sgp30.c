/**
  **************************************************************************
  * @file     sgp30.c
  * @brief    SGP30 空气质量传感器驱动实现（移植自 ESP-IDF 版本, 逻辑不变）
  *
  * 参考 SGP30 数据手册, 采用 I2C 主机模式通信:
  *   - 设备地址：0x58
  *   - 初始化命令：0x20 0x03（Init_air_quality），需等待约 10ms
  *   - 测量命令：0x20 0x08（Measure_air_quality），返回 6 字节数据
  *
  * 移植差异(相对 ESP-IDF 原版):
  *   1. I2C 事务由 driver/i2c.h 命令链模式改为本工程 Drivers/sw_i2c
  *      的软件模拟 I2C(SCL=PA2, SDA=PA3);
  *   2. vTaskDelay 等待改为 delay_ms 忙等(DWT 实现, 保持驱动层不依赖
  *      FreeRTOS; 调用方为低优先级任务, 忙等期间可被高优先级任务抢占);
  *   3. esp_err_t 返回值改为 sgp30_status_t, ESP_LOG 打印移除。
  **************************************************************************
  */

#include <stddef.h>
#include "sgp30.h"
#include "sw_i2c.h"
#include "delay.h"

/** SGP30 7 位从机地址 */
#define SGP30_I2C_ADDR                0x58U
/** 初始化命令（Init_air_quality） */
#define SGP30_CMD_INIT_AIR_QUALITY    0x2003U
/** 测量命令（Measure_air_quality） */
#define SGP30_CMD_MEASURE_AIR_QUALITY 0x2008U

/** CRC-8 多项式（SGP30 专用：0x31，初始值 0xFF） */
#define SGP30_CRC8_POLYNOMIAL         0x31U
#define SGP30_CRC8_INIT               0xFFU

/** 初始化命令后的等待时间（ms） */
#define SGP30_INIT_WAIT_MS            10U
/** 测量命令后的等待时间（ms, 数据手册测量周期约 12ms） */
#define SGP30_MEASURE_WAIT_MS         12U

/** 模块是否已初始化 */
static uint8_t s_initialized = 0U;

/*********************************************************************
 * 函 数 名: sgp30_crc8
 * 入口参数: data  待校验数据
 *           len   数据长度
 * 出口参数: 无
 * 返 回 值: uint8_t CRC-8 校验值
 * 函数功能: 计算 SGP30 的 CRC-8 校验值(多项式 0x31, 初值 0xFF)
 * 说    明: 与 Sensirion 数据手册一致, 移植自原版未改动
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 移植(ESP-IDF -> AT32F403A)
 **********************************************************************/
static uint8_t sgp30_crc8(const uint8_t *data, uint8_t len)
{
  uint8_t crc = SGP30_CRC8_INIT;
  uint8_t i;
  uint8_t bit;

  for(i = 0U; i < len; i++)
  {
    crc ^= data[i];
    for(bit = 0U; bit < 8U; bit++)
    {
      if((crc & 0x80U) != 0U)
      {
        crc = (uint8_t)(((uint32_t)crc << 1) ^ SGP30_CRC8_POLYNOMIAL);
      }
      else
      {
        crc = (uint8_t)((uint32_t)crc << 1);
      }
    }
  }

  return crc;
}

/*********************************************************************
 * 函 数 名: sgp30_send_command
 * 入口参数: command  16 位命令字(内部按大端拆为 2 字节发送)
 * 出口参数: 无
 * 返 回 值: sgp30_status_t  SGP30_OK=成功
 * 函数功能: 向 SGP30 发送一条命令(2 字节大端命令字)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 移植(i2c 命令链 -> sw_i2c_write)
 **********************************************************************/
static sgp30_status_t sgp30_send_command(uint16_t command)
{
  /* 命令按大端拆为高/低字节两字节发送 */
  const uint8_t cmd[2] =
  {
    (uint8_t)(command >> 8),      /* 命令高字节 */
    (uint8_t)(command & 0xFFU)    /* 命令低字节 */
  };

  if(sw_i2c_write(SGP30_I2C_ADDR, cmd, 2U) != 0U)
  {
    return SGP30_ERR;
  }

  return SGP30_OK;
}

/*********************************************************************
 * 函 数 名: sgp30_read_bytes
 * 入口参数: data  存放读取数据的缓冲区
 *           len   读取长度（字节）
 * 出口参数: data  存放读到的数据
 * 返 回 值: sgp30_status_t  SGP30_OK=成功
 * 函数功能: 从 SGP30 连续读取数据(末字节 NACK 结束)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 移植(i2c 命令链 -> sw_i2c_read)
 **********************************************************************/
static sgp30_status_t sgp30_read_bytes(uint8_t *data, uint8_t len)
{
  if(sw_i2c_read(SGP30_I2C_ADDR, data, len) != 0U)
  {
    return SGP30_ERR;
  }

  return SGP30_OK;
}

/*********************************************************************
 * 函 数 名: sgp30_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: sgp30_status_t  SGP30_OK=成功
 * 函数功能: 初始化软件 I2C 引脚并发送传感器初始化命令(Init_air_quality)
 * 说    明: 相对原版去掉了 sda/scl 引脚参数(引脚在 sw_i2c.h 中以宏
 *           定义); 初始化前检测总线空闲电平, 便于快速定位接线/上拉问题
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 移植(ESP-IDF -> AT32F403A)
 **********************************************************************/
sgp30_status_t sgp30_init(void)
{
  /* ---- Step1: 初始化 PA2/PA3 软件 I2C 并检查总线空闲电平 ---- */
  sw_i2c_init();
  if(sw_i2c_bus_idle_ok() == 0U)
  {
    return SGP30_ERR;                            /* 总线被拉低: 接线/上拉异常 */
  }

  /* ---- Step2: 发送初始化命令 ---- */
  if(sgp30_send_command(SGP30_CMD_INIT_AIR_QUALITY) != SGP30_OK)
  {
    return SGP30_ERR;
  }

  /* ---- Step3: 等待传感器完成初始化 ---- */
  delay_ms(SGP30_INIT_WAIT_MS);

  s_initialized = 1U;

  return SGP30_OK;
}

/*********************************************************************
 * 函 数 名: sgp30_read
 * 入口参数: data  存放测量结果的指针(不得为 NULL)
 * 出口参数: data  写入 eCO2(ppm)与 TVOC(ppb)值
 * 返 回 值: sgp30_status_t  SGP30_OK=成功(数据已通过 CRC 校验)
 * 函数功能: 发送 Measure_air_quality 命令并解析返回的 eCO2 与 TVOC 值
 * 说    明: 读回 6 字节 = eCO2(2) + CRC(1) + TVOC(2) + CRC(1), 大端序;
 *           任一段 CRC 校验失败都返回错误, 不更新结果
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 移植(ESP-IDF -> AT32F403A)
 **********************************************************************/
sgp30_status_t sgp30_read(sgp30_data_t *data)
{
  uint8_t buf[6] = {0U};

  if((s_initialized == 0U) || (data == NULL))
  {
    return SGP30_ERR;
  }

  /* ---- Step1: 发送测量命令 ---- */
  if(sgp30_send_command(SGP30_CMD_MEASURE_AIR_QUALITY) != SGP30_OK)
  {
    return SGP30_ERR;
  }

  /* ---- Step2: 等待传感器完成测量（测量周期约 12ms） ---- */
  delay_ms(SGP30_MEASURE_WAIT_MS);

  /* ---- Step3: 读取 6 字节返回数据: eCO2(2) + CRC(1) + TVOC(2) + CRC(1) ---- */
  if(sgp30_read_bytes(buf, sizeof(buf)) != SGP30_OK)
  {
    return SGP30_ERR;
  }

  /* ---- Step4: CRC 校验(两段独立校验, 任一失败即丢弃本次结果) ---- */
  if(sgp30_crc8(buf, 2U) != buf[2])
  {
    return SGP30_ERR;
  }
  if(sgp30_crc8(&buf[3], 2U) != buf[5])
  {
    return SGP30_ERR;
  }

  /* ---- Step5: 解析大端数据 ---- */
  data->eco2 = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
  data->tvoc = (uint16_t)(((uint16_t)buf[3] << 8) | buf[4]);

  return SGP30_OK;
}
