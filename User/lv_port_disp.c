/**
  **************************************************************************
  * @file     lv_port_disp.c
  * @brief    LVGL v8 显示适配层实现（ST7789V 240x240, 硬件 SPI1）
  *
  * 实现说明：
  *   使用单块 1/4 屏尺寸的绘制缓冲区（240x60x2 = 28.8KB）。LVGL 将控件
  *   绘制到该缓冲区后调用 flush 回调，适配层通过 LCD_WriteAreaBytes 将
  *   缓冲区内容按 RGB565 字节流（高字节在前，LV_COLOR_16_SWAP=1 保证）
  *   直接写入 ST7789V 显存窗口。flush 采用同步阻塞方式（SPI 无 DMA），
  *   写完立即调用 lv_disp_flush_ready 通知 LVGL 缓冲区可复用。
  **************************************************************************
  */

#include "lv_port_disp.h"
#include "lcd_st7789.h"

/* 显示缓冲区: 一行 240 像素 x 60 行, RGB565 每像素 2 字节 */
#define DISP_BUF_LINES  60

/* LVGL 绘制缓冲区与显示驱动描述符（LVGL 要求全局有效, 不能放栈上） */
static lv_disp_draw_buf_t s_draw_buf;                        // LVGL 绘制缓冲区描述符
static lv_color_t        s_buf1[240 * DISP_BUF_LINES];       // 绘制缓冲区1(28.8KB)
static lv_disp_drv_t     s_disp_drv;                         // LVGL 显示驱动描述符

/*********************************************************************
 * 函 数 名: disp_flush
 * 入口参数: disp        LVGL 显示驱动描述符指针
 *           area        待刷新区域(x1/y1/x2/y2 为像素坐标)
 *           color_p     绘制好的像素数据首地址(RGB565, 已字节序交换)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: LVGL flush 回调: 将绘制缓冲区内容写入 ST7789V 指定窗口
 * 说    明: LV_COLOR_16_SWAP=1 时缓冲区内像素已按"高字节在前"排列,
 *           直接按字节流顺序发送即可; 全屏最大 115200 字节约 31ms
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 新增函数
 **********************************************************************/
static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  /* ---- Step1: 把绘制缓冲区内容按字节流写入屏幕指定窗口 ---- */
  LCD_WriteAreaBytes((uint16_t)area->x1, (uint16_t)area->y1,
                     (uint16_t)area->x2, (uint16_t)area->y2,
                     (const uint8_t *)color_p,
                     (uint32_t)((uint32_t)(area->x2 - area->x1 + 1)
                                * (uint32_t)(area->y2 - area->y1 + 1) * 2));

  /* ---- Step2: 通知 LVGL 缓冲区已用完, 可继续绘制下一区域 ---- */
  lv_disp_flush_ready(disp);
}

/*********************************************************************
 * 函 数 名: lv_port_disp_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 初始化 LVGL 显示适配层: 注册绘制缓冲区与显示驱动
 * 说    明: 必须在 lv_init() 之后调用; SPI 与屏幕由 LCD_Init 完成初始化
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月13日
 *     修改内容: 新增函数
 **********************************************************************/
void lv_port_disp_init(void)
{
  /* ---- Step1: 注册绘制缓冲区(240x60, 单缓冲, 屏幕的 1/4) ---- */
  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, NULL, 240 * DISP_BUF_LINES);

  /* ---- Step2: 配置显示驱动: 240x240 分辨率, 绑定 flush 回调 ---- */
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res  = 240;                     /* 水平分辨率 */
  s_disp_drv.ver_res  = 240;                     /* 垂直分辨率 */
  s_disp_drv.draw_buf = &s_draw_buf;             /* 绘制缓冲区 */
  s_disp_drv.flush_cb = disp_flush;              /* 刷新回调 */
  s_disp_drv.full_refresh = 0;                   /* 局部刷新, 降低 SPI 传输量 */

  /* ---- Step3: 注册显示驱动到 LVGL ---- */
  lv_disp_drv_register(&s_disp_drv);
}
