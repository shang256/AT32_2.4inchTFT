/**
  **************************************************************************
  * @file     driver_max30102.c
  * @brief    MAX30102 心率血氧传感器驱动实现（软件模拟 I2C, SCL=PA2 / SDA=PA3）
  *
  * 采样配置:
  *   - SpO2 模式(MODE=0x03): 红光/红外双 LED 交替;
  *   - ADC 量程 4096nA, 采样率 100sps, 脉宽 411us(18bit 分辨率);
  *   - SMP_AVE=2(片内 2 选 1 平均) -> FIFO 有效输出 50sps;
  *   - 任务 500ms 读一次 FIFO(25 样本 < FIFO 深度 32, 不溢出)。
  *
  * 信号处理(定点, 无浮点):
  *   - 手指检测: 最新红外样本 DC > 50000(18bit 满量程 262143 的 ~19%);
  *   - 心率: 4.8s 环形窗口红外信号去直流 -> 动态阈值(60% 峰高)上升沿
  *     峰检测 -> 相邻峰间隔均值换算 BPM(限幅 40~180, 有效拍数>=3 才输出);
  *   - 血氧: R = (ACred/DCred)/(ACir/DCir), SpO2 = 110 - 25R 经验公式
  *     (64 位整数除法, 限幅 70~100)。
  *
  * FIFO 溢出恢复: 任务被调度延迟导致 OVF 置位时, 读写指针强行同步
  * (RD=WR, OVF 清零), 丢弃一个周期数据后算法窗口继续填充, 不挂死。
  **************************************************************************
  */

#include <stddef.h>
#include "driver_max30102.h"
#include "driver_sw_i2c.h"
#include "driver_delay.h"

/* ---------------- 寄存器地址(7 位从机地址 0x57) ---------------- */
#define MAX30102_I2C_ADDR        0x57U               /* 从机地址 */
#define MAX30102_REG_INT_STS1    0x00U               /* 中断状态 1 */
#define MAX30102_REG_INT_EN1     0x02U               /* 中断使能 1 */
#define MAX30102_REG_FIFO_WR     0x04U               /* FIFO 写指针(5bit) */
#define MAX30102_REG_OVF_CNT     0x05U               /* FIFO 溢出计数 */
#define MAX30102_REG_FIFO_RD     0x06U               /* FIFO 读指针(5bit) */
#define MAX30102_REG_FIFO_DATA   0x07U               /* FIFO 数据口 */
#define MAX30102_REG_FIFO_CFG    0x08U               /* FIFO 配置 */
#define MAX30102_REG_MODE_CFG    0x09U               /* 模式配置 */
#define MAX30102_REG_SPO2_CFG    0x0AU               /* SpO2 配置 */
#define MAX30102_REG_LED1_PA     0x0CU               /* LED1(红光) 电流 */
#define MAX30102_REG_LED2_PA     0x0DU               /* LED2(红外) 电流 */
#define MAX30102_REG_PART_ID     0xFFU               /* Part ID(期望 0x15) */

/* ---------------- 配置值 ---------------- */
#define MAX30102_PART_ID_VAL     0x15U               /* 器件识别码 */
#define MAX30102_MODE_RESET      0x40U               /* 软复位位(MODE_CFG bit6, 写 1 自清) */
#define MAX30102_MODE_SPO2       0x03U               /* SpO2 模式(红+红外, MODE[1:0]=11) */
#define MAX30102_FIFO_CFG_VAL    0x2FU               /* SMP_AVE=2, 不滚动, A_FULL=15 */
#define MAX30102_SPO2_CFG_VAL    0x24U               /* 4096nA/100sps/411us(18bit) */
#define MAX30102_LED_PA_DEFAULT  0x30U               /* LED 电流约 9.6mA(PA*0.2mA) */
#define MAX30102_RESET_WAIT_MS   10U                 /* 软复位等待(ms) */
#define MAX30102_FINGER_IR_TH    50000U              /* 手指在位红外 DC 阈值(18bit) */
#define MAX30102_FIFO_DEPTH      32U                 /* FIFO 样本深度 */
#define MAX30102_SAMPLE_BYTES    6U                  /* 每样本字节数(红3+红外3) */
#define MAX30102_ADC_MASK        0x3FFFFU            /* 18bit ADC 有效位 */

