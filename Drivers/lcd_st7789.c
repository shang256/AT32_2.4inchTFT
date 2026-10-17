/**
  **************************************************************************
  * @file     lcd_st7789.c
  * @brief    ST7789V 240x240 LCD 驱动实现（AT32F403ACGU7 + 硬件 SPI1）
  *
  * 实现内容：
  *   1. GPIO 初始化（DC/RES/CS/BLK 控制脚 + SPI1 复用脚 SCL/SDA）
  *   2. SPI1 主机初始化（8bit、MSB 先行、模式 3：CPOL=1/CPHA=2Edge）
  *   3. 硬件复位时序与 ST7789V 上电初始化寄存器序列
  *   4. 显示窗口设置、全屏清色与区域填充（像素流 DMA1 通道3 搬运, 命令轮询）
  *
  * 性能说明：
  *   全屏 240x240x16bit 共 115200 字节。时钟配置为 240MHz 主频时
  *   APB2=120MHz，SPI1 二分频后 60MHz，一次全屏填充约 15.5ms（约 64 帧/秒）。
  *   LVGL 刷新像素流由 DMA1 通道3(SPI1_TX) 硬件搬运, CPU 不再逐字节
  *   轮询 TDBE, 刷新时 CPU 占用显著下降(传输总时长仍由 SPI 时钟决定)。
  *   若接线较长导致显示异常（花屏/错位），可将 SPI_MCLK_DIV_2 改回
  *   SPI_MCLK_DIV_4（30MHz）以提升信号完整性。
  **************************************************************************
  */

#include "lcd_st7789.h"
#include "delay.h"

/* ---------------- 底层 SPI/控制脚操作（内部函数） ---------------- */

/**
  * @brief  等待 SPI1 发送彻底完成（移位寄存器空闲）
  * @note   DC 引脚切换前必须调用，确保最后一个字节已完整移出，
  *         否则命令/数据边界会错乱
  */
static void lcd_spi_wait_done(void)
{
  while(spi_i2s_flag_get(LCD_SPI, SPI_I2S_BF_FLAG) != RESET);
}

/**
  * @brief  通过 SPI1 发送单个字节（仅等待发送缓冲区空）
  */
static void lcd_spi_write_byte(uint8_t data)
{
  /* 等待发送数据缓冲区空（上一字节已装入移位寄存器即可继续写入） */
  while(spi_i2s_flag_get(LCD_SPI, SPI_I2S_TDBE_FLAG) == RESET);
  spi_i2s_data_transmit(LCD_SPI, data);
}

/**
 * @brief  用 DMA1 通道3（固定映射 SPI1_TX）将内存字节流搬运到 SPI1 数据寄存器
 * @param  data：像素字节流首地址
 * @param  len ：字节数（本工程单块绘制缓冲 240*40*2=19200 字节, 远小于 65535）
 * @note   本工程 DMA 驱动源文件未加入工程, 故直接操作寄存器结构（见
 *         at32f403a_407_dma.h 的 dma_channel_type）; 相比逐字节轮询 TDBE,
 *         硬件 DMA 自动响应 TDBE 请求搬运, CPU 只需启动后等计数归零;
 *         计数归零仅表示字节已写入 SPI 数据寄存器, 末字节可能仍在移位
 *         寄存器中, 故末尾仍需等 BF 标志才能安全切换 DC
 */
