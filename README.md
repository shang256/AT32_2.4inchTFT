# AT32F403ACGU7 + ST7789V 240x240 LVGL 演示工程

基于 **Keil MDK5 (ARM Compiler 6)** 与**雅特力官方固件库**的 **LVGL v8.3 官方性能基准测试（lv_demo_benchmark）** 工程。使用**硬件 SPI1（60MHz）** 驱动 ST7789V，自动循环运行 LVGL 典型绘制场景（矩形/阴影/边框/文字/圆弧/图片混合/图片变换等），每个场景实时显示 FPS，全部结束后给出**汇总评分表格**。

工程已在本机编译验证通过（0 Error / 0 Warning），HEX 位于 `Project\Objects\AT32F403A_LCD_Demo.hex`。

## 引脚连接

| 屏幕引脚 | MCU 引脚 | 说明 |
|---------|---------|------|
| BLK（背光） | PB1 | 推挽输出，默认高电平点亮 |
| CS（片选） | PB0 | 推挽输出，低电平选中 |
| SDA（数据） | PA7 | SPI1_MOSI（复用推挽） |
| RES（复位） | PA6 | 推挽输出，低电平复位 |
| SCL（时钟） | PA5 | SPI1_SCK（复用推挽） |
| DC（数据/命令） | PA4 | 推挽输出，低=命令、高=数据 |

> 注意：PA6 同时是 SPI1_MISO 引脚，本工程 SPI 只发不收，PA6 仅作普通 GPIO 复位脚，两者无冲突。屏幕仅需 3.3V 供电与 GND，VCC 请勿接 5V（除非模组自带稳压）。

## 工程结构

```
AT32F403ACGU7/
├── Project/
│   └── AT32F403A_LCD_Demo.uvprojx   # Keil5 工程文件（AC6 编译器，生成 HEX）
├── User/
│   ├── main.c                       # 主程序：240MHz 时钟配置 + LVGL 动态演示界面
│   ├── lv_conf.h                    # LVGL 配置（RGB565 字节序交换 / 48KB 内存池 / FPS 监视器）
│   ├── lv_port_disp.c / .h          # LVGL 显示适配层（绘制缓冲区 + flush 回调）
│   ├── delay.c / delay.h            # SysTick 1ms 中断（LVGL 心跳）+ DWT 微秒延时
│   └── at32f403a_407_it.c / .h      # 中断服务函数（SysTick_Handler 提供 LVGL 心跳）
├── Drivers/
│   └── lcd_st7789.c / lcd_st7789.h  # ST7789V 驱动（含 LVGL 用的字节流接口）
├── Libraries/
│   ├── CMSIS/                       # 官方库：内核头 + 器件寄存器 + 系统时钟
│   ├── Drivers/                     # 官方库：crm / gpio / spi / misc 外设驱动
│   └── lvgl/                        # LVGL v8.3.0 源码（按 lv_conf.h 裁剪编译）
└── MDK/
    └── startup_at32f403a_407.s      # 官方启动文件（栈已扩至 4KB）
```

## 使用步骤

