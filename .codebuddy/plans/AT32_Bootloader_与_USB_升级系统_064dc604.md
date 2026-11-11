---
name: AT32 Bootloader 与 USB 升级系统
overview: 在 Software/Bootloader 新建 ≤12KB 的 BOOT 工程（TypeC/USB CDC PA11/PA12，跳转 APP 与固件升级），APP 固件集成在线触发升级（USB CDC + 复位进 BOOT），并在 TOOL 下实现 Python 拖入式升级上位机（自动识别 BOOT/APP 模式、传输、CRC 校验、提示升级成功）。
todos:
  - id: shared-layer
    content: 创建 Software/Shared 共享层：boot_shared.h(分区/参数页/协议定义) + flash_drv.c/h(寄存器级擦写) + usb_cdc.c/h(精简 CDC 栈)，使用 [skill:HMI Code Commenter] 规范注释
    status: completed
  - id: boot-project
    content: 编写 BOOT 工程：boot_main.c(时钟/决策/跳转) + boot_protocol.c/h + startup_boot.s + Bootloader.uvprojx
    status: completed
    dependencies:
      - shared-layer
  - id: boot-build
    content: UV4 命令行构建 BOOT，验证 0 错误且 Program Size ≤12KB，超限则裁剪
    status: completed
    dependencies:
      - boot-project
  - id: app-integrate
    content: APP 集成在线触发：app_usb_upgrade.c/h、app_main.c 加 VTOR/USB 初始化/重启标志、uvprojx 改 IROM 0x08003000 并加源文件，构建验证
    status: completed
    dependencies:
      - shared-layer
  - id: host-tool
    content: 编写 TOOL/upgrader/upgrade_host.py 拖入式升级上位机 + requirements.txt，py_compile 验证
    status: completed
  - id: final-check
    content: 双工程最终构建复核、清理临时文件、输出使用流程说明
    status: completed
    dependencies:
      - boot-build
      - app-integrate
      - host-tool
---

## 用户需求

在 `Software/Bootloader` 目录下生成 BOOT（Bootloader）项目，用于跳转 APP 及固件升级：

1. BOOT 代码尽可能小，控制在 **12KB 以内**
2. BOOT 使用 TypeC 接口，D+/D- 连接 **PA11/PA12** 引脚（USB 设备控制器）
3. 在 `TOOL` 下用 **Python** 实现升级上位机：整机通过 TypeC 连接上位机后，把 APP 文件（.bin）拖入上位机，自动完成升级并提示“升级中”与“升级成功”消息

## 澄清结果（已确认）

- **升级进入方式：APP 在线触发**——在 APP 固件中增加 USB CDC 与最小协议，上位机可命令运行中的 APP 写升级标志并自动复位进 BOOT，实现“拖入即升级”，无需手动按键；BOOT 同时保留 PB3 按住上电强制升级通道作为安全网（空片/APP 损坏时仍可升级）
- **BOOT 分区大小：12KB**（0x08000000~0x08002FFF），APP 从 **0x08003000** 开始链接

## 功能概述

- BOOT 上电决策：有升级请求 / APP 无效 / 按住 PB3 → 进入 USB 升级模式；否则直接跳转 APP
- 升级模式：BOOT 枚举为 USB 虚拟串口（CDC），接收上位机协议命令完成擦除、写入、CRC 校验，成功后置有效标志并自动跳转新 APP
- APP 运行期：同样枚举 CDC，应答上位机握手并响应“重启进 BOOT”命令，实现整机在线升级闭环
- 上位机：tkinter 图形界面，支持拖入 .bin 文件，自动识别设备（BOOT 或 APP 模式）、必要时命令 APP 切换到 BOOT、显示升级进度与结果提示

## Tech Stack

- **BOOT 固件**：C99 + Keil MDK5（ARM Compiler 6，`-Oz` 优化），SPL 头文件 + 寄存器级自写驱动（USB/Flash，因 `at32f403a_407_usb.c` 与 `at32f403a_407_flash.c` 不在仓库）
- **APP 固件**：沿用现有 FreeRTOS + LVGL 工程，新增中断驱动 USB CDC 模块（不触碰 RTOS API，避免线程安全问题）
- **上位机**：Python 3 + tkinter（GUI）+ pyserial（串口）+ tkinterdnd2（可选拖拽，缺失时回退为文件选择按钮/命令行参数）
- **构建验证**：`C:\Keil_v5\UV4\UV4.exe -b <uvprojx> -j0 -o build_log.txt` 命令行构建（已验证可用），检查 build log 的 Program Size

## Flash 分区规划（1MB）

