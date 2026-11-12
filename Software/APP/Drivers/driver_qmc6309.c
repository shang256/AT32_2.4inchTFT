/**
  **************************************************************************
  * @file     driver_qmc6309.c
  * @brief    QMC6309 三轴地磁传感器驱动实现（软件模拟 I2C, SCL=PA2 / SDA=PA3）
  *
  * 参考 QST QMC6309 数据手册(loongsoldier Rust crate 寄存器定义), 采用
  * 软件 I2C 主机模式通信：
  *   - 设备地址：0x1A（与 SGP30/AHT20/BMP280/MAX30102 共享总线）
  *   - Chip ID 寄存器：0x00, 期望值 0x90
  *   - 软复位：Ctrl2(0x0B) bit7 写 1 后必须手动写 0 清除
  *   - 配置：Ctrl1(0x0A)=0x63 连续模式 + 过采样 + LPF;
  *           Ctrl2(0x0B)=0x28 ODR=50Hz + ±8 高斯量程
  *   - 数据寄存器：0x01~0x06 连续 6 字节(X/Y/Z 各 16 位有符号, 小端)
  *   - 状态寄存器：0x09 bit0=DRDY(读后自清) bit1=OVFL bit4=NVM 加载完成
  *
  * 方位角计算：全定点实现(无浮点, 与工程风格一致)：
  *   - 简单硬铁校准：运行时持续跟踪 X/Y 的 min/max, 减去中心偏移;
  *   - CORDIC 16 级迭代 atan2(角度查表, 0.1° 单位), 象限预处理到
  *     [-45°, +45°] 后迭代, 输出归一化到 0~3599(北=0, 顺时针递增);
  *   - 无倾角补偿(纯磁力计无加速度计), 假设设备水平使用。
  **************************************************************************
  */

#include <stddef.h>
#include <stdint.h>
#include "driver_qmc6309.h"
#include "driver_sw_i2c.h"
#include "driver_delay.h"

/** QMC6309 7 位从机地址 */
#define QMC6309_I2C_ADDR         0x1AU
/** 芯片 ID 寄存器 */
#define QMC6309_REG_CHIP_ID      0x00U
/** 期望芯片 ID 值 */
#define QMC6309_CHIP_ID_VAL      0x90U
/** 数据起始寄存器（X/Y/Z 连续 6 字节, 小端有符号） */
#define QMC6309_REG_DATA         0x01U
/** 状态寄存器: bit0 DRDY / bit1 OVFL / bit4 NVM 加载完成 */
#define QMC6309_REG_STATUS       0x09U
/** 控制寄存器 1: 工作模式/过采样/LPF */
#define QMC6309_REG_CTRL1        0x0AU
/** 控制寄存器 2: 软复位/ODR/量程/Set-Reset */
#define QMC6309_REG_CTRL2        0x0BU
/** 控制寄存器 3: bit0 自检使能 */
#define QMC6309_REG_CTRL3        0x0EU

/** Ctrl1: 连续模式(MODE=11) + 过采样 + LPF(带宽设置 8) */
#define QMC6309_CTRL1_VAL        0x63U
/** Ctrl2: ODR=50Hz + 量程 ±8 高斯(灵敏度最高) */
#define QMC6309_CTRL2_VAL        0x28U
/** Ctrl2 软复位位(bit7, 写 1 复位后须写 0 清除) */
#define QMC6309_CTRL2_SOFT_RST   0x80U

/** 状态寄存器 bit 掩码 */
#define QMC6309_ST_DRDY          0x01U
#define QMC6309_ST_OVFL          0x02U
#define QMC6309_ST_NVM_RDY       0x10U

/** 方位角人工校准偏移(0.1° 单位): 用于修正传感器轴向/安装方向偏差, 默认 0 */
#define QMC6309_HEADING_OFFSET   0

/** 硬铁校准有效所需的最小 X/Y 采集幅度(LSB), 达到前直接用原始值 */
#define QMC6309_CAL_SPAN_MIN     300