/* ---------------- 信号处理参数 ---------------- */
#define HR_FS                    50U                 /* 有效采样率(FIFO 输出, sps) */
#define HR_BUF_SAMPLES           240U                /* 算法窗口 4.8s(240 样本) */
#define HR_MIN_AC_SWING          1500U               /* 有效脉搏交流幅度下限(ADC LSB) */
#define HR_TH_NUM                3U                  /* 动态阈值分子(3/5 = 60% 峰高) */
#define HR_TH_DEN                5U                  /* 动态阈值分母 */
#define HR_IV_MIN                16U                 /* 最小峰间隔样本数(约 187BPM) */
#define HR_IV_MAX                90U                 /* 最大峰间隔样本数(约 33BPM) */
#define HR_BEATS_MIN             3U                  /* 输出心率所需最少拍数 */
#define HR_BPM_MIN               40U                 /* 心率下限(BPM) */
#define HR_BPM_MAX               180U                /* 心率上限(BPM) */
#define SPO2_MIN                 70U                 /* 血氧下限(%) */
#define SPO2_MAX                 100U                /* 血氧上限(%) */
#define SPO2_AC_MIN              100U                /* 血氧计算交流幅度下限 */

/* ---------------- 模块状态 ---------------- */
static uint8_t s_initialized = 0U;                   /* 1=已初始化成功 */

/* 算法环形缓冲(静态分配, 240*4*2 ~= 1.9KB, 不占任务栈) */
static uint32_t s_ir_buf[HR_BUF_SAMPLES];            /* 红外通道窗口 */
static uint32_t s_red_buf[HR_BUF_SAMPLES];           /* 红光通道窗口 */
static uint16_t s_buf_head = 0U;                     /* 环形写位置 */
static uint16_t s_buf_count = 0U;                    /* 已填充样本数(<240 未出结果) */

/* 最近一次结果缓存(模块离线/无新数据时沿用上一拍) */
static max30102_result_t s_last_result;

/* FIFO 批量读暂存(最大 32 样本 x 6 字节) */
static uint8_t s_fifo_buf[MAX30102_FIFO_DEPTH * MAX30102_SAMPLE_BYTES];

/* 去直流中间缓冲(240x4B, 静态分配避免占用任务栈) */
static int32_t s_ac_ir[HR_BUF_SAMPLES];

/*********************************************************************
 * 函 数 名: max30102_wr_reg
 * 入口参数: reg   寄存器地址
 *           val   写入值
 * 出口参数: 无
 * 返 回 值: max30102_status_t MAX30102_OK=成功
 * 函数功能: 写 MAX30102 一个寄存器
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(MAX30102 驱动需求)
 **********************************************************************/
static max30102_status_t max30102_wr_reg(uint8_t reg, uint8_t val)
{
  const uint8_t buf[2] = { reg, val };

  if(sw_i2c_write(MAX30102_I2C_ADDR, buf, 2U) != 0U)
  {
    return MAX30102_ERR;
  }
  return MAX30102_OK;
}

/*********************************************************************
 * 函 数 名: max30102_rd_regs
 * 入口参数: reg   起始寄存器地址
 *           buf   读出缓冲
 *           len   连续读取字节数
 * 出口参数: buf   读出的寄存器值
 * 返 回 值: max30102_status_t MAX30102_OK=成功
 * 函数功能: 从 MAX30102 连续读寄存器(写寄存器指针 + Repeated START 读)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(MAX30102 驱动需求)
 **********************************************************************/
static max30102_status_t max30102_rd_regs(uint8_t reg, uint8_t *buf, uint8_t len)
{
  if(sw_i2c_write_then_read(MAX30102_I2C_ADDR, reg, buf, len) != 0U)
  {
    return MAX30102_ERR;
  }
  return MAX30102_OK;
}

