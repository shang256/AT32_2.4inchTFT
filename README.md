# AT32F403ACGU7 + ST7789V 240x240 LVGL + FreeRTOS 演示工程

基于 **Keil MDK5 (ARM Compiler 6)** 的 **FreeRTOS + LVGL v8.3** 交互演示工程（AT32F403ACGU7 @ 240MHz，硬件 SPI1 60MHz 驱动 ST7789V）。**旋转编码器**联动 LVGL 界面与 **PWM 背光调光**，屏幕右下角实时显示 FPS。

工程已编译验证通过（0 Error / 0 Warning），HEX 位于 `Project\Objects\AT32F403A_LCD_Demo.hex`。

## 三层架构

```
AT32F403ACGU7/
├── Project/
│   └── AT32F403A_LCD_Demo.uvprojx   # Keil5 工程（分组与目录一一对应）
├── Application/                       # 应用层：业务与任务编排
│   ├── app_main.c/h                  # main 入口、时钟配置、FreeRTOS 任务创建
│   └── app_ui.c/h                    # 编码器联动演示 UI（LVGL 交互逻辑）
├── Middleware/                        # 中间层：操作系统与图形库的配置/适配
│   ├── freertos/FreeRTOSConfig.h     # FreeRTOS 配置（240MHz/1kHz tick/20KB 堆）
│   └── lvgl/                         # LVGL 配置与移植
│       ├── lv_conf.h                 # 图形库裁剪配置
│       ├── lv_rtos_tick.h            # LVGL 心跳桥接（FreeRTOS.h → task.h 包含顺序）
│       └── lv_port_disp.c/h          # 显示适配（绘制缓冲 + flush 回调）
├── Drivers/                           # 驱动层：外设驱动
│   ├── lcd_st7789.c/h                # ST7789V 屏幕驱动（SPI1）
│   ├── backlight.c/h                 # 背光 PWM 驱动（TMR3_CH4 @ PB1）
│   ├── encoder.c/h                   # 旋转编码器驱动（EXINT 四倍频 + 按键消抖）
│   ├── delay.c/h                     # DWT 忙等延时（初始化阶段专用）
│   └── at32f403a_407_it.c/h          # 中断向量分发（SysTick/EXINT）
├── Libraries/                         # 第三方库
│   ├── CMSIS/ + Drivers/             # 雅特力官方固件库（含 tmr/exint）
│   ├── FreeRTOS/                     # FreeRTOS v202212.01 内核（GCC/ARM_CM4F 端口）
│   └── lvgl/                         # LVGL v8.3.0 源码（含官方 demos）
└── MDK/startup_at323a_407.s          # 官方启动文件（栈 4KB）
```

**分层依赖规则**：`Application → Middleware → Drivers → Libraries`，单向依赖；
驱动层互不调用，应用层不直接操作寄存器，LVGL 接口只允许在 LVGL 任务中访问。

## 任务划分（FreeRTOS）

| 任务 | 优先级 | 栈 | 职责 |
|------|-------|-----|------|
| `encoder_task` | 3 | 1KB | 20ms 轮询编码器驱动，将旋转/按键事件经**FreeRTOS 队列**投递（不触碰 LVGL） |
| `lvgl_task` | 2 | 4KB | **LVGL 刷图专用线程**：5ms 周期执行 `lv_timer_handler()`，从队列取编码器事件驱动 UI |
| idle（空闲） | 0 | 512B | FreeRTOS 内置空闲任务 |

线程安全设计：LVGL 非线程安全，所有 UI 访问收敛在 `lvgl_task`；编码器事件经队列跨线程传递（`app_encoder_msg_t`），`encoder_task` 只与驱动层和队列交互。

## 硬件连接

| 外设引脚 | MCU 引脚 | 说明 |
|---------|---------|------|
| LCD BLK（背光） | PB1 | **TMR3_CH4 PWM 1kHz 调光**，占空比 20%~100%（默认映射无需 remap） |
| LCD CS / DC / RES | PB0 / PA4 / PA6 | 推挽输出 |
| LCD SCL / SDA | PA5 / PA7 | SPI1_SCK / SPI1_MOSI（60MHz） |
| ENC PUSH | PB4 | 编码器按键（低电平按下，内部上拉 + 计数消抖） |
| ENC CH_A | PB3 | 编码器 A 相（**JTAG 释放后作 GPIO**，外部上拉 3.3V） |
| ENC CH_B | PB5 | 编码器 B 相（外部上拉 3.3V） |