/** CORDIC atan 迭代角度表(0.1° 单位): atan(2^-i)*10, i=0~15 */
static const uint16_t s_cordic_atan_tab[16] =
{
  4500U, 2657U, 1406U, 713U, 358U, 179U, 90U, 45U,
  22U,   11U,   6U,    3U,   1U,   1U,   0U,  0U
};

/** 模块是否已初始化 */
static uint8_t s_initialized = 0U;

/* 硬铁校准跟踪量(X/Y 轴 min/max, 上电后旋转数圈即可收敛) */
static int16_t s_min_x;                       /*!< X 轴历史最小值 */
static int16_t s_max_x;                       /*!< X 轴历史最大值 */
static int16_t s_min_y;                       /*!< Y 轴历史最小值 */
static int16_t s_max_y;                       /*!< Y 轴历史最大值 */

/*********************************************************************
 * 函 数 名: qmc6309_write_reg
 * 入口参数: reg    寄存器地址
 *           val    写入值
 * 出口参数: 无
 * 返 回 值: qmc6309_status_t  QMC6309_OK=成功
 * 函数功能: 写单个寄存器（地址 + 1 字节数据）
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(QMC6309 驱动需求)
 **********************************************************************/
static qmc6309_status_t qmc6309_write_reg(uint8_t reg, uint8_t val)
{
  const uint8_t buf[2] = { reg, val };

  if(sw_i2c_write(QMC6309_I2C_ADDR, buf, 2U) != 0U)
  {
    return QMC6309_ERR;
  }
  return QMC6309_OK;
}

/*********************************************************************
 * 函 数 名: qmc6309_read_regs
 * 入口参数: reg    起始寄存器地址
 *           buf    读出数据缓冲区
 *           len    读字节数
 * 出口参数: buf    存放读到的数据
 * 返 回 值: qmc6309_status_t  QMC6309_OK=成功
 * 函数功能: 从指定寄存器起连续读取（写寄存器地址 + Repeated START 读）
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(QMC6309 驱动需求)
 **********************************************************************/
static qmc6309_status_t qmc6309_read_regs(uint8_t reg, uint8_t *buf, uint16_t len)
{
  if(sw_i2c_write_then_read(QMC6309_I2C_ADDR, reg, buf, len) != 0U)
  {
    return QMC6309_ERR;
  }
  return QMC6309_OK;
}

/*********************************************************************
 * 函 数 名: qmc6309_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: qmc6309_status_t  QMC6309_OK=成功
 * 函数功能: 校验芯片 ID、软复位后配置连续模式(50Hz/±8G)
 * 说    明: 1. 先读芯片 ID 做在线判定, 未接/地址错直接返回失败;
 *           2. Ctrl2 bit7 软复位为自锁位, 复位后须写 0 清除;
 *           3. 复位后轮询状态寄存器 bit4 等待 NVM 校准数据加载完成,
 *              超时(约 20ms)则按配置继续(部分批次该位行为不一致);
 *           4. 本函数在 i2c_sensor_task 上下文调用, 内部毫秒级忙等
 *              只发生在上电/热插拔重试时刻, 不影响稳态调度。
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(QMC6309 驱动需求)
 **********************************************************************/