static void lcd_spi_write_bytes_dma(const uint8_t *data, uint32_t len)
{
  dma_channel_type *dma_ch = DMA1_CHANNEL3;

  /* ---- Step1: 先停通道再配置(内存 -> SPI1_TX, 8bit 宽度) ---- */
  dma_ch->ctrl_bit.chen   = 0;                      /* 停通道, 避免配置期间误触发 */
  dma_ch->paddr           = (uint32_t)&LCD_SPI->dt; /* 外设地址 = SPI1 数据寄存器 */
  dma_ch->maddr           = (uint32_t)data;         /* 内存源地址 */
  dma_ch->dtcnt           = (uint16_t)len;          /* 传输字节数 */
  dma_ch->ctrl_bit.dtd    = 1;                      /* 方向: 内存 -> 外设 */
  dma_ch->ctrl_bit.pincm  = 0;                      /* 外设地址固定 */
  dma_ch->ctrl_bit.mincm  = 1;                      /* 内存地址自增 */
  dma_ch->ctrl_bit.pwidth = 0;                      /* 外设数据宽度 8bit */
  dma_ch->ctrl_bit.mwidth = 0;                      /* 内存数据宽度 8bit */
  dma_ch->ctrl_bit.lm     = 0;                      /* 非循环模式 */
  dma_ch->ctrl_bit.m2m    = 0;                      /* 非内存到内存 */
  dma_ch->ctrl_bit.chpl   = 1;                      /* 中优先级 */

  /* ---- Step2: 清完成标志, 使能 SPI1 发送 DMA 后再启动 DMA 通道 ---- */
  DMA1->clr = DMA1_FDT3_FLAG;
  spi_i2s_dma_transmitter_enable(LCD_SPI, TRUE);
  dma_ch->ctrl_bit.chen = 1;

  /* ---- Step3: 等待 DMA 计数归零(所有字节已写入 SPI 数据寄存器) ---- */
  while(dma_ch->dtcnt != 0);

  /* ---- Step4: 关 DMA, 等 SPI 移位寄存器发完末字节(后续切 DC 前必须) ---- */
  dma_ch->ctrl_bit.chen = 0;
  spi_i2s_dma_transmitter_enable(LCD_SPI, FALSE);
  lcd_spi_wait_done();
}

/**
  * @brief  LCD 写命令（DC 拉低后发送命令字节）
  */
void LCD_WriteCmd(uint8_t cmd)
{
  /* 等待之前的数据流发送完毕，再切换 DC 电平，避免边界错乱 */
  lcd_spi_wait_done();
  gpio_bits_reset(LCD_DC_PORT, LCD_DC_PIN);       /* DC=0：命令模式 */
  lcd_spi_write_byte(cmd);
}

/**
  * @brief  LCD 写单字节数据（DC 拉高后发送）
  */
void LCD_WriteData8(uint8_t data)
{
  lcd_spi_wait_done();
  gpio_bits_set(LCD_DC_PORT, LCD_DC_PIN);         /* DC=1：数据模式 */
  lcd_spi_write_byte(data);
}

/**
  * @brief  LCD 写 16bit 数据（高字节在前，用于 RGB565 像素）
  */
void LCD_WriteData16(uint16_t data)
{
  lcd_spi_wait_done();
  gpio_bits_set(LCD_DC_PORT, LCD_DC_PIN);         /* DC=1：数据模式 */
  lcd_spi_write_byte((uint8_t)(data >> 8));       /* 先发高字节 */
  lcd_spi_write_byte((uint8_t)(data & 0xFF));     /* 再发低字节 */
}

/* ---------------- 初始化部分 ---------------- */

/**
  * @brief  初始化 LCD 相关 GPIO 与 SPI1 外设
  * @note   PA5/PA7 配置为复用推挽输出（SPI1_SCK/SPI1_MOSI 默认映射，
  *         无需 remap）；PA6（RES）与 SPI1_MISO 引脚复用，本工程仅作
  *         普通推挽输出用于复位控制，SPI 只发不收，两者无冲突
  */
