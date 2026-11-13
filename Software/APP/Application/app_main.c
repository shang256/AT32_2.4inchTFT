/**
  **************************************************************************
  * @file     app_main.c
  * @brief    应用层入口：硬件初始化 + FreeRTOS 任务框架（入口 main）
  *
  * 分层说明（本工程三层架构）：
  *   Application（本层）  : 业务任务编排与 UI 逻辑
  *   Middleware（中间层） : FreeRTOS 配置 / LVGL 配置与显示适配
  *   Drivers（驱动层）    : LCD / 背光 PWM / 编码器 / SGP30 / 延时 / 中断分发
  *
  * 任务划分：
  *   1. encoder_task（优先级 3）: 20ms 轮询编码器驱动, 将旋转/按键事件
  *      投递到 FreeRTOS 队列 —— 不直接触碰 LVGL（保证线程安全）；
  *   2. lvgl_task（优先级 2）  : LVGL 刷图专用线程, 周期执行
  *      lv_timer_handler(), 并从队列取出编码器/传感器事件驱动 UI 联动；
  *   3. sgp30_task（优先级 1） : 1s 周期读取 SGP30 的 eCO2/TVOC 并投递
  *      队列(驱动内部 12ms 忙等可被高优先级任务随时抢占, 不影响 UI)。
  *
  * 整机电源逻辑（DEEPSLEEP 停机）：
  *   上电/复位 -> 默认停机(关机态) -> PUSH 按下(PB3=EXINT3 下降沿)
  *   -> 唤醒(恢复 240MHz) -> 开机运行 -> PUSH 长按 3s -> 熄屏停机
  *   -> 再按 PUSH 唤醒, 原界面原地恢复显示(循环)。
  **************************************************************************
  */

#include "app_main.h"
#include "app_ui.h"
#include "app_state.h"
#include "app_usb_upgrade.h"
#include "driver_delay.h"
#include "driver_lcd_st7789.h"
#include "driver_backlight.h"
#include "driver_encoder.h"
#include "driver_lowpower.h"
#include "driver_sgp30.h"
#include "driver_aht20.h"
#include "driver_bmp280.h"
#include "driver_max30102.h"
#include "driver_qmc6309.h"
#include "lvgl.h"
#include "lv_port_disp.h"

/* 全局: 编码器事件队列(驱动层事件 -> LVGL 线程) */
QueueHandle_t g_encoder_queue = NULL;

/* 全局: SGP30 传感器数据队列(传感器任务 -> LVGL 线程) */
QueueHandle_t g_sensor_queue = NULL;

/* 全局: 环境传感器数据队列(AHT20/BMP280 任务 -> LVGL 线程) */
QueueHandle_t g_env_queue = NULL;

/* 全局: 心率血氧数据队列(MAX30102 任务 -> LVGL 线程) */
QueueHandle_t g_hr_queue = NULL;

/* 全局: 指南针数据队列(QMC6309 -> LVGL 线程) */
QueueHandle_t g_compass_queue = NULL;

/* 全局: I2C 总线互斥锁(保护软件 I2C 总线的多任务并发访问) */
SemaphoreHandle_t g_i2c_mutex = NULL;

/* 任务栈大小(字: 4 字节) */
#define TASK_STACK_LVGL    (uint16_t)(1024)   /* LVGL 任务 4KB 栈 */
#define TASK_STACK_ENCODER (uint16_t)(256)    /* 编码器任务 1KB 栈 */
#define TASK_STACK_I2C     (uint16_t)(640)    /* I2C 总线传感器任务 2.5KB 栈(四设备顺序访问, 算法缓冲为静态) */

/*********************************************************************
 * 函 数 名: system_clock_config
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 配置系统时钟至 240MHz（HICK 8MHz 经二分频 4MHz 入 PLL, 60 倍频）
 * 说    明: apb2=120MHz(SPI1 时钟源, SPI 二分频后 60MHz);
 *           apb1=120MHz(TMR3 时钟源, 240 分频后 1MHz 计数);
 *           F403A 零等待区(ZW)Flash 架构下无需手动设置 Flash 等待周期
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 从原工程迁移, 逻辑未变
 **********************************************************************/