/*********************************************************************
 * 函 数 名: max30102_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: max30102_status_t MAX30102_OK=成功
 * 函数功能: 初始化 MAX30102(验证 Part ID + 软复位 + 配置采样并启动)
 * 说    明: 调用方需持有 g_i2c_mutex; 失败可由任务周期重试;
 *           采样配置: 4096nA 量程/100sps/411us 脉宽, 片内 2 选 1
 *           平均后 FIFO 输出 50sps, 500ms 任务读 25 样本不溢出
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(MAX30102 驱动需求)
 **********************************************************************/
max30102_status_t max30102_init(void)
{
  uint8_t part_id = 0U;

  /* ---- Step1: 软件 I2C 就绪(幂等) ---- */
  sw_i2c_init();

  /* ---- Step2: 读 Part ID 验证器件在线(期望 0x15) ---- */
  if(max30102_rd_regs(MAX30102_REG_PART_ID, &part_id, 1U) != MAX30102_OK)
  {
    return MAX30102_ERR;
  }
  if(part_id != MAX30102_PART_ID_VAL)
  {
    return MAX30102_ERR;                              /* 总线有应答但不是 MAX30102 */
  }

  /* ---- Step3: 软复位并等待 ---- */
  if(max30102_wr_reg(MAX30102_REG_MODE_CFG, MAX30102_MODE_RESET) != MAX30102_OK)
  {
    return MAX30102_ERR;
  }
  delay_ms(MAX30102_RESET_WAIT_MS);

  /* ---- Step4: 清 FIFO 指针(写指针/溢出计数/读指针全部归零) ---- */
  if((max30102_wr_reg(MAX30102_REG_FIFO_WR, 0U) != MAX30102_OK)
     || (max30102_wr_reg(MAX30102_REG_OVF_CNT, 0U) != MAX30102_OK)
     || (max30102_wr_reg(MAX30102_REG_FIFO_RD, 0U) != MAX30102_OK))
  {
    return MAX30102_ERR;
  }

  /* ---- Step5: FIFO 配置(SMP_AVE=2)/SpO2 配置/LED 电流 ---- */
  if((max30102_wr_reg(MAX30102_REG_FIFO_CFG, MAX30102_FIFO_CFG_VAL) != MAX30102_OK)
     || (max30102_wr_reg(MAX30102_REG_SPO2_CFG, MAX30102_SPO2_CFG_VAL) != MAX30102_OK)
     || (max30102_wr_reg(MAX30102_REG_LED1_PA, MAX30102_LED_PA_DEFAULT) != MAX30102_OK)
     || (max30102_wr_reg(MAX30102_REG_LED2_PA, MAX30102_LED_PA_DEFAULT) != MAX30102_OK))
  {
    return MAX30102_ERR;
  }

  /* ---- Step6: 算法缓冲复位并启动 SpO2 采样 ---- */
  s_buf_head = 0U;
  s_buf_count = 0U;
  s_last_result.valid = 0U;
  s_last_result.finger_on = 0U;
  s_last_result.hr_bpm = 0U;
  s_last_result.spo2 = 0U;
  if(max30102_wr_reg(MAX30102_REG_MODE_CFG, MAX30102_MODE_SPO2) != MAX30102_OK)
  {
    return MAX30102_ERR;
  }

  s_initialized = 1U;
  return MAX30102_OK;
}

/*********************************************************************
 * 函 数 名: max30102_fetch_fifo
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: max30102_status_t MAX30102_OK=成功(含无新样本)
 * 函数功能: 批量读取 FIFO 新样本并填入算法环形缓冲
 * 说    明: 读 0x04~0x06 三个连续寄存器得写指针/溢出/读指针;
 *           溢出时读写指针强行同步丢弃一个周期; 每样本 6 字节,
 *           前 3 字节 LED1(红光), 后 3 字节 LED2(红外), 均 18bit
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(MAX30102 驱动需求)
 **********************************************************************/