> 编码器解码方式：**TMR7 1ms 定时中断同步采样 + 状态表四倍频解码**（相比 EXINT 双边沿中断，同步采样天然滤除 EC11 机械触点抖动，不丢步不乱跳）。界面右上角有 **"A:x B:x P:x"** 实时电平自检行：空闲时应显示 `A:1 B:1 P:1`（上拉 3.3V），旋转/按下时对应数字实时翻转；若恒定不变，说明信号未进入 MCU（检查接线与编码器供电）。

> 注意：PB3/PB4 复位后默认为 JTAG（JTDO/NJTRST），`encoder_init` 通过 `SWJTAG_MUX_010` 切换为仅 SWD 模式释放这两个引脚，**SWD 调试不受影响**（请勿使用 JTAG 调试）。

## 演示效果

上电后界面显示：标题、大号亮度百分比文字、背光亮度条（20%~100%）、翻转按钮：

- **旋转编码器**：调节背光亮度（每格 ±2%），**UI 亮度条、百分比文字与 TMR3 PWM 实际亮度实时同步**
- **点按编码器**：按钮状态翻转——ON 态蓝色高亮 "BTN: ON"，OFF 态灰色暗显 "BTN: OFF"
- 屏幕右下角显示 **FPS + CPU 占用率**（FreeRTOS 空闲率换算）

## 关键驱动接口

```c
/* backlight.h —— 背光 PWM */
void backlight_init(void);                    /* 初始化 TMR3_CH4 PWM（默认 100%） */
void backlight_set_percent(uint8_t percent);  /* 设置亮度 20~100（自动限幅） */

/* encoder.h —— 编码器 */
void encoder_init(void);                      /* 初始化引脚与 EXINT（自动释放 JTAG） */
int32_t encoder_get_delta(void);              /* 读取并清零旋转增量（4 倍频计步） */
uint8_t encoder_push_pressed(void);           /* 按键状态（30ms 消抖）：1=按下 */
uint8_t encoder_push_rising_edge(void);       /* "松开→按下"沿事件（读后清零） */

/* lcd_st7789.h —— 屏幕 */
void LCD_Init(void);                          /* SPI1 + 复位 + 寄存器初始化 */
void LCD_WriteAreaBytes(...);                 /* 窗口写 RGB565 字节流（LVGL flush 使用） */
```

## FreeRTOS / LVGL 移植要点

- **SysTick 接管**：`SysTick_Handler` 调用 `xPortSysTickHandler`；PendSV/SVC 经 `FreeRTOSConfig.h` 宏重定向到 port.c 实现
- **LVGL 心跳**：`LV_TICK_CUSTOM=1` 直接取 `xTaskGetTickCount()`（RTOS tick 1ms），无需任何 `lv_tick_inc` 调用；包含顺序经 `lv_rtos_tick.h` 桥接
- **堆**：FreeRTOS heap_4 20KB（任务栈/队列），LVGL TLSF 32KB（UI 对象），绘制缓冲 240x40（18.8KB）
- **RAM 预算**：总计约 77KB / 96KB；Flash 约 160KB / 1MB

## 使用步骤

1. 双击 `Project\AT32F403A_LCD_Demo.uvprojx` 打开（依赖 `ArteryTek.AT32F403A_407_DFP` 设备包）
2. `F7` 编译 → 生成 HEX → 烧录（**须使用 SWD 模式调试器**，JTAG 已被编码器占用）
3. 上电即见交互界面，旋转/按下编码器体验联动效果

## 常见问题

| 现象 | 处理 |
|------|------|
| 编码器完全无反应 | 确认驱动已使能 `CRM_IOMUX_PERIPH_CLOCK`（SWJTAG 释放依赖它，缺失时 PB3/PB4 保持 JTAG 功能） |
| **A 相恒定不变（B/P 正常）** | 典型的 **SWO 调试探针残留占用 PB3**：调试器开启过 Trace 后 TPIU 配置在复位后仍残留。驱动已在初始化时复位调试跟踪块（关-开 TRCENA）清除 SWO；若需边连调试器边用编码器，请在 Keil `Debug → Settings → Trace` 中取消勾选 Trace |
| 编码器方向相反 | 交换模组 CH_A/CH_B 接线，或在 `encoder.c` 解码表中取反 |
| 按键无效 | 确认 PUSH 按下为低电平（内部上拉已启用）；模组若高有效需改 `encoder.c` 判定 |
| 背光闪烁/偏低 | 低于 20% 的亮度被驱动限幅（防止闪烁），范围可改 `backlight.h` 宏 |
| 无法 JTAG 调试 | 正常现象：PB3/PB4 已释放为编码器，请改用 SWD |