static void system_clock_config(void)
{
  /* ---- Step1: 复位时钟管理器(CRM), 恢复默认配置 ---- */
  crm_reset();

  /* ---- Step2: 使能内部高速时钟 HICK(8MHz, 无需外部晶振) ---- */
  crm_clock_source_enable(CRM_CLOCK_SOURCE_HICK, TRUE);
  while(crm_flag_get(CRM_HICK_STABLE_FLAG) != SET)
  {
  }

  /* ---- Step3: 配置 PLL: HICK/2=4MHz 入 PLL, 60 倍频输出 240MHz ---- */
  crm_pll_config(CRM_PLL_SOURCE_HICK, CRM_PLL_MULT_60, CRM_PLL_OUTPUT_RANGE_GT72MHZ);
  crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
  while(crm_flag_get(CRM_PLL_STABLE_FLAG) != SET)
  {
  }

  /* ---- Step4: 总线分频: AHB 不分频, APB1/APB2 二分频 ---- */
  crm_ahb_div_set(CRM_AHB_DIV_1);
  crm_apb1_div_set(CRM_APB1_DIV_2);
  crm_apb2_div_set(CRM_APB2_DIV_2);

  /* ---- Step5: 顺滑模式下切换 PLL 为系统时钟源 ---- */
  crm_auto_step_mode_enable(TRUE);
  crm_sysclk_switch(CRM_SCLK_PLL);
  while(crm_sysclk_switch_status_get() != CRM_SCLK_PLL)
  {
  }
  crm_auto_step_mode_enable(FALSE);

  /* ---- Step6: 更新全局系统时钟变量(delay/backlight 等驱动依赖此值) ---- */
  system_core_clock_update();
}

/*********************************************************************
 * 函 数 名: encoder_task
 * 入口参数: arg     任务参数(未使用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 编码器任务: 20ms 轮询驱动层, 将旋转/按键/长按进度事件投递
 *           队列; 长按达 3s 时执行关机序列(熄屏->松手->停机)
 * 说    明: 本任务只与驱动层/队列交互, 不触碰任何 LVGL 接口,
 *           避免跨线程访问 UI 造成竞态;
 *           关机序列说明: 1) 先投递"长按达阈值"消息让 UI 画出
 *           "Power off"提示; 2) 400ms 后熄背光; 3) 等待松手——
 *           EXINT3 以按下下降沿唤醒, 松手后再停机才可获得有效边沿;
 *           4) 停机(DEEPSLEEP)期间 RAM/屏幕 GRAM 保持, PUSH 按下
 *           唤醒后函数返回: 恢复背光并投递唤醒开机事件(状态机重走
 *           LOGO->仪表盘 开机流程, 非原地恢复旧界面), 任务循环继续
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(FreeRTOS 引入);
 *     修改日期: 2026年10月17日
 *     修改内容: 新增长按 3s 关机序列与 hold_ms 事件投递(低功耗需求);
 *               待机方案由 STANDBY 改为 DEEPSLEEP(不改 PUSH 接线)
 *     修改日期: 2026年11月11日
 *     修改内容: 循环新增 USB 升级重启请求检测(APP 在线升级需求)
 *     修改日期: 2026年11月13日
 *     修改内容: 唤醒后由"原地恢复旧界面"改为投递 wakeup 开机事件,
 *               状态机重走 LOGO 开机流程(状态机需求); 常规周期显式
 *               清零 wakeup 字段防止残留
 **********************************************************************/