1. **打开工程**：双击 `Project\AT32F403A_LCD_Demo.uvprojx`。
   - 依赖设备包 `ArteryTek.AT32F403A_407_DFP`（本机已安装 2.2.3；若提示缺包，可在 Pack Installer 中安装，或到[雅特力官网](https://www.arterytek.com)下载）。
   - 编译器：ARM Compiler 6（MDK V6.16 验证通过；完整编译约 2 分钟）。
2. **编译**：`Project → Build Target`（F7），生成 `Project\Objects\AT32F403A_LCD_Demo.hex`。
3. **烧录**：连接调试器（CMSIS-DAP / J-Link 等）后按 LOAD 按钮下载，或使用 ISP 工具烧录 HEX。
   - 若使用 J-Link 等非 ULINK 调试器，在 `Options for Target → Debug` 选择对应调试器，并在 `Utilities` 页勾选 "Use Debug Driver"。
4. **现象**：上电后自动开始 benchmark 测试：每个场景显示约数秒，屏幕底部显示**场景名与实时 FPS**；全部场景结束后显示**汇总表格**（各场景 FPS 与平均帧率），停留片刻后自动重新循环。

## LVGL 移植说明

- **版本**：LVGL v8.3.0（`Libraries\lvgl`），按 `lv_conf.h` 裁剪：当前运行官方 **lv_demo_benchmark**（启用 arc/bar/btn/label/img/line/table 控件、flex 布局、默认深色主题、Montserrat 14/28 字体、48KB TLSF 内存池）
- **显示适配**：`lv_port_disp.c` 注册 240x60 单绘制缓冲区，flush 回调经 `LCD_WriteAreaBytes` 按 RGB565 字节流写入屏幕（`LV_COLOR_16_SWAP=1` 保证高字节在前，无需逐像素组包）
- **心跳来源**：SysTick 1ms 中断调用 `lv_tick_inc(1)`；主循环 `lv_timer_handler()` + 5ms 间隔轮询
- **帧率显示**：benchmark 每场景自带 FPS 测量与显示，结束时汇总；`LV_USE_PERF_MONITOR=1` 仍在右下角显示整体 FPS 与 CPU 占用
- **资源占用**：Flash 约 210KB（含 benchmark 图片与压缩字体）/ RAM 约 84KB（AT32F403ACGU7 为 1MB Flash + 96KB SRAM）

### 帧率说明

SPI1 时钟 60MHz 下，全屏（240x240x16bit）传输约 15.5ms；LVGL 刷新周期设为 16ms（`lv_conf.h` 中 `LV_DISP_DEF_REFR_PERIOD`），**FPS 显示上限约 62**。LVGL 采用**局部刷新**：动画每帧仅重绘变化区域（弧线/进度条/文本），实测帧率主要取决于脏区域面积。若追求更高帧率上限，可将 `LV_DISP_DEF_REFR_PERIOD` 进一步调小（如 10ms → 上限 100FPS），但小于单帧全屏耗时（15.5ms）时全屏重绘会掉帧。若接线较长出现花屏/错位，可将 `lcd_st7789.c` 中 `SPI_MCLK_DIV_2` 改回 `SPI_MCLK_DIV_4`（30MHz）。

## 系统时钟说明

`main.c` 中的 `system_clock_config()` 将系统时钟配置为 **240MHz**（HICK 8MHz 内部时钟 ÷2 经 PLL 60 倍频，无需外部晶振）：

- SCLK / AHB = 240MHz
- APB2（SPI1 时钟源）= 120MHz
- SPI1 波特率 = 120MHz ÷ 2 = **60MHz**（全屏填充约 15.5ms，≈64 帧/秒）

若接线较长导致显示异常（花屏/错位），可将 `lcd_st7789.c` 中的 `SPI_MCLK_DIV_2` 改回 `SPI_MCLK_DIV_4`（30MHz）以提升信号完整性。

## 常见问题调整（均在 `Drivers/lcd_st7789.h`）

| 现象 | 调整方法 |
|------|---------|
| 显示内容整体偏移（只显示左/上一部分，或出现镜像错位） | 修改 `LCD_X_OFFSET` / `LCD_Y_OFFSET`（默认 0 / 80，多数 240x240 模组为 0/80 或 0/0） |
| 颜色反相（红变青、白变黑） | 将 `lcd_st7789.c` 初始化中的 `LCD_CMD_INVON`（0x21）改为 `LCD_CMD_INVOFF`（0x20） |
| 背光不亮（或常亮无法关闭） | 修改 `LCD_BACKLIGHT_ON_LEVEL`：1=高电平点亮，0=低电平点亮 |
| 横竖屏 / 镜像方向不对 | 修改 `lcd_st7789.c` 中 MADCTL（0x36）参数：0x00 正常 / 0xC0 上下镜像 / 0x60 左右镜像 / 0xA0 旋转 180°（改 MADCTL 后建议同步调整偏移宏） |

## 关键 API

```c
void LCD_Init(void);              /* 初始化（GPIO+SPI1+复位+寄存器序列） */
void LCD_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2); /* 设置窗口 */
void LCD_Clear(uint16_t color);   /* 全屏填色 */
void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color); /* 区域填色 */
void LCD_SetBacklight(uint8_t on); /* 背光开关 */
```

RGB565 颜色宏：`COLOR_RED / COLOR_GREEN / COLOR_BLUE / COLOR_WHITE / COLOR_BLACK / COLOR_YELLOW / COLOR_CYAN / COLOR_MAGENTA`（定义见 `lcd_st7789.h`）。
