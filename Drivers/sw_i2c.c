/**
  **************************************************************************
  * @file     sw_i2c.c
  * @brief    软件模拟 I2C 主机实现（SCL=PA2 / SDA=PA3, 约 100kHz）
  *
  * 时序说明（标准 I2C）：
  *   - 起始: SCL 高期间 SDA 下降沿;  结束: SCL 高期间 SDA 上升沿;
  *   - 数据: SCL 低电平期间允许改变 SDA, SCL 高电平期间保持稳定并采样;
  *   - 应答: 第 9 个时钟从机拉低 SDA 表示 ACK, 释放为高表示 NACK;
  *   - 半周期 5us(DWT 忙等) -> SCL 约 100kHz, 满足 SGP30 400kHz 以内要求。
  **************************************************************************
  */

#include <stddef.h>
#include "sw_i2c.h"
#include "delay.h"

/* I2C 半周期(us): 5us 半周期约对应 100kHz SCL */
#define SW_I2C_HALF_PERIOD_US   5U

/* 引脚电平操作宏(开漏: 置 1 = 释放总线交给上拉, 清 0 = 主动拉低) */
#define SW_I2C_SCL_HIGH()       gpio_bits_set(SW_I2C_SCL_PORT, SW_I2C_SCL_PIN)
#define SW_I2C_SCL_LOW()        gpio_bits_reset(SW_I2C_SCL_PORT, SW_I2C_SCL_PIN)
#define SW_I2C_SDA_HIGH()       gpio_bits_set(SW_I2C_SDA_PORT, SW_I2C_SDA_PIN)
#define SW_I2C_SDA_LOW()        gpio_bits_reset(SW_I2C_SDA_PORT, SW_I2C_SDA_PIN)
#define SW_I2C_SDA_READ()       ((gpio_input_data_bit_read(SW_I2C_SDA_PORT, SW_I2C_SDA_PIN) != RESET) ? 1U : 0U)

/*********************************************************************
 * 函 数 名: sw_i2c_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 使能 GPIOA 时钟, 将 PA2/PA3 配置为开漏输出并释放总线
 * 说    明: 内部上拉在输出模式下不起作用(F1 型 GPIO 架构), 总线高电平
 *           依赖 SGP30 模块板载上拉电阻
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求)
 **********************************************************************/
void sw_i2c_init(void)
{
  gpio_init_type gpio_init_struct;

  /* ---- Step1: 使能 GPIOA 时钟(LCD 驱动已使能, 重复使能无害) ---- */
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);

  /* ---- Step2: PA2/PA3 配置为开漏输出(模拟 I2C 的"线与"结构) ---- */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins           = SW_I2C_SCL_PIN | SW_I2C_SDA_PIN;
  gpio_init_struct.gpio_mode           = GPIO_MODE_OUTPUT;
  gpio_init_struct.gpio_out_type       = GPIO_OUTPUT_OPEN_DRAIN;
  gpio_init_struct.gpio_pull           = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(SW_I2C_SCL_PORT, &gpio_init_struct);

  /* ---- Step3: 释放总线(两线置高, 进入空闲状态) ---- */
  SW_I2C_SCL_HIGH();
  SW_I2C_SDA_HIGH();
  delay_us(SW_I2C_HALF_PERIOD_US);
}

/*********************************************************************
 * 函 数 名: sw_i2c_bus_idle_ok
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: uint8_t  1=两线均读到高(上拉正常) 0=有线被拉低(接线/上拉异常)
 * 函数功能: 检测总线空闲电平, 用于通信前快速诊断
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求)
 **********************************************************************/
uint8_t sw_i2c_bus_idle_ok(void)
{
  return ((SW_I2C_SDA_READ() != 0U)
          && (gpio_input_data_bit_read(SW_I2C_SCL_PORT, SW_I2C_SCL_PIN) != RESET)) ? 1U : 0U;
}

/*********************************************************************
 * 函 数 名: sw_i2c_start
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 产生 I2C 起始条件(SCL 高期间 SDA 下降沿)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求)
 **********************************************************************/
static void sw_i2c_start(void)
{
  SW_I2C_SDA_HIGH();
  SW_I2C_SCL_HIGH();
  delay_us(SW_I2C_HALF_PERIOD_US);
  SW_I2C_SDA_LOW();                              /* SCL 高期间 SDA 拉低 -> 起始 */
  delay_us(SW_I2C_HALF_PERIOD_US);
  SW_I2C_SCL_LOW();                              /* 钳住总线, 准备传输数据 */
  delay_us(SW_I2C_HALF_PERIOD_US);
}

/*********************************************************************
 * 函 数 名: sw_i2c_stop
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 产生 I2C 停止条件(SCL 高期间 SDA 上升沿)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求)
 **********************************************************************/
static void sw_i2c_stop(void)
{
  SW_I2C_SDA_LOW();
  delay_us(SW_I2C_HALF_PERIOD_US);
  SW_I2C_SCL_HIGH();
  delay_us(SW_I2C_HALF_PERIOD_US);
  SW_I2C_SDA_HIGH();                             /* SCL 高期间 SDA 释放变高 -> 停止 */
  delay_us(SW_I2C_HALF_PERIOD_US);
}

/*********************************************************************
 * 函 数 名: sw_i2c_write_byte
 * 入口参数: byte    待发送字节(MSB 在前)
 * 出口参数: 无
 * 返 回 值: uint8_t  0=从机 ACK  1=从机 NACK
 * 函数功能: 发送一个字节并采样第 9 个时钟的应答位
 * 说    明: SDA 仅在 SCL 低电平期间改变, 保证时序合规
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求)
 **********************************************************************/