__NO_RETURN static void encoder_task(void *arg)
{
  app_encoder_msg_t msg;                          // 待投递的事件
  static uint32_t s_prev_hold = 0;                // 上一周期按下时长(检测归零沿)
  (void)arg;

  while(1)
  {
    /* ---- Step0: USB 升级重启请求检测(写升级标志并复位, 不返回) ---- */
    if(app_usb_upgrade_poll_reboot() != 0U)
    {
      /* 仅消除断言路径, NVIC_SystemReset 不会返回 */
    }

    /* ---- Step1: 读取驱动层累计增量/按键沿事件/长按时长 ---- */
    msg.delta = encoder_get_delta();
    msg.push_pressed = encoder_push_rising_edge();
    msg.hold_ms = (int32_t)encoder_push_hold_ms();
    msg.wakeup = 0;                                 /* 常规周期无唤醒事件(仅 Step2.1 置 1) */

    /* ---- Step2: 长按达 3s -> 执行关机序列 ---- */
    if(encoder_push_long_press() != 0)
    {
      msg.hold_ms = (int32_t)ENC_PUSH_LONG_PRESS_MS;   /* 强制携带阈值语义 */
      xQueueSend(g_encoder_queue, &msg, 0);           /* UI 画出 Power off 提示 */
      vTaskDelay(pdMS_TO_TICKS(400));                  /* 留出 LVGL 绘制时间 */
      backlight_off();                                 /* 熄背光(PWM 最低 20% 仍可见) */
      while(encoder_push_pressed() != 0U)              /* 等待松手(见函数头注释 3) */
      {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      vTaskDelay(pdMS_TO_TICKS(200));                  /* 安静期: 躲开松手弹片回弹初期 */
      lowpower_power_down();                          /* 停机, PUSH 按下唤醒 */

      /* Step2.1 唤醒恢复: 背光回到关机前亮度(s_percent 在 RAM 中保持),
              并投递"停机唤醒开机事件"——状态机收到后重走完整开机流程
              (清关机提示 -> LOGO -> 仪表盘), 替代原先的"原界面原地恢复" */
      backlight_init();                               /* 恢复 PB1 复用 + PWM 输出 */
      msg.delta = 0;
      msg.push_pressed = 0;
      msg.hold_ms = 0;
      msg.wakeup = 1;                                 /* 开机事件: 状态机 OFF->POWER_ON->LOGO */
      xQueueSend(g_encoder_queue, &msg, 0);           /* LVGL 任务消费后启动开机流程 */
      s_prev_hold = 0;                                /* 归零沿已随事件携带, 无需补发 */
    }

    /* ---- Step3: 有事件才投递(hold 从非 0 归 0 也发, 供 UI 取消提示) ---- */
    if((msg.delta != 0) || (msg.push_pressed != 0)
       || (msg.hold_ms != 0) || (s_prev_hold != 0))
    {
      xQueueSend(g_encoder_queue, &msg, 0);      // 队列满则丢弃(旧事件会被新值覆盖, 无碍)
    }
    s_prev_hold = (uint32_t)msg.hold_ms;

    /* ---- Step4: 空闲时休眠 20ms ---- */
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

/*********************************************************************
 * 函 数 名: i2c_sensor_task
 * 入口参数: arg     任务参数(未使用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: I2C 总线统一传感器任务: 单任务独占软件 I2C 总线, 以
 *           500ms 节拍轮询总线上挂载的全部设备
 *           (SGP30 + AHT20 + BMP280 + MAX30102 + QMC6309)并投递结果
 * 节拍编排: 1. MAX30102 每拍(500ms)最先执行 —— FIFO 深度 32 x
 *              50sps 仅容纳 640ms, 放在拍首保证两次读取间隔最稳,
 *              稳态约 595ms(含 1s 设备段耗时), 不溢出;
 *           2. SGP30/AHT20/BMP280 为 1s 周期, 在偶数拍执行;
 *           3. 单任务独占总线, 互斥锁仍保留(段级 take/give, 开销
 *              可忽略), 为未来新增 I2C 使用者保持并发安全;
 *           4. 各设备独立就绪标志与离线重试分频(约 2s), 热插拔
 *              自动恢复, 任一离线时对应字段显示 "--" 互不影响;
 *           5. 驱动内部 delay_ms 忙等(AHT20 读约 80ms, 初始化最坏
 *              约 240ms)期间可被编码器/LVGL 任务抢占, 不阻塞 UI;
 *           6. SGP30 初始化后需 1s 周期持续测量维护基线, 前 15s
 *              预热期固定输出 400ppm/0ppb 属正常现象。
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月12日
 *     修改内容: 新增函数(合并 sgp30_task/env_task/max30102_task
 *               为单总线任务, 消除互斥竞争并节省任务栈);
 *             2026年11月12日新增 QMC6309 磁力计轮询(A4 段, 500ms,
 *               指南针页面需求)
 **********************************************************************/
__NO_RETURN static void i2c_sensor_task(void *arg)
{
  app_sensor_msg_t  smsg;                           // SGP30 待投递数据
  app_env_msg_t     emsg;                           // 环境传感器待投递数据
  app_hr_msg_t      hmsg;                           // 心率血氧待投递数据
  app_compass_msg_t cmsg;                           // 指南针待投递数据
  sgp30_data_t      sgp;                            // SGP30 测量结果
  aht20_data_t      aht;                            // AHT20 温湿度结果
  bmp280_data_t     bmp;                            // BMP280 气压结果
  max30102_result_t hr;                             // MAX30102 结果
  qmc6309_data_t    qmc;                            // QMC6309 三轴磁场结果
  uint8_t sgp_ready = 0U;                           // SGP30 就绪标志
  uint8_t aht_ready = 0U;                           // AHT20 就绪标志
  uint8_t bmp_ready = 0U;                           // BMP280 就绪标志
  uint8_t hr_ready  = 0U;                           // MAX30102 就绪标志
  uint8_t qmc_ready = 0U;                           // QMC6309 就绪标志
  uint8_t sgp_div   = 0U;                           // SGP30 离线重试分频(2 个 1s 拍 = 2s)
  uint8_t hr_div    = 0U;                           // MAX30102 离线重试分频(4 拍 = 2s)
  uint8_t qmc_div   = 0U;                           // QMC6309 离线重试分频(4 拍 = 2s)
  uint8_t tick      = 0U;                           // 500ms 节拍计数(偶数拍跑 1s 设备)
  uint8_t ok;                                       // I2C 事务结果缓存
  (void)arg;

  while(1)
  {
    /* ================= A 段: MAX30102 心率血氧(每 500ms, 拍首执行) ================= */

    /* ---- A1: 未就绪则分频重试初始化(约 2s 一次) ---- */
    if(hr_ready == 0U)
    {
      if(hr_div == 0U)
      {
        hr_div = 4U;
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        hr_ready = (max30102_init() == MAX30102_OK) ? 1U : 0U;
        xSemaphoreGive(g_i2c_mutex);
      }
      else
      {
        hr_div--;
      }
    }

    /* ---- A2: 就绪则读 FIFO + 算法, 通信失败复位就绪(下周期重连) ---- */
    if(hr_ready != 0U)
    {
      xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
      ok = (max30102_read(&hr) == MAX30102_OK) ? 1U : 0U;
      xSemaphoreGive(g_i2c_mutex);
      if(ok != 0U)
      {
        hmsg.hr_bpm    = (int32_t)hr.hr_bpm;
        hmsg.spo2      = (int32_t)hr.spo2;
        hmsg.finger_on = (int32_t)hr.finger_on;
        hmsg.valid     = 1;
      }
      else
      {
        hr_ready = 0U;
        hmsg.hr_bpm = 0; hmsg.spo2 = 0; hmsg.finger_on = 0; hmsg.valid = 0;
      }
    }
    /* ---- A3: 离线 -> 投递占位(UI 显示 "--") ---- */
    else
    {
      hmsg.hr_bpm = 0; hmsg.spo2 = 0; hmsg.finger_on = 0; hmsg.valid = 0;
    }
    xQueueSend(g_hr_queue, &hmsg, 0);

    /* ---- A4: QMC6309 磁力计(每 500ms, 读取失败仅复位就绪不重发占位,
       指南针页面保持上一次角度, 离线消息由 A4.1 补发) ---- */
    if(qmc_ready == 0U)
    {
      if(qmc_div == 0U)
      {
        qmc_div = 4U;                               /* 4 拍 = 2s 重试间隔 */
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        qmc_ready = (qmc6309_init() == QMC6309_OK) ? 1U : 0U;
        xSemaphoreGive(g_i2c_mutex);
      }
      else
      {
        qmc_div--;
      }
      /* A4.1 离线 -> 投递占位(UI 指南针显示 "--") */
      cmsg.heading_x10 = 0; cmsg.raw_x = 0; cmsg.raw_y = 0; cmsg.raw_z = 0;
      cmsg.valid = 0;
      xQueueSend(g_compass_queue, &cmsg, 0);
    }
    if(qmc_ready != 0U)
    {
      xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
      ok = (qmc6309_read(&qmc) == QMC6309_OK) ? 1U : 0U;
      xSemaphoreGive(g_i2c_mutex);
      if(ok != 0U)
      {
        cmsg.heading_x10 = qmc6309_heading(&qmc);
        cmsg.raw_x = (int32_t)qmc.x;
        cmsg.raw_y = (int32_t)qmc.y;
        cmsg.raw_z = (int32_t)qmc.z;
        cmsg.valid = 1;
        xQueueSend(g_compass_queue, &cmsg, 0);
      }
      else
      {
        qmc_ready = 0U;                             /* 读失败: 复位就绪, 2s 后重连 */
      }
    }

    /* ================= B 段: 1s 周期设备(偶数拍执行) ================= */
    if((tick & 1U) == 0U)
    {
      /* ---- B1: SGP30 eCO2/TVOC(未就绪分频重试, 读取失败复位) ---- */
      if(sgp_ready == 0U)
      {
        if(sgp_div == 0U)
        {
          sgp_div = 2U;                              /* 2 个 1s 拍 = 2s 重试间隔 */
          xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
          sgp_ready = (sgp30_init() == SGP30_OK) ? 1U : 0U;
          xSemaphoreGive(g_i2c_mutex);
        }
        else
        {
          sgp_div--;
        }
      }
      if(sgp_ready != 0U)
      {
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        ok = (sgp30_read(&sgp) == SGP30_OK) ? 1U : 0U;
        xSemaphoreGive(g_i2c_mutex);
        if(ok != 0U)
        {
          smsg.eco2  = (uint32_t)sgp.eco2;
          smsg.tvoc  = (uint32_t)sgp.tvoc;
          smsg.valid = 1U;
        }
        else
        {
          sgp_ready = 0U;                            /* 读失败: 复位就绪, 2s 后重连 */
          smsg.valid = 0U;
        }
      }
      else
      {
        smsg.valid = 0U;                             /* 离线: UI 显示 "--" */
      }
      xQueueSend(g_sensor_queue, &smsg, 0);

      /* ---- B2: AHT20 温湿度 + BMP280 气压海拔(各自独立就绪判定) ---- */
      emsg.temp_x10  = 0;
      emsg.hum_x10   = 0;
      emsg.press_x10 = 0;
      emsg.altitude  = 0;
      emsg.aht_valid = 0;
      emsg.bmp_valid = 0;

      /* AHT20: 未就绪先初始化, 读失败复位(下个 1s 拍重新初始化) */
      if(aht_ready == 0U)
      {
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        aht_ready = (aht20_init() == AHT20_OK) ? 1U : 0U;
        xSemaphoreGive(g_i2c_mutex);
      }
      if(aht_ready != 0U)
      {
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        ok = (aht20_read(&aht) == AHT20_OK) ? 1U : 0U;
        xSemaphoreGive(g_i2c_mutex);
        if(ok != 0U)
        {
          emsg.temp_x10  = (int32_t)aht.temp_x10;
          emsg.hum_x10   = (int32_t)aht.hum_x10;
          emsg.aht_valid = 1;
        }
        else
        {
          aht_ready = 0U;
        }
      }

      /* BMP280: 同模式, 输出气压 + 换算海拔 */
      if(bmp_ready == 0U)
      {
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        bmp_ready = (bmp280_init() == BMP280_OK) ? 1U : 0U;
        xSemaphoreGive(g_i2c_mutex);
      }
      if(bmp_ready != 0U)
      {
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        ok = (bmp280_read(&bmp) == BMP280_OK) ? 1U : 0U;
        xSemaphoreGive(g_i2c_mutex);
        if(ok != 0U)
        {
          emsg.press_x10 = bmp.press_x10;
          emsg.altitude  = (int32_t)bmp280_altitude(bmp.press_x10);
          emsg.bmp_valid = 1;
        }
        else
        {
          bmp_ready = 0U;
        }
      }
      xQueueSend(g_env_queue, &emsg, 0);
    }

    /* ================= C 段: 500ms 节拍 ================= */
    vTaskDelay(pdMS_TO_TICKS(500));
    tick++;
  }
}

/*********************************************************************
 * 函 数 名: lvgl_task
 * 入口参数: arg     任务参数(未使用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: LVGL 刷图专用线程: 周期执行 lv_timer_handler 驱动
 *           刷新/动画/输入处理, 并转发编码器/传感器事件:
 *           1) 停机唤醒开机事件 -> 状态机 app_state_wakeup();
 *           2) 旋转增量 -> 背光调节(持续交互, 非状态跳转);
 *           3) 按键沿/长按进度 -> 状态机语义事件分发(短按/长按),
 *              由当前状态注册的按键函数指针处理;
 *           4) 周期调用 app_state_tick() 驱动开机过渡/LOGO 计时跳转
 * 说    明: LVGL 非线程安全, 所有 UI 访问都收敛在本任务
 *           (编码器/传感器事件经队列从各自任务送入)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(FreeRTOS 引入);
 *             2026年10月15日新增 SGP30 队列消费转发
 *     修改日期: 2026年11月13日
 *     修改内容: 按键事件转发重构为状态机分发(app_state_key_event),
 *               新增 app_state_tick 周期调度与 wakeup 开机事件转发
 *               (状态机需求); 旋转调背光路径改调 app_ui_encoder_rotate
 **********************************************************************/
__NO_RETURN static void lvgl_task(void *arg)
{
  app_encoder_msg_t msg;                          // 从队列收到的事件
  app_sensor_msg_t  smsg;                         // 从队列收到的传感器数据
  app_env_msg_t     emsg;                         // 从队列收到的环境传感器数据
  app_hr_msg_t      hmsg;                         // 从队列收到的心率血氧数据
  app_compass_msg_t cmsg;                         // 从队列收到的指南针数据
  int32_t           prev_hold = 0;                // 上一条消息的按下时长(检测归零沿)
  (void)arg;

  while(1)
  {
    /* ---- Step1: 非阻塞收取编码器事件(可能一次积压多条, 全部处理) ---- */
    while(xQueueReceive(g_encoder_queue, &msg, 0) == pdTRUE)
    {
      /* Step1.1: 停机唤醒开机事件(状态机 OFF->POWER_ON, 重走 LOGO 流程) */
      if(msg.wakeup != 0)
      {
        app_state_wakeup();
      }

      /* Step1.2: 旋转增量 -> 背光调节(持续交互, 不属于状态跳转) */
      if(msg.delta != 0)
      {
        app_ui_encoder_rotate(msg.delta);
      }

      /* Step1.3: 按下沿事件 -> 状态机短按语义事件(按键触发函数指针分发) */
      if(msg.push_pressed != 0)
      {
        app_state_key_event(APP_KEY_SHORT_PRESS, (uint32_t)msg.hold_ms);
      }

      /* Step1.4: 长按事件(保持中/达阈值/松开归零) -> 状态机分发
         (归零沿也转发: 状态机统一取消关机进度提示) */
      if((msg.hold_ms != 0) || (prev_hold != 0))
      {
        if(msg.hold_ms >= (int32_t)ENC_PUSH_LONG_PRESS_MS)
        {
          app_state_key_event(APP_KEY_LONG_PRESS, (uint32_t)msg.hold_ms);
        }
        else
        {
          app_state_key_event(APP_KEY_HOLD_PROGRESS, (uint32_t)msg.hold_ms);
        }
      }
      prev_hold = msg.hold_ms;
    }

    /* ---- Step2: 非阻塞收取 SGP30 传感器数据, 转发 UI 显示 ---- */
    while(xQueueReceive(g_sensor_queue, &smsg, 0) == pdTRUE)
    {
      app_ui_sgp30_update(smsg.eco2, smsg.tvoc, smsg.valid);
    }

    /* ---- Step2.1: 非阻塞收取环境传感器数据(AHT20/BMP280), 转发 UI 显示 ---- */
    while(xQueueReceive(g_env_queue, &emsg, 0) == pdTRUE)
    {
      app_ui_env_update(emsg.temp_x10, emsg.hum_x10, emsg.press_x10,
                        emsg.altitude, emsg.aht_valid, emsg.bmp_valid);
    }

    /* ---- Step2.2: 非阻塞收取心率血氧数据(MAX30102), 转发 UI 显示 ---- */
    while(xQueueReceive(g_hr_queue, &hmsg, 0) == pdTRUE)
    {
      app_ui_hr_update(hmsg.hr_bpm, hmsg.spo2, hmsg.finger_on, hmsg.valid);
    }

    /* ---- Step2.3: 非阻塞收取指南针数据(QMC6309), 转发 UI 显示 ---- */
    while(xQueueReceive(g_compass_queue, &cmsg, 0) == pdTRUE)
    {
      app_ui_compass_update(cmsg.heading_x10, cmsg.raw_x, cmsg.raw_y,
                            cmsg.raw_z, cmsg.valid);
    }

    /* ---- Step3: 状态机周期调度(开机过渡延时/LOGO 到时自动跳转) ---- */
    app_state_tick();

    /* ---- Step4: 执行 LVGL 刷新(脏区绘制 + flush 到屏幕) ---- */
    lv_timer_handler();

    /* ---- Step5: 5ms 周期, 平衡响应速度与 CPU 占用 ---- */
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/*********************************************************************
 * 函 数 名: main
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: int(正常情况下不会返回)
 * 函数功能: 程序入口: 驱动层初始化 -> LVGL 初始化 -> 创建任务 -> 启动调度器
 * 说    明: 调度器启动后 main 上下文结束, 之后全部逻辑运行在任务中
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 由裸机大循环架构重构为 FreeRTOS 双任务架构
 *     修改日期: 2026年11月11日
 *     修改内容: 新增 VTOR 重定位(0x08003000)与 USB 在线升级初始化
 *               (BOOT/APP 分区升级需求, APP 链接基址 0x08003000)
 *     修改日期: 2026年11月12日
 *     修改内容: 新增 MAX30102 心率血氧任务与队列(第五应用任务);
 *               同日合并 SGP30/ENV/HR 三任务为单一 i2c_sensor_task
 *               (单任务独占 I2C 总线, 节省约 2.5KB 任务栈)
 *     修改日期: 2026年11月13日
 *     修改内容: 恢复上电默认停机按键开机(lowpower_init/boot_wait);
 *               app_ui_init 更名为 app_ui_page_create 并新增
 *               app_state_init 状态机启动(状态机需求)
 **********************************************************************/
int main(void)
{
  /* ---- Step0: 向量表重定位到 APP 基址 0x08003000 ----
     SystemInit(system_at32f403a_407.c)会把 VTOR 设回 0x08000000,
     必须在其后(main 中)重设, BOOT 跳转与本机复位两条路径统一生效 */
  SCB->VTOR = 0x08003000UL;

  /* ---- Step1: 系统时钟配置到 240MHz, 使能 DWT 延时基准 ---- */
  system_clock_config();
  delay_init();

  /* ---- Step1.1: 低功耗开机管理(整机电源逻辑) ----
     默认关机态: 停机(DEEPSLEEP)等待 PUSH 按下(PB3=EXINT3 下降沿)
     唤醒, 函数返回即"按键按下开机", main 继续正常初始化流程,
     状态机随后走 LOGO->仪表盘 开机流程;
     按复位/上电会再次进入停机(默认关机态);
     (调试期如需跳过按键开机, 注释下面两行即可) */
  lowpower_init();
  lowpower_boot_wait();

  /* ---- Step2: 驱动层初始化: 屏幕 / 背光 PWM / 编码器 ---- */
  LCD_Init();
  backlight_init();                               /* 初始亮度 100% */
  encoder_init();

  /* ---- Step2.1: USB 在线升级初始化(TypeC 虚拟串口常驻, 拖入即升级) ---- */
  app_usb_upgrade_init();

  /* ---- Step3: 中间层初始化: LVGL 内核与显示适配 ---- */
  lv_init();
  lv_port_disp_init();

  /* ---- Step4: 应用层初始化: 创建全部页面控件(LOGO/仪表/指南针,
     必须在 LVGL 任务首次刷新前)并启动状态机(首屏=开机 LOGO 页,
     约 1.5s 后自动进骑行仪表盘) ---- */
  app_ui_page_create();
  app_state_init();

  /* ---- Step5: 创建事件队列与三个应用任务 ---- */
  g_encoder_queue = xQueueCreate(8, sizeof(app_encoder_msg_t));
  g_sensor_queue  = xQueueCreate(4, sizeof(app_sensor_msg_t));
  g_env_queue     = xQueueCreate(4, sizeof(app_env_msg_t));
  g_hr_queue      = xQueueCreate(4, sizeof(app_hr_msg_t));
  g_compass_queue = xQueueCreate(4, sizeof(app_compass_msg_t));
  g_i2c_mutex     = xSemaphoreCreateMutex();     /* 保护软件 I2C 总线的并发访问(预留) */
  xTaskCreate(encoder_task,   "ENC", TASK_STACK_ENCODER, NULL, 3, NULL);
  xTaskCreate(lvgl_task,      "LVGL",TASK_STACK_LVGL,    NULL, 2, NULL);
  xTaskCreate(i2c_sensor_task,"I2C", TASK_STACK_I2C,     NULL, 1, NULL);

  /* ---- Step6: 启动 FreeRTOS 调度器(不再返回) ---- */
  vTaskStartScheduler();

  /* 调度器启动失败(堆不足等), 停在此处便于调试定位 */
  while(1)
  {
  }
}

/*********************************************************************
 * 函 数 名: vApplicationMallocFailedHook
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: FreeRTOS 堆内存分配失败钩子
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(FreeRTOS 引入)
 **********************************************************************/
__NO_RETURN void vApplicationMallocFailedHook(void)
{
  while(1)
  {
  }
}

/*********************************************************************
 * 函 数 名: vApplicationStackOverflowHook
 * 入口参数: xTask       发生溢出的任务句柄
 *           pcTaskName  任务名
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 任务栈溢出钩子(死循环挂起, 便于调试器查看任务名)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(FreeRTOS 引入)
 **********************************************************************/
__NO_RETURN void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  (void)pcTaskName;
  while(1)
  {
  }
}