qmc6309_status_t qmc6309_init(void)
{
  uint8_t id = 0U;
  uint8_t status = 0U;
  uint8_t retry;

  /* Step1: 读芯片 ID, 验证器件在线 */
  if(qmc6309_read_regs(QMC6309_REG_CHIP_ID, &id, 1U) != QMC6309_OK)
  {
    return QMC6309_ERR;
  }
  if(id != QMC6309_CHIP_ID_VAL)
  {
    return QMC6309_ERR;
  }

  /* Step2: 软复位(Ctrl2 bit7 写 1), 复位位自锁需手动清 0 */
  (void)qmc6309_write_reg(QMC6309_REG_CTRL2, QMC6309_CTRL2_SOFT_RST);
  delay_ms(2U);
  (void)qmc6309_write_reg(QMC6309_REG_CTRL2, 0x00U);
  delay_ms(2U);

  /* Step3: 轮询状态寄存器 bit4 等待 NVM 加载完成(最多约 20ms) */
  for(retry = 0U; retry < 20U; retry++)
  {
    if(qmc6309_read_regs(QMC6309_REG_STATUS, &status, 1U) != QMC6309_OK)
    {
      return QMC6309_ERR;
    }
    if((status & QMC6309_ST_NVM_RDY) != 0U)
    {
      break;
    }
    delay_ms(1U);
  }

  /* Step4: 配置连续模式 + 50Hz ODR + ±8G 量程 */
  if(qmc6309_write_reg(QMC6309_REG_CTRL1, QMC6309_CTRL1_VAL) != QMC6309_OK)
  {
    return QMC6309_ERR;
  }
  if(qmc6309_write_reg(QMC6309_REG_CTRL2, QMC6309_CTRL2_VAL) != QMC6309_OK)
  {
    return QMC6309_ERR;
  }

  /* Step5: 初始化硬铁校准跟踪量, 标记初始化完成 */
  s_min_x = 0;
  s_max_x = 0;
  s_min_y = 0;
  s_max_y = 0;
  s_initialized = 1U;
  return QMC6309_OK;
}

/*********************************************************************
 * 函 数 名: qmc6309_read
 * 入口参数: data   三轴数据输出指针
 * 出口参数: data   X/Y/Z 原始值(有符号 LSB)
 * 返 回 值: qmc6309_status_t  QMC6309_OK=成功(含新数据)
 * 函数功能: 检查 DRDY 后读取三轴原始磁场值
 * 说    明: 1. 状态寄存器 bit0 DRDY 读后自动清零, 因此先读状态
 *              判断、再读数据, 顺序不可颠倒;
 *           2. DRDY=0(50Hz 输出 vs 500ms 轮询, 必有新数据)或
 *              OVFL=1(量程溢出)均返回失败, 由上层重试;
 *           3. 数据小端(低字节在前), 拼接为 16 位有符号。
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(QMC6309 驱动需求)
 **********************************************************************/
qmc6309_status_t qmc6309_read(qmc6309_data_t *data)
{
  uint8_t status = 0U;
  uint8_t raw[6];

  if((data == NULL) || (s_initialized == 0U))
  {
    return QMC6309_ERR;
  }

  /* Step1: 读状态寄存器(顺带清 DRDY), 检查新数据就绪且未溢出 */
  if(qmc6309_read_regs(QMC6309_REG_STATUS, &status, 1U) != QMC6309_OK)
  {
    return QMC6309_ERR;
  }
  if(((status & QMC6309_ST_DRDY) == 0U) || ((status & QMC6309_ST_OVFL) != 0U))
  {
    return QMC6309_ERR;
  }

  /* Step2: 连续读 6 字节数据, 小端拼接为有符号 16 位 */
  if(qmc6309_read_regs(QMC6309_REG_DATA, raw, 6U) != QMC6309_OK)
  {
    return QMC6309_ERR;
  }
  data->x = (int16_t)(((uint16_t)raw[0]) | ((uint16_t)raw[1] << 8));
  data->y = (int16_t)(((uint16_t)raw[2]) | ((uint16_t)raw[3] << 8));
  data->z = (int16_t)(((uint16_t)raw[4]) | ((uint16_t)raw[5] << 8));
  return QMC6309_OK;
}

/*********************************************************************
 * 函 数 名: qmc6309_cordic_atan2
 * 入口参数: y   纵坐标(定点整数)
 *           x   横坐标(定点整数)
 * 出口参数: 无
 * 返 回 值: int32_t  atan2(y, x), 0.1° 单位, 范围 -1800~1800
 * 函数功能: CORDIC 矢量模式定点 atan2(16 级迭代, 无浮点)
 * 说    明: 1. 先做象限预处理: x<0 时双轴取反并记 180°; y>x 时
 *              旋转 90° 把向量压进 [-45°,+45°] 收敛区间;
 *           2. 每级迭代仅做移位加减, 累加 atan(2^-i) 查表值;
 *           3. 旋转模式只取角度, 向量模的增益(约 1.647)不影响结果。
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(QMC6309 驱动需求)
 **********************************************************************/