static void lcd_gpio_spi_init(void)
{
  gpio_init_type gpio_init_struct;
  spi_init_type  spi_init_struct;

  /* 使能 GPIOA、GPIOB、SPI1 与 DMA1 外设时钟(DMA1 用于 SPI1_TX 刷新搬运) */
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_SPI1_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);

  /* 控制脚统一默认参数：推挽输出、无上下拉、强驱动能力 */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_mode           = GPIO_MODE_OUTPUT;
  gpio_init_struct.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull           = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;

  /* DC=PA4、RES=PA6 */
  gpio_init_struct.gpio_pins = LCD_DC_PIN | LCD_RES_PIN;
  gpio_init(LCD_DC_PORT, &gpio_init_struct);

  /* CS=PB0（背光 BLK=PB1 已移交 backlight 驱动做 TMR3_CH4 PWM 调光） */
  gpio_init_struct.gpio_pins = LCD_CS_PIN;
  gpio_init(LCD_CS_PORT, &gpio_init_struct);

  /* SPI1 引脚：SCL=PA5（SPI1_SCK）、SDA=PA7（SPI1_MOSI），
     默认映射无需调用 gpio_pin_remap_config */
  gpio_init_struct.gpio_pins = GPIO_PINS_5 | GPIO_PINS_7;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init(GPIOA, &gpio_init_struct);

  /* SPI1 主机模式初始化 */
  spi_default_para_init(&spi_init_struct);
  spi_init_struct.transmission_mode      = SPI_TRANSMIT_FULL_DUPLEX;
  spi_init_struct.master_slave_mode      = SPI_MODE_MASTER;
  spi_init_struct.mclk_freq_division     = SPI_MCLK_DIV_2;        /* 120MHz/2=60MHz */
  spi_init_struct.first_bit_transmission = SPI_FIRST_BIT_MSB;
  spi_init_struct.frame_bit_num          = SPI_FRAME_8BIT;
  spi_init_struct.clock_polarity         = SPI_CLOCK_POLARITY_HIGH;  /* CPOL=1 */
  spi_init_struct.clock_phase            = SPI_CLOCK_PHASE_2EDGE;    /* CPHA=2Edge → SPI 模式 3 */
  spi_init_struct.cs_mode_selection      = SPI_CS_SOFTWARE_MODE;    /* 片选用 GPIO 手动控制 */
  spi_init(LCD_SPI, &spi_init_struct);

  /* 片选拉低选中（整次刷新期间保持低电平，避免反复拉高增加开销） */
  gpio_bits_reset(LCD_CS_PORT, LCD_CS_PIN);

  spi_enable(LCD_SPI, TRUE);
}

/**
  * @brief  ST7789V 硬件复位时序
  * @note   RES 拉低保持 10ms 以上再拉高，等待 120ms 让芯片完成内部复位
  */
static void lcd_hard_reset(void)
{
  gpio_bits_reset(LCD_RES_PORT, LCD_RES_PIN);    /* RES=0，进入复位 */
  delay_ms(20);
  gpio_bits_set(LCD_RES_PORT, LCD_RES_PIN);      /* RES=1，释放复位 */
  delay_ms(120);                                 /* 等待芯片内部上电复位完成 */
}

/**
  * @brief  ST7789V 上电初始化寄存器序列（240x240 模组通用配置）
  */
