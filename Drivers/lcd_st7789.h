/**
  **************************************************************************
  * @file     lcd_st7789.h
  * @brief    ST7789V 240x240 LCD 驱动接口（硬件 SPI1 接口）
  *
  * 硬件连接（主控 AT32F403ACGU7）：
  *   CS  （片选） : PB0   推挽输出，低电平选中
  *   SDA （数据） : PA7   SPI1_MOSI（复用推挽）
  *   RES （复位） : PA6   推挽输出，低电平复位
  *   SCL （时钟） : PA5   SPI1_SCK（复用推挽）
  *   DC  （命令） : PA4   推挽输出，低=命令 高=数据
  *   BLK（背光） : PB1   已移交 backlight 驱动（TMR3_CH4 PWM 调光）
  *
  * ST7789V 为 320 行 GRAM，多数 240x240 模组从第 80 行开始显示，
  * 通过 LCD_Y_OFFSET 宏适配，若显示内容偏移或错位可调整该值。
  **************************************************************************
  */

#ifndef __LCD_ST7789_H
#define __LCD_ST7789_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/* ---------------- 屏幕参数配置（可按模组调整） ---------------- */

#define LCD_W                  240      /* 屏幕水平分辨率（像素） */
#define LCD_H                  240      /* 屏幕垂直分辨率（像素） */

#define LCD_X_OFFSET           0        /* 显示区 X 起始偏移（多数 240x240 模组为 0） */
#define LCD_Y_OFFSET           0//80       /* 显示区 Y 起始偏移（240x240 在 320 行 GRAM 中偏移 80） */

#define LCD_SPI                SPI1     /* 使用的外设：SPI1（SCL=PA5/SPI1_SCK，SDA=PA7/SPI1_MOSI） */

/* ---------------- 引脚定义 ---------------- */

#define LCD_DC_PORT            GPIOA    /* DC（数据/命令选择）端口 */
#define LCD_DC_PIN             GPIO_PINS_4

#define LCD_RES_PORT           GPIOA    /* RES（复位）端口 */
#define LCD_RES_PIN            GPIO_PINS_6

#define LCD_CS_PORT            GPIOB    /* CS（片选）端口 */
#define LCD_CS_PIN             GPIO_PINS_0

/* ---------------- ST7789V 常用命令 ---------------- */

#define LCD_CMD_SWRESET        0x01     /* 软件复位 */
#define LCD_CMD_SLPOUT         0x11     /* 退出睡眠模式 */
#define LCD_CMD_NORON          0x13     /* 普常显示模式开启 */
#define LCD_CMD_INVOFF         0x20     /* 关闭反色 */
#define LCD_CMD_INVON          0x21     /* 开启反色（多数 240x240 IPS 模组需要开启） */
#define LCD_CMD_DISPON         0x29     /* 显示开启 */
#define LCD_CMD_CASET          0x2A     /* 设置列地址窗口 */
#define LCD_CMD_RASET          0x2B     /* 设置行地址窗口 */
#define LCD_CMD_RAMWR          0x2C     /* 显存写入开始 */
#define LCD_CMD_MADCTL         0x36     /* 内存数据访问控制（扫描方向） */
#define LCD_CMD_COLMOD         0x3A     /* 像素格式设置 */

/* ---------------- 常用 RGB565 颜色定义 ---------------- */

#define COLOR_BLACK           0x0000   /* 黑色 */
#define COLOR_WHITE           0xFFFF   /* 白色 */
#define COLOR_RED             0xF800   /* 红色 */
#define COLOR_GREEN           0x07E0   /* 绿色 */
#define COLOR_BLUE            0x001F   /* 蓝色 */
#define COLOR_YELLOW          0xFFE0   /* 黄色 */
#define COLOR_CYAN            0x07FF   /* 青色 */
#define COLOR_MAGENTA         0xF81F   /* 洋红色 */

/* ---------------- 对外接口函数 ---------------- */

void LCD_Init(void);                                       /* LCD 初始化（GPIO+SPI1+复位+寄存器配置） */
void LCD_WriteCmd(uint8_t cmd);                            /* 写命令（DC=0） */
void LCD_WriteData8(uint8_t data);                         /* 写单字节数据（DC=1） */
void LCD_WriteData16(uint16_t data);                       /* 写 16bit 数据（高字节在前，DC=1） */
void LCD_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2); /* 设置显示窗口 */
void LCD_Clear(uint16_t color);                            /* 全屏填充单色 */
void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color); /* 区域填充单色 */
/* 背光已独立为 backlight 驱动（TMR3_CH4 PWM 调光, 见 backlight.h）,
   lcd_st7789 只负责显示部分 */

void LCD_WriteAreaBytes(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint8_t *data, uint32_t len); /* 窗口写原始字节流（RGB565 大端） */

#ifdef __cplusplus
}
#endif

#endif /* __LCD_ST7789_H */