| 区域 | 地址范围 | 大小 | 说明 |
| --- | --- | --- | --- |
| BOOT | 0x08000000 ~ 0x08002FFF | 12KB | Bootloader 本体，含向量表 |
| APP | 0x08003000 ~ 0x0807F7FF | ~988KB | 应用固件（重新链接到此基址） |
| 参数页 | 0x0807F800 ~ 0x0807FFFF | 2KB | magic('BTP1') / app_size / app_crc32 / flags(update_request, app_valid) |


APP 有效性判定：参数页标志 + APP 基址栈顶值落在 RAM 范围（0x20000000~0x20018000）且复位向量落在 APP 区间。

## 核心技术方案

### 1. 精简 USB CDC 栈（Software/Shared/usb_cdc.c/h，BOOT 与 APP 共用）

- 基于 AT32F403A **USBFS 设备控制器**（F1 风格，非 OTG）：寄存器结构 `usbd_type`（ept[8]/ctrl/intsts/devaddr/buftbl/cfg），PMA 包缓冲 0x40006000，BTABLE 缓冲描述符机制——全部使用 `at32f403a_407_usb.h` 官方宏（`USB_SET_TX_ADDRESS`、`USB_SET_RXLEN`、`USB_SET_TXSTS` 等）
- **描述符**：IAD + 通信接口（EP1 IN interrupt，仅存在不使用）+ 数据接口（EP2 IN / EP2 OUT bulk 64B）；自定义 VID/PID，Windows 免驱动 usbser 枚举为 COM 口
- **枚举处理**：EP0 控制传输（GET_DESCRIPTOR 设备/配置/字符串、SET_ADDRESS、SET_CONFIGURATION、SET_LINE_CODING/GET_LINE_CODING/SET_CONTROL_LINE_STATE）
- **中断**：`USBFS_H_CAN1_TX_IRQHandler`（IRQ19）/ `USBFS_L_CAN1_RX0_IRQHandler`（IRQ20），高优先级中断处理正确传输，低优先级处理其余
- 48MHz 时钟：`crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK)`（内部 HICK 校准 48MHz，不依赖 PLL 分频——240MHz PLL 无法整除出 48MHz，crm_usb_div_type 最大 ÷4）

### 2. BOOT 工程（Software/Bootloader/）

- **启动决策**：上电 → 时钟配置（PLL 240MHz，USB 用 HICK 48MHz）→ 读参数页 + 检测 PB3（寄存器级 GPIO 配置，不引 SPL gpio.c）→ 三条件任一成立进升级模式：update_request 置位 / APP 无效 / PB3 按住；否则跳 APP
- **跳转 APP**：`SCB->VTOR = 0x08003000` → 读 APP 栈顶/复位向量 → 关 USB 中断 + 复位 USB 外设（`crm_periph_reset`）→ `__set_MSP(sp)` → 函数指针跳转
- **升级协议**（帧格式：主机 `[0xA5][cmd][len16LE][payload]`，设备 `[0x5A][cmd][status][len16LE][payload]`）：
- `HELLO`：应答 BOOT 版本/模式/APP 有效性
- `ERASE`：按 APP 大小擦除 APP 区页（2KB/页）
- `DATA`：1024 字节/包写入（4 字节对齐补齐）+ ACK(seq)
- `VERIFY`：上位机发期望 CRC32，BOOT 无表逐位计算全 APP 区比对
- `DONE`：写参数页（size/crc/有效标志）→ 跳转新 APP
- **大小控制**：`-Oz`；SPL 仅编 `system_at32f403a_407.c` + `at32f403a_407_crm.c`；NVIC/PB3/flash 全部寄存器级自写；启动文件 Stack 缩至 0x400、Heap 0；无 printf/浮点。预估 ~9KB，余量充足

### 3. APP 在线触发（Software/APP/Application/app_usb_upgrade.c/h）

- 纯中断驱动复用共享 USB CDC 栈：HELLO 应答（mode=APP）+ REBOOT 命令（中断内仅置 `volatile` 标志，严格不触碰 LVGL/RTOS API，符合本项目“LVGL 仅 LVGL 任务访问”的线程约束）
- `encoder_task`（20ms 周期）检测标志 → 写参数页 update_request → `NVIC_SystemReset()`（在任务上下文执行 flash 写与复位，避开中断内写 flash 的死锁风险）
- USB 中断优先级配置为数值大于 `configMAX_SYSCALL_INTERRUPT_PRIORITY`（不调用 FreeRTOS FromISR API）

### 4. APP 工程配合修改