static void lcd_reg_init(void)
{
  LCD_WriteCmd(LCD_CMD_SLPOUT);                  /* 0x11：退出睡眠模式 */
  delay_ms(120);                                 /* 数据手册要求退出睡眠后等待 120ms */

  LCD_WriteCmd(0xB2);                            /* PORCTRL： porch 控制 */
  LCD_WriteData8(0x0C);
  LCD_WriteData8(0x0C);
  LCD_WriteData8(0x00);
  LCD_WriteData8(0x33);
  LCD_WriteData8(0x33);

  LCD_WriteCmd(0xB7);                            /* GCTRL：栅极电压 VGH/VGL */
  LCD_WriteData8(0x35);

  LCD_WriteCmd(0xBB);                            /* VCOMS：VCOM 电压（约 0.68V） */
  LCD_WriteData8(0x19);

  LCD_WriteCmd(0xC0);                            /* LCMCTRL：伽马与极性反转控制 */
  LCD_WriteData8(0x2C);

  LCD_WriteCmd(0xC2);                            /* VDVVRHEN：使能 VDV/VRH 寄存器写入 */
  LCD_WriteData8(0x01);

  LCD_WriteCmd(0xC3);                            /* VRHS：VRH 电压（GVDD） */
  LCD_WriteData8(0x12);

  LCD_WriteCmd(0xC4);                            /* VDVS：VDV 电压 */
  LCD_WriteData8(0x20);

  LCD_WriteCmd(0xB6);                            /* FRCTRL2：帧率控制（119Hz） */
  LCD_WriteData8(0x0F);

  LCD_WriteCmd(0xD0);                            /* PWCTRL1：电源控制（AVDD/AVCL） */
  LCD_WriteData8(0xA4);
  LCD_WriteData8(0xA1);

  LCD_WriteCmd(LCD_CMD_MADCTL);                  /* 0x36：扫描方向，0x00=从左到右、从上到下 */
  LCD_WriteData8(0x00);

  LCD_WriteCmd(0x3A);                            /* COLMOD：像素格式 16bit RGB565 */
  LCD_WriteData8(0x55);

  LCD_WriteCmd(LCD_CMD_INVON);                   /* 0x21：开启反色（多数 240x240 IPS 模组需要，
                                                    若颜色相反可改为 LCD_CMD_INVOFF 0x20） */

  /* 预设全屏显示窗口（内部自动叠加 X/Y 偏移并按 16bit 拆分发送） */
  LCD_SetWindow(0, 0, LCD_W - 1, LCD_H - 1);

  LCD_WriteCmd(LCD_CMD_NORON);                   /* 0x13：普常显示模式 */
  delay_ms(10);

  LCD_WriteCmd(LCD_CMD_RAMWR);                   /* 0x2C：准备写显存，先刷一帧黑色避免花屏 */
  lcd_spi_wait_done();
  gpio_bits_set(LCD_DC_PORT, LCD_DC_PIN);
  {
    uint32_t i;
    for(i = 0; i < (uint32_t)LCD_W * LCD_H * 2; i++)
    {
      lcd_spi_write_byte(0x00);
    }
    lcd_spi_wait_done();
  }

  LCD_WriteCmd(LCD_CMD_DISPON);                  /* 0x29：开启显示 */
  delay_ms(50);
}

/**
  * @brief  LCD 总初始化入口（GPIO+SPI1 → 硬件复位 → 寄存器序列）
  * @note   调用前需完成系统时钟配置（delay_init 也依赖此时钟）
  */
void LCD_Init(void)
{
  lcd_gpio_spi_init();                           /* GPIO 与 SPI1 初始化，CS 已选中 */
  lcd_hard_reset();                              /* 硬件复位时序 */
  lcd_reg_init();                                /* 寄存器初始化序列 */
}

/* ---------------- 显示操作部分 ---------------- */

/**
  * @brief  设置显示窗口（后续写显存数据将从窗口左上角开始）
  * @param  x1,y1：窗口左上角坐标（0 ~ LCD_W-1 / 0 ~ LCD_H-1）
  * @param  x2,y2：窗口右下角坐标（自动叠加 LCD_X_OFFSET/LCD_Y_OFFSET）
  */
void LCD_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
  uint16_t sx1 = x1 + LCD_X_OFFSET;              /* 实际坐标 = 逻辑坐标 + 偏移 */
  uint16_t sy1 = y1 + LCD_Y_OFFSET;
  uint16_t sx2 = x2 + LCD_X_OFFSET;
  uint16_t sy2 = y2 + LCD_Y_OFFSET;

  LCD_WriteCmd(LCD_CMD_CASET);                   /* 0x2A：列地址窗口 */
  LCD_WriteData8((uint8_t)(sx1 >> 8));
  LCD_WriteData8((uint8_t)(sx1 & 0xFF));
  LCD_WriteData8((uint8_t)(sx2 >> 8));
  LCD_WriteData8((uint8_t)(sx2 & 0xFF));

  LCD_WriteCmd(LCD_CMD_RASET);                   /* 0x2B：行地址窗口 */
  LCD_WriteData8((uint8_t)(sy1 >> 8));
  LCD_WriteData8((uint8_t)(sy1 & 0xFF));
  LCD_WriteData8((uint8_t)(sy2 >> 8));
  LCD_WriteData8((uint8_t)(sy2 & 0xFF));
}

/**
  * @brief  区域填充单色（自动裁剪越界坐标）
  */