static max30102_status_t max30102_fetch_fifo(void)
{
  uint8_t  ptrs[3] = { 0U, 0U, 0U };                  /* WR / OVF / RD */
  uint8_t  wr;                                        /* 写指针 */
  uint8_t  ovf;                                       /* 溢出计数 */
  uint8_t  rd;                                        /* 读指针 */
  uint8_t  num;                                       /* 可读样本数 */
  uint8_t  i;                                         /* 样本循环索引 */

  /* ---- Step1: 读三个指针寄存器 ---- */
  if(max30102_rd_regs(MAX30102_REG_FIFO_WR, ptrs, 3U) != MAX30102_OK)
  {
    return MAX30102_ERR;
  }
  wr = ptrs[0] & 0x1FU;
  ovf = ptrs[1] & 0x1FU;
  rd = ptrs[2] & 0x1FU;

  /* ---- Step2: 溢出恢复(任务延迟导致): RD=WR 同步, 丢一个周期 ---- */
  if(ovf != 0U)
  {
    (void)max30102_wr_reg(MAX30102_REG_OVF_CNT, 0U);
    (void)max30102_wr_reg(MAX30102_REG_FIFO_RD, wr);
    return MAX30102_OK;
  }

  /* ---- Step3: 计算可读样本数(模 32) ---- */
  num = (uint8_t)((wr - rd) & 0x1FU);
  if(num == 0U)
  {
    return MAX30102_OK;                               /* 无新样本 */
  }

  /* ---- Step4: 一次性读出 num x 6 字节 ---- */
  if(max30102_rd_regs(MAX30102_REG_FIFO_DATA, s_fifo_buf,
                      (uint8_t)(num * MAX30102_SAMPLE_BYTES)) != MAX30102_OK)
  {
    return MAX30102_ERR;
  }

  /* ---- Step5: 逐样本解析入环形缓冲(18bit, 前=红光 后=红外) ---- */
  for(i = 0U; i < num; i++)
  {
    uint16_t base = (uint16_t)(i * MAX30102_SAMPLE_BYTES);

    s_red_buf[s_buf_head] = (((uint32_t)s_fifo_buf[base] << 16)
                            | ((uint32_t)s_fifo_buf[base + 1U] << 8)
                            |  (uint32_t)s_fifo_buf[base + 2U]) & MAX30102_ADC_MASK;
    s_ir_buf[s_buf_head]  = (((uint32_t)s_fifo_buf[base + 3U] << 16)
                            | ((uint32_t)s_fifo_buf[base + 4U] << 8)
                            |  (uint32_t)s_fifo_buf[base + 5U]) & MAX30102_ADC_MASK;
    s_buf_head = (uint16_t)((s_buf_head + 1U) % HR_BUF_SAMPLES);
    if(s_buf_count < HR_BUF_SAMPLES)
    {
      s_buf_count++;
    }
  }
  return MAX30102_OK;
}

/*********************************************************************
 * 函 数 名: max30102_compute
 * 入口参数: latest_ir   最新红外原始值(手指检测用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 4.8s 窗口信号处理: 手指检测/心率峰检测/血氧比值
 * 说    明: 窗口未填满(前 4.8s)时仅更新手指状态不出数值;
 *           心率与血氧独立判定有效性, 互不影响
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(MAX30102 驱动需求)
 **********************************************************************/