- `uvprojx`：IROM1 起始 0x08003000、大小 0x7F800；第 20 行 `<Cpu>` 描述同步
- `app_main.c`：`main()` 开头（SystemInit 之后）设 `SCB->VTOR = 0x08003000`（因 system_at32f403a_407.c 的 SystemInit 会把 VTOR 设回 0x08000000）；驱动初始化后调用 `app_usb_upgrade_init()`；`encoder_task` 增加重启标志检测分支
- APP 仍照常输出 hex/bin，但烧录需与 BOOT 配套（BOOT 烧 0x08000000，APP 烧 0x08003000 或经上位机升级）

### 5. 上位机（TOOL/upgrader/）

- `upgrade_host.py`（tkinter GUI）+ `requirements.txt`（pyserial；tkinterdnd2 可选）
- 流程：拖入/选择 .bin → 扫描 COM 口（VID/PID 过滤 + HELLO 探测）→ mode=APP 则发 REBOOT 并等待重枚举 → mode=BOOT 直接升级 → ERASE → DATA 循环（进度条）→ VERIFY → DONE → 弹窗“升级成功”；异常路径（无设备/超时/CRC 失败）均给出明确中文提示
- 帧收发带超时与重试，串口参数任意（CDC 全速批量与波特率无关）

## 目录结构

```
d:/shang256/AT32_2.4inchTFT/
├── Software/
│   ├── Shared/                        # [NEW] BOOT/APP 共享代码层
│   │   ├── boot_shared.h              # 分区地址/参数页结构/协议命令枚举/帧格式
│   │   ├── flash_drv.c/.h             # 寄存器级 flash 驱动(解锁/擦页/字编程/锁)
│   │   └── usb_cdc.c/.h               # 精简 USB CDC 栈(枚举+bulk 收发)
│   ├── Bootloader/                    # [NEW] BOOT 工程(≤12KB)
│   │   ├── boot_conf.h                # BOOT 配置(超时/版本/协议参数)
│   │   ├── Application/boot_main.c    # 时钟/启动决策/跳转 APP
│   │   ├── Drivers/boot_protocol.c/.h # 升级协议状态机(HELLO/ERASE/DATA/VERIFY/DONE)
│   │   ├── MDK/startup_boot.s         # 启动文件(Stack 0x400/Heap 0)
│   │   └── Project/Bootloader.uvprojx # Keil 工程(相对路径引用 ../APP/Libraries 与 ../Shared)
│   └── APP/                           # [MODIFY] 应用工程
│       ├── Application/app_usb_upgrade.c/.h  # [NEW] APP 端 USB CDC + REBOOT 命令
│       ├── Application/app_main.c             # [MODIFY] VTOR 重定位/USB 初始化/重启标志
│       └── Project/AT32F403A_LCD_Demo.uvprojx # [MODIFY] IROM 0x08003000 + 新源文件
├── TOOL/
│   └── upgrader/                      # [NEW] Python 升级上位机
│       ├── upgrade_host.py            # tkinter GUI 拖入式升级工具
│       └── requirements.txt           # pyserial(必需)/tkinterdnd2(可选)
```

## Implementation Notes

- **PMA 访问**：严格按 `at32f403a_407_usb.h` 官方宏模式操作 BTABLE 描述符（32 位访问）与 PMA 数据（16 位拷贝，参照 Artery 官方 usb_write_packet 风格）
- **Flash 写安全**：中断上下文绝不擦写 flash（擦页期间取指停顿会丢 USB 时序）；APP 侧写参数页放在任务上下文，BOOT 侧在收包处理循环中同步执行并依赖主机 ACK 超时重发
- **参数页更新**：擦写前先 RAM 缓存整页参数再回写（flash 只能 1→0，标志更新需整页重编程）
- **跳转前清理**：关 USB 中断 + 外设复位 + `__disable_irq()` 后恢复（APP 启动文件会重新初始化），确保 APP 从干净状态启动
- **回归控制**：APP 既有功能（编码器/LVGL/传感器/停机）不受影响，仅新增 USB 初始化与 encoder_task 一个检测分支；BOOT 构建后检查 build log Program Size ≤ 12288 字节
- 注释风格遵循项目既有中文函数头规范（函数名/入口参数/出口参数/返回值/函数功能/修改历史）

## Agent Extensions

### Skill

- **HMI Code Commenter**
- Purpose: 新增 BOOT/APP/上位机联动的 C 源码（usb_cdc.c、flash_drv.c、boot_main.c、boot_protocol.c、app_usb_upgrade.c）时，确保所有函数遵循项目既有的中文函数头注释规范（函数名/入口参数/出口参数/返回值/函数功能/修改历史）与步骤化行内注释
- Expected outcome: 新增 C 代码注释风格与现有 app_ui.c/app_main.c 完全一致