void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
  uint32_t size;
  uint32_t i;

  /* 坐标范围裁剪，防止 CASET/RASET 越界 */
  if(x1 >= LCD_W) x1 = LCD_W - 1;
  if(x2 >= LCD_W) x2 = LCD_W - 1;
  if(y1 >= LCD_H) y1 = LCD_H - 1;
  if(y2 >= LCD_H) y2 = LCD_H - 1;
  if(x1 > x2){ uint16_t t = x1; x1 = x2; x2 = t; }
  if(y1 > y2){ uint16_t t = y1; y1 = y2; y2 = t; }

  size = (uint32_t)(x2 - x1 + 1) * (y2 - y1 + 1);   /* 总像素数 */

  LCD_SetWindow(x1, y1, x2, y2);
  LCD_WriteCmd(LCD_CMD_RAMWR);                   /* 0x2C：开始写显存 */

  /* DC 拉高后连续发送像素流：内部仅等待发送缓冲区空（流水线方式），
     避免逐字节等待移位完成，提高刷新效率 */
  lcd_spi_wait_done();
  gpio_bits_set(LCD_DC_PORT, LCD_DC_PIN);
  for(i = 0; i < size; i++)
  {
    lcd_spi_write_byte((uint8_t)(color >> 8));   /* RGB565 高字节 */
    lcd_spi_write_byte((uint8_t)(color & 0xFF)); /* RGB565 低字节 */
  }
  lcd_spi_wait_done();                           /* 等最后字节发完，再允许后续命令切换 DC */
}

/**
  * @brief  全屏填充单色（LCD_Fill 的全屏特例）
  */
void LCD_Clear(uint16_t color)
{
  LCD_Fill(0, 0, LCD_W - 1, LCD_H - 1, color);
}

/*********************************************************************
 * 函 数 名: LCD_WriteAreaBytes
 * 入口参数: x1, y1      指定区域左上角坐标(0~239)
 *           x2, y2      指定区域右下角坐标(0~239)
 *           data        像素数据指针(RGB565, 高字节在前的字节流)
 *           len         数据总字节数(必须等于区域像素数x2)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 向指定区域写入原始 RGB565 字节流, 供 LVGL 的 flush 回调
 *           直接输出绘制缓冲区内容, 避免逐像素重新组包
 * 说    明: 依赖 lv_conf.h 中 LV_COLOR_16_SWAP=1, 保证缓冲区内像素
 *           本身已按"高字节在前"排列, 可按字节顺序直接发送;
 *           像素流由 DMA1 通道3(SPI1_TX) 硬件搬运, 命令/窗口仍用
 *           轮询发送(DC 切换需 SPI 空闲, 数据量小无 DMA 必要)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 新增函数, 支撑 LVGL 显示适配层
 *     修改日期: 2026年10月17日
 *     修改内容: 像素流由逐字节轮询改为 DMA1 通道3 搬运(DMA 刷新需求)
 **********************************************************************/
void LCD_WriteAreaBytes(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                        const uint8_t *data, uint32_t len)
{
  /* ---- Step1: 坐标范围裁剪, 防止 CASET/RASET 越界 ---- */
  if(x1 >= LCD_W){ x1 = LCD_W - 1; }
  if(x2 >= LCD_W){ x2 = LCD_W - 1; }
  if(y1 >= LCD_H){ y1 = LCD_H - 1; }
  if(y2 >= LCD_H){ y2 = LCD_H - 1; }

  /* ---- Step2: 设置显示窗口并发出写显存命令 ---- */
  LCD_SetWindow(x1, y1, x2, y2);
  LCD_WriteCmd(LCD_CMD_RAMWR);                  /* 0x2C: 开始写显存 */

  /* ---- Step3: DC 拉高后由 DMA1 搬运字节流(DMA 内已等末字节发完) ---- */
  lcd_spi_wait_done();
  gpio_bits_set(LCD_DC_PORT, LCD_DC_PIN);
  lcd_spi_write_bytes_dma(data, len);
}