static void max30102_compute(uint32_t latest_ir)
{
  uint32_t i;                                         /* 窗口循环索引 */
  uint32_t dc_ir = 0U;                                /* 红外均值 */
  uint32_t dc_red = 0U;                               /* 红光均值 */

  /* 手指检测: 红外 DC 高于阈值(手指覆盖时红外吸收强, DC 大) */
  s_last_result.finger_on = (latest_ir > MAX30102_FINGER_IR_TH) ? 1U : 0U;

  /* ---- Step1: 窗口未填满 -> 暂无心率/血氧输出 ---- */
  if(s_buf_count < HR_BUF_SAMPLES)
  {
    return;
  }

  /* ---- Step2: 求双通道均值(DC) ---- */
  for(i = 0U; i < HR_BUF_SAMPLES; i++)
  {
    dc_ir += s_ir_buf[i];
    dc_red += s_red_buf[i];
  }
  dc_ir /= HR_BUF_SAMPLES;
  dc_red /= HR_BUF_SAMPLES;

  /* ---- Step3: 红外去直流 + 求交流极值 ---- */
  {
    int32_t  max_ac = 0;
    int32_t  min_ac = 0;
    int32_t  thr;                                     /* 动态检测阈值 */
    uint8_t  above = 0U;                              /* 1=当前处于阈值上方 */
    int32_t  peak_val = 0;                            /* 当前峰跟踪值 */
    int32_t  peak_pos = -1;                           /* 当前峰位置 */
    int32_t  last_peak_pos = -1;                      /* 上一确认峰位置 */
    uint32_t iv_sum = 0U;                             /* 有效峰间隔累计 */
    uint32_t iv_cnt = 0U;                             /* 有效峰间隔个数 */

    for(i = 0U; i < HR_BUF_SAMPLES; i++)
    {
      s_ac_ir[i] = (int32_t)s_ir_buf[i] - (int32_t)dc_ir;
      if(i == 0U)
      {
        max_ac = s_ac_ir[0];
        min_ac = s_ac_ir[0];
      }
      else
      {
        if(s_ac_ir[i] > max_ac)
        {
          max_ac = s_ac_ir[i];
        }
        if(s_ac_ir[i] < min_ac)
        {
          min_ac = s_ac_ir[i];
        }
      }
    }

    /* ---- Step4: 交流幅度不足 -> 信号弱, 清零输出 ---- */
    if((uint32_t)(max_ac - min_ac) < HR_MIN_AC_SWING)
    {
      s_last_result.hr_bpm = 0U;
      return;
    }

    /* ---- Step5: 动态阈值(60% 峰高)上升沿峰检测 ---- */
    thr = min_ac + ((max_ac - min_ac) * HR_TH_NUM / HR_TH_DEN);
    for(i = 0U; i < HR_BUF_SAMPLES; i++)
    {
      if(s_ac_ir[i] > thr)
      {
        if(above == 0U)                               /* 上升穿越: 进入新峰 */
        {
          above = 1U;
          peak_val = s_ac_ir[i];
          peak_pos = (int32_t)i;
        }
        else if(s_ac_ir[i] > peak_val)                /* 峰内继续跟踪最大值 */
        {
          peak_val = s_ac_ir[i];
          peak_pos = (int32_t)i;
        }
        else
        {
          /* 峰内下降段, 继续等待回落 */
        }
      }
      else
      {
        if(above != 0U)                               /* 回落穿出阈值: 确认一个峰 */
        {
          above = 0U;
          if(last_peak_pos >= 0)
          {
            int32_t iv = peak_pos - last_peak_pos;    /* 相邻峰间隔(样本) */
            if((iv >= HR_IV_MIN) && (iv <= HR_IV_MAX))
            {
              iv_sum += (uint32_t)iv;
              iv_cnt++;
            }
          }
          last_peak_pos = peak_pos;
        }
      }
    }

    /* ---- Step6: 有效拍数足够 -> 平均间隔换算 BPM(限幅) ---- */
    if(iv_cnt >= HR_BEATS_MIN)
    {
      uint32_t bpm = (60U * HR_FS * iv_cnt) / iv_sum; /* 60*fs/(平均间隔) */

      if(bpm < HR_BPM_MIN)
      {
        bpm = HR_BPM_MIN;
      }
      if(bpm > HR_BPM_MAX)
      {
        bpm = HR_BPM_MAX;
      }
      s_last_result.hr_bpm = (uint8_t)bpm;
    }
    else
    {
      s_last_result.hr_bpm = 0U;                      /* 拍数不足, 本拍不出结果 */
    }
  }

  /* ---- Step7: 血氧(仅手指在位且双通道交流足够时计算) ---- */
  if(s_last_result.finger_on != 0U)
  {
    int32_t  ac_r;                                    /* 红光交流幅度 */
    int32_t  ac_i;                                    /* 红外交流幅度 */
    uint32_t max_r = 0U;
    uint32_t min_r = 0U;
    int32_t  mx_i = 0;
    int32_t  mn_i = 0;

    for(i = 0U; i < HR_BUF_SAMPLES; i++)
    {
      if((i == 0U) || (s_red_buf[i] > max_r))
      {
        max_r = s_red_buf[i];
      }
      if((i == 0U) || (s_red_buf[i] < min_r))
      {
        min_r = s_red_buf[i];
      }
      if((i == 0U) || ((int32_t)s_ir_buf[i] > mx_i))
      {
        mx_i = (int32_t)s_ir_buf[i];
      }
      if((i == 0U) || ((int32_t)s_ir_buf[i] < mn_i))
      {
        mn_i = (int32_t)s_ir_buf[i];
      }
    }
    ac_r = (int32_t)(max_r - min_r);
    ac_i = mx_i - mn_i;

    if((ac_r > SPO2_AC_MIN) && (ac_i > SPO2_AC_MIN)
       && (dc_ir != 0U) && (dc_red != 0U))
    {
      /* R = (ACred/DCred)/(ACir/DCir), SpO2 = 110 - 25R (经验公式)
         64 位整数避免 18bit x 18bit 乘法溢出 */
      uint64_t r_num = (uint64_t)((uint32_t)ac_r) * dc_ir;
      uint64_t r_den = (uint64_t)((uint32_t)ac_i) * dc_red;
      uint32_t spo2 = 110U - (uint32_t)(25U * r_num / r_den);

      if(spo2 < SPO2_MIN)
      {
        spo2 = SPO2_MIN;
      }
      if(spo2 > SPO2_MAX)
      {
        spo2 = SPO2_MAX;
      }
      s_last_result.spo2 = (uint8_t)spo2;
    }
    else
    {
      s_last_result.spo2 = 0U;                        /* 信号不足 */
    }
  }
  else
  {
    s_last_result.spo2 = 0U;                          /* 无手指不出血氧 */
  }
}