static int32_t qmc6309_cordic_atan2(int32_t y, int32_t x)
{
  int32_t angle = 0;
  int32_t tx;
  uint8_t i;

  /* Step1: 原点特判 */
  if((x == 0) && (y == 0))
  {
    return 0;
  }

  /* Step2: x<0 时双轴取反, 角度补偿 180°(之后 x>=0) */
  if(x < 0)
  {
    x = -x;
    y = -y;
    angle += 1800;
  }

  /* Step3: y>x 时旋转 -90°(x<-y 处理后向量落入 ±45° 收敛域) */
  if(y > x)
  {
    tx = x;
    x = -y;
    y = tx;
    angle += 900;
  }

  /* Step4: 16 级 CORDIC 迭代(矢量模式, y 驱动向 0 收敛) */
  for(i = 0U; i < 16U; i++)
  {
    tx = x;
    if(y > 0)
    {
      x = x + (y >> i);
      y = y - (tx >> i);
      angle += (int32_t)s_cordic_atan_tab[i];
    }
    else
    {
      x = x - (y >> i);
      y = y + (tx >> i);
      angle -= (int32_t)s_cordic_atan_tab[i];
    }
  }
  return angle;
}

/*********************************************************************
 * 函 数 名: qmc6309_heading
 * 入口参数: data   三轴原始数据(qmc6309_read 的输出)
 * 出口参数: 无
 * 返 回 值: int32_t  方位角(0.1° 单位, 0~3599, 北=0 顺时针递增)
 * 函数功能: 计算水平放置时的磁航向角(硬铁校准 + CORDIC atan2)
 * 说    明: 1. 硬铁校准: 持续跟踪 X/Y 的 min/max, 减去中心偏移;
 *              上电后水平旋转数圈即可收敛, 收敛前直接用原始值;
 *           2. heading = atan2(y_corrected, x_corrected), 即 X 轴
 *              指向北时输出 0°; 安装方向差异用宏
 *              QMC6309_HEADING_OFFSET 修正(0.1° 单位);
 *           3. 结果归一化到 [0, 3599], 供 UI 表盘直接显示。
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(QMC6309 驱动需求)
 **********************************************************************/
int32_t qmc6309_heading(const qmc6309_data_t *data)
{
  int32_t cx;
  int32_t cy;
  int32_t heading;

  if(data == NULL)
  {
    return 0;
  }

  /* Step1: 更新硬铁校准 min/max 跟踪量 */
  if(data->x < s_min_x)
  {
    s_min_x = data->x;
  }
  if(data->x > s_max_x)
  {
    s_max_x = data->x;
  }
  if(data->y < s_min_y)
  {
    s_min_y = data->y;
  }
  if(data->y > s_max_y)
  {
    s_max_y = data->y;
  }

  /* Step2: 去中心偏移(采集幅度足够才启用校准值, 否则用原始值) */
  if(((int32_t)s_max_x - (int32_t)s_min_x) > QMC6309_CAL_SPAN_MIN)
  {
    cx = (int32_t)data->x - (((int32_t)s_max_x + (int32_t)s_min_x) / 2);
  }
  else
  {
    cx = (int32_t)data->x;
  }
  if(((int32_t)s_max_y - (int32_t)s_min_y) > QMC6309_CAL_SPAN_MIN)
  {
    cy = (int32_t)data->y - (((int32_t)s_max_y + (int32_t)s_min_y) / 2);
  }
  else
  {
    cy = (int32_t)data->y;
  }

  /* Step3: CORDIC atan2 求角 + 人工偏移修正 */
  heading = qmc6309_cordic_atan2(cy, cx) + QMC6309_HEADING_OFFSET;

  /* Step4: 归一化到 0~3599(0.1° 单位) */
  while(heading < 0)
  {
    heading += 3600;
  }
  while(heading >= 3600)
  {
    heading -= 3600;
  }
  return heading;
}
