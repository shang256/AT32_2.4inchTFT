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
│   ├── encoder.c/h                   # 旋转编码器驱动（TMR7 同步采样四倍频 + 长按 3s 关机判定）
│   ├── lowpower.c/h                  # 低功耗管理（STANDBY 待机 + WKUP 唤醒，寄存器级实现）
│   ├── sw_i2c.c/h                    # 软件模拟 I2C 主机（SCL=PA2 / SDA=PA3, 开漏 ~100kHz）
│   ├── sgp30.c/h                     # SGP30 空气质量传感器驱动（eCO2/TVOC, 移植自 ESP-IDF）
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
| `encoder_task` | 3 | 1KB | 20ms 轮询编码器驱动，将旋转/按键/长按进度事件经**FreeRTOS 队列**投递（不触碰 LVGL）；**长按 3s 执行关机序列**（提示 → 熄屏 → 等松手 → 进 STANDBY） |
| `lvgl_task` | 2 | 4KB | **LVGL 刷图专用线程**：5ms 周期执行 `lv_timer_handler()`，从队列取编码器/传感器事件驱动 UI |
| `sgp30_task` | 1 | 1KB | 1s 周期读取 SGP30 eCO2/TVOC，经队列投递 LVGL 线程显示（12ms 忙等可被高优先级任务抢占） |
| idle（空闲） | 0 | 512B | FreeRTOS 内置空闲任务 |

线程安全设计：LVGL 非线程安全，所有 UI 访问收敛在 `lvgl_task`；编码器事件经队列跨线程传递（`app_encoder_msg_t`），传感器数据经队列跨线程传递（`app_sensor_msg_t`），生产者任务只与驱动层和队列交互。

## 硬件连接

| 外设引脚 | MCU 引脚 | 说明 |
|---------|---------|------|
| LCD BLK（背光） | PB1 | **TMR3_CH4 PWM 1kHz 调光**，占空比 20%~100%（默认映射无需 remap） |
| LCD CS / DC / RES | PB0 / PA4 / PA6 | 推挽输出 |
| LCD SCL / SDA | PA5 / PA7 | SPI1_SCK / SPI1_MOSI（60MHz） |
| ENC PUSH | PB3 | 编码器按键（低有效：按键一脚接 GND，内部上拉），**兼作停机唤醒 EXINT3**（按下产生下降沿唤醒） |
| ENC CH_A | PB4 | 编码器 A 相（**JTAG 释放后作 GPIO**，外部上拉 3.3V） |
| ENC CH_B | PB5 | 编码器 B 相（外部上拉 3.3V） |
| SGP30 SCL | PA2 | 软件 I2C 时钟（开漏输出，依赖模块板载上拉） |
| SGP30 SDA | PA3 | 软件 I2C 数据（开漏输出，依赖模块板载上拉） |

> 编码器解码方式：**TMR7 1ms 定时中断同步采样 + 状态表四倍频解码**（相比 EXINT 双边沿中断，同步采样天然滤除 EC11 机械触点抖动，不丢步不乱跳）。界面右上角有 **"A:x B:x P:x"** 实时电平自检行：空闲时应显示 `A:1 B:1 P:1`（上拉 3.3V），旋转/按下时对应数字实时翻转；若恒定不变，说明信号未进入 MCU（检查接线与编码器供电）。

> 注意：PB3/PB4 复位后默认为 JTAG（JTDO/NJTRST），`encoder_init` 通过 `SWJTAG_MUX_010` 切换为仅 SWD 模式释放这两个引脚，**SWD 调试不受影响**（请勿使用 JTAG 调试）。

## 整机电源逻辑（DEEPSLEEP 停机模式）

电源状态循环：**上电/复位 → 默认停机（关机态，熄屏）→ 按 PUSH（PB3=EXINT3 下降沿）唤醒开机 → 运行中 PUSH 长按 3s → 熄屏 → 松手后停机 → 再按 PUSH 唤醒，原界面原地恢复（循环）**