/*********************************************************************
 * 函 数 名: max30102_read
 * 入口参数: result   结果输出指针(不得为 NULL)
 * 出口参数: *result  本次心率/血氧/手指状态
 * 返 回 值: max30102_status_t MAX30102_OK=本次通信成功
 * 函数功能: 周期读取 FIFO 新样本并输出最新心率血氧结果
 * 说    明: 调用方需持有 g_i2c_mutex; 建议周期 500ms(与 FIFO
 *           深度匹配); 通信失败时 valid=0 且沿用上次缓存值
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(MAX30102 驱动需求)
 **********************************************************************/
max30102_status_t max30102_read(max30102_result_t *result)
{
  uint32_t latest_ir;                                 /* 最新红外样本 */

  if((s_initialized == 0U) || (result == NULL))
  {
    if(result != NULL)
    {
      result->valid = 0U;
    }
    return MAX30102_ERR;
  }

  /* ---- Step1: 批量读 FIFO ---- */
  if(max30102_fetch_fifo() != MAX30102_OK)
  {
    s_last_result.valid = 0U;
    *result = s_last_result;
    return MAX30102_ERR;
  }

  /* ---- Step2: 取最新红外样本做手指检测并跑算法 ---- */
  latest_ir = (s_buf_count != 0U)
              ? s_ir_buf[(uint16_t)((s_buf_head + HR_BUF_SAMPLES - 1U) % HR_BUF_SAMPLES)]
              : 0U;
  s_last_result.valid = 1U;
  max30102_compute(latest_ir);

  /* ---- Step3: 输出结果 ---- */
  *result = s_last_result;
  return MAX30102_OK;
}