static uint8_t sw_i2c_write_byte(uint8_t byte)
{
  uint8_t i;
  uint8_t ack;

  /* ---- Step1: SCL 低电平期间逐位移出 8 位数据(MSB 在前) ---- */
  for(i = 0U; i < 8U; i++)
  {
    if((byte & 0x80U) != 0U)
    {
      SW_I2C_SDA_HIGH();
    }
    else
    {
      SW_I2C_SDA_LOW();
    }
    delay_us(SW_I2C_HALF_PERIOD_US);
    SW_I2C_SCL_HIGH();
    delay_us(SW_I2C_HALF_PERIOD_US);
    SW_I2C_SCL_LOW();
    byte = (uint8_t)(byte << 1);
  }

  /* ---- Step2: 释放 SDA, 第 9 个时钟高电平期间采样 ACK ---- */
  SW_I2C_SDA_HIGH();
  delay_us(SW_I2C_HALF_PERIOD_US);
  SW_I2C_SCL_HIGH();
  delay_us(SW_I2C_HALF_PERIOD_US);
  ack = SW_I2C_SDA_READ();                       /* 0=ACK 1=NACK */
  SW_I2C_SCL_LOW();
  delay_us(SW_I2C_HALF_PERIOD_US);

  return ack;
}

/*********************************************************************
 * 函 数 名: sw_i2c_read_byte
 * 入口参数: send_nack  1=读完本字节后回复 NACK(末字节) 0=回复 ACK
 * 出口参数: 无
 * 返 回 值: uint8_t 读到的字节(MSB 在前)
 * 函数功能: 从总线读回一个字节并发送应答位
 * 说    明: 读之前释放 SDA(置 1), SCL 高电平中心采样从机驱动的数据
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求)
 **********************************************************************/
static uint8_t sw_i2c_read_byte(uint8_t send_nack)
{
  uint8_t i;
  uint8_t byte = 0U;

  /* ---- Step1: 释放 SDA 交由从机驱动, SCL 上升沿采样 8 位 ---- */
  SW_I2C_SDA_HIGH();
  for(i = 0U; i < 8U; i++)
  {
    SW_I2C_SCL_HIGH();
    delay_us(SW_I2C_HALF_PERIOD_US);
    byte = (uint8_t)(((uint32_t)byte << 1) | SW_I2C_SDA_READ());
    SW_I2C_SCL_LOW();
    delay_us(SW_I2C_HALF_PERIOD_US);
  }

  /* ---- Step2: 第 9 个时钟回复 ACK/NACK(主机驱动 SDA) ---- */
  if(send_nack != 0U)
  {
    SW_I2C_SDA_HIGH();
  }
  else
  {
    SW_I2C_SDA_LOW();
  }
  delay_us(SW_I2C_HALF_PERIOD_US);
  SW_I2C_SCL_HIGH();
  delay_us(SW_I2C_HALF_PERIOD_US);
  SW_I2C_SCL_LOW();
  delay_us(SW_I2C_HALF_PERIOD_US);
  SW_I2C_SDA_HIGH();                             /* 应答后释放 SDA */

  return byte;
}

/*********************************************************************
 * 函 数 名: sw_i2c_write
 * 入口参数: dev_addr  从机 7 位地址(函数内部左移补读写位)
 *           buf      待写数据缓冲区(NULL 且 len=0 允许, 用于纯地址探测)
 *           len      待写字节数
 * 出口参数: 无
 * 返 回 值: uint8_t  0=成功  1=失败(地址或数据 NACK)
 * 函数功能: I2C 写事务: START + 地址(写) + 数据 + STOP
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求)
 **********************************************************************/
uint8_t sw_i2c_write(uint8_t dev_addr, const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  if((buf == NULL) && (len != 0U))
  {
    return 1U;
  }

  /* ---- Step1: 起始 + 器件地址(写), 无应答直接失败退出 ---- */
  sw_i2c_start();
  if(sw_i2c_write_byte((uint8_t)(dev_addr << 1)) != 0U)
  {
    sw_i2c_stop();
    return 1U;
  }

  /* ---- Step2: 逐字节写入, 任一字节 NACK 即终止 ---- */
  for(i = 0U; i < len; i++)
  {
    if(sw_i2c_write_byte(buf[i]) != 0U)
    {
      sw_i2c_stop();
      return 1U;
    }
  }

  sw_i2c_stop();
  return 0U;
}

/*********************************************************************
 * 函 数 名: sw_i2c_read
 * 入口参数: dev_addr  从机 7 位地址(函数内部左移补读写位)
 *           buf      读出数据存放缓冲区
 *           len      读字节数(须大于 0)
 * 出口参数: buf      存放读到的数据
 * 返 回 值: uint8_t  0=成功  1=失败(地址 NACK 或参数错误)
 * 函数功能: I2C 读事务: START + 地址(读) + 数据(末字节 NACK) + STOP
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求)
 **********************************************************************/
uint8_t sw_i2c_read(uint8_t dev_addr, uint8_t *buf, uint16_t len)
{
  uint16_t i;

  if((buf == NULL) || (len == 0U))
  {
    return 1U;
  }

  /* ---- Step1: 起始 + 器件地址(读), 无应答直接失败退出 ---- */
  sw_i2c_start();
  if(sw_i2c_write_byte((uint8_t)(((uint32_t)dev_addr << 1) | 1U)) != 0U)
  {
    sw_i2c_stop();
    return 1U;
  }

  /* ---- Step2: 逐字节读回, 末字节回复 NACK 通知从机结束 ---- */
  for(i = 0U; i < len; i++)
  {
    buf[i] = sw_i2c_read_byte((uint8_t)((i + 1U == len) ? 1U : 0U));
  }

  sw_i2c_stop();
  return 0U;
}