- **模式选型**：当前采用 **DEEPSLEEP（停机）**——SRAM/GPIO/外设寄存器全保持，任意 EXINT 引脚可唤醒，**PUSH 无需改线（保持 PB3，按下=下降沿唤醒）**；唤醒后从 WFI 处继续执行（非复位），仅需软件恢复 PLL 到 240MHz。功耗高于 STANDBY（调压器低功耗模式百 ?A 级），但整机功耗瓶颈在屏幕/SGP30 等外设模块
- **上电默认关机态**：`main` 早期调用 `lowpower_boot_wait()`——自行完成最小唤醒前置（SWJTAG 释放 PB3 + 上拉输入 + EXINT3 下降沿）后停机，按 PUSH 返回即开机。**烧录后请按 PUSH 开机**；按复位键/重新上电会再次进入关机态
- **长按 3s 关机**：TMR7 1ms 中断维护连续按下时长，达 3000ms 产生长按事件。关机序列：投递消息让 UI 画 "Power off..." → 400ms 后 `backlight_off()` 熄屏 → 等待松手（EXINT3 以按下下降沿唤醒，松手后再停机才有有效边沿）→ 停机。唤醒后 `backlight_init()` 恢复到关机前亮度（RAM 保持），并发送 hold=0 消息隐藏关机提示，原界面原地恢复
- **唤醒确认（防假唤醒）**：按键弹片弹性回弹会使 PB3 产生"伪下降沿"（尤其 PUSH 线仅有内部弱上拉时）。唤醒后 `lowpower.c` 要求 PB3 **持续低电平 50ms**（真实按住）才确认开机，否则判定毛刺并自动重新停机；进停机前另有 200ms 安静期，且 WFI 前清除 TMR7/SysTick/EXINT3 pending 消除进入瞬间竞态
- **唤醒恢复时钟**：停机时硬件自动关闭 PLL，唤醒后 `lowpower.c` 内部重新使能 PLL 切回 240MHz（**不调用 `crm_reset()`**——停机期间 CRM 外设时钟门控全部保持，reset 会导致 TMR7/SPI1 等全部失效）
- **STANDBY 备用方案**：若未来需要 ?A 级待机，`lowpower.c` 保留完整 STANDBY 实现（唤醒脚固定 WKUP=PA0 上升沿，PUSH 需改接 PA0 且按键一脚改接 3.3V，唤醒等效复位），启用步骤见 `lowpower.h` 头注释
- **实现依赖**：官方库源 `at32f403a_407_pwc.c` 未收录入工程，`Drivers/lowpower.c` 直接操作 PWC 寄存器位段（`ctrl.vrsel`/`ctrl.lpsel` 等），无外部文件依赖

## 演示效果

上电后界面显示：顶部标题、中央 270° 环形仪表盘（弧心嵌亮度百分比大字与 eCO2/TVOC 小字）、翻转按钮：

- **旋转编码器**：调节背光亮度（每格 ±2%）——仪表盘弧值以 **400ms 缓动动画平滑扫动**，指示弧颜色随亮度三段渐变（20% 紫 → 60% 亮青 → 100% 荧光绿），弧心大号百分比数字同步变色并**放大-回弹弹跳**，与 TMR3 PWM 实际亮度实时同步
- **点按编码器**：按钮状态翻转——背景色 **300ms 渐变过渡**（OFF 灰 ? ON 蓝）+ 按钮**整体弹跳缩放**，文字随之变化（"BTN: ON"/"BTN: OFF"）
- **SGP30 空气质量实时显示**：弧心下方两行小字显示 **eCO2（绿色）** 与 **TVOC（青色）**，1s 周期刷新；初始化后约 15s 预热期固定显示 400/0（数据手册定义的正常现象）
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

/* sgp30.h —— SGP30 空气质量传感器（软件 I2C: SCL=PA2 / SDA=PA3） */
sgp30_status_t sgp30_init(void);              /* 初始化 I2C + Init_air_quality 命令(0x2003) */
sgp30_status_t sgp30_read(sgp30_data_t *data);/* 测量(0x2008)+CRC 校验, 读 eCO2(ppm)/TVOC(ppb) */

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
| 按键无效 | PUSH 为**低有效**（PB3 内部上拉，按下接地变低）；自检行 P 空闲应显示 1、按下变 0，若相反请检查按键一脚接线 |
| 背光闪烁/偏低 | 低于 20% 的亮度被驱动限幅（防止闪烁），范围可改 `backlight.h` 宏 |
| 无法 JTAG 调试 | 正常现象：PB3/PB4 已释放为编码器，请改用 SWD |
| 屏幕一直显示 "eCO2: -- ppm" | ① 检查 SGP30 模块 SCL→PA2 / SDA→PA3 / VCC→3.3V / GND 共地；② 模块须带板载上拉（PA2/PA3 开漏依赖上拉，`sgp30_init` 会做总线空闲检测）；③ 初始化失败时 `sgp30_task` 每 2s 自动重试，接好后约 2s 内恢复显示 |
| eCO2/TVOC 恒为 400ppm/0ppb | SGP30 上电预热期（约 15s）的固定输出，属正常现象，无需处理 |
| 上电后屏幕不亮 | 整机默认停机（关机态），**按一下 PUSH 开机**；烧录后频繁复位也一样，属正常设计 |
| 停机后按 PUSH 无法唤醒 | ① 确认按键一脚接 GND 且按下可靠接地（低有效，唤醒靠 EXINT3 下降沿）；② 确认关机时已松手（按住状态下停机无有效边沿）；③ 检查 PB3 电平自检行是否随按下翻转 |
| **松手后几秒屏幕自己亮起（假唤醒）** | 按键弹片"弹性后效"：长按松开后触点接触电阻在数秒内缓慢爬升，PUSH 线仅靠内部 ~40kΩ 弱上拉，PB3 电平缓慢滑落穿过 EXINT3 阈值产生"伪下降沿"。**软件已修复**（`lowpower.c`）：唤醒后须持续按住 50ms 才确认有效，毛刺自动重新停机；进停机前另有 200ms 安静期。**硬件建议**：给 PUSH 线增加 10kΩ 外部上拉到 3.3V，从源头增强抗回弹能力 |
| 关机唤醒后界面卡住/外设停摆 | 停机唤醒后时钟恢复由 `lowpower.c` 完成（重配 PLL 切 240MHz，**不做 `crm_reset()`**）；若在别处调用含 `crm_reset()` 的时钟配置会关闭全部外设时钟门控，导致 TMR7/SPI1 停摆 |
