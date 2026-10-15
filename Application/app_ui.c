/**
  **************************************************************************
  * @file     app_ui.c
  * @brief    应用层交互演示 UI 实现（旋转调背光亮度条 / 点按翻转按钮）
  *
  * 交互设计（240x240 深色主题）：
  *   - 屏幕中部大号亮度百分比文字 + 亮度条(Bar 20~100%)：
  *     旋转编码器时 UI 显示条、百分比文字与 TMR3 PWM 亮度实时同步；
  *   - 屏幕下部一个按钮(BTN)：点按编码器按键时按钮状态翻转，
  *     ON 态为蓝色高亮、OFF 态为灰色暗显，文字随之变化；
  *   - 底部提示文字, 右下角 LVGL 性能监视器显示 FPS。
  *
  * 线程约束：所有 LVGL 接口仅由 LVGL 任务调用（app_main.c 的 lvgl_task）,
  * 编码器事件经 FreeRTOS 队列送达, 本模块不做任何跨线程 UI 访问。
  **************************************************************************
  */

#include "app_ui.h"
#include "backlight.h"
#include "encoder.h"

/* 界面控件句柄(LVGL 任务上下文访问, 无需保护) */
static lv_obj_t  *s_title_label;      // 顶部标题
static lv_obj_t  *s_value_label;      // 大号亮度百分比文字
static lv_obj_t  *s_bl_bar;           // 背光亮度条
static lv_obj_t  *s_bl_label;        // 亮度条下方提示
static lv_obj_t  *s_btn;              // 点按翻转按钮
static lv_obj_t  *s_btn_label;       // 按钮文字(子对象)
static lv_obj_t  *s_hint_label;      // 底部操作提示
static lv_obj_t  *s_debug_label;     // 引脚电平自检行(硬件排查用)
static lv_obj_t  *s_eco2_label;      // SGP30 eCO2 显示(左)
static lv_obj_t  *s_tvoc_label;      // SGP30 TVOC 显示(右)

/* 按钮翻转状态: 1=ON(蓝高亮) 0=OFF(灰暗显) */
static uint8_t s_btn_state = 0;

/* 当前亮度百分比(缓存, 初始与背光驱动默认一致) */
static int8_t s_bl_value = 100;

/* 按钮两态配色 */
#define UI_BTN_ON_COLOR    lv_color_hex(0x2196F3)   /* ON: 蓝色高亮 */
#define UI_BTN_OFF_COLOR   lv_color_hex(0x424242)   /* OFF: 灰色暗显 */

/*********************************************************************
 * 函 数 名: app_ui_bl_refresh
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 按当前亮度值同步刷新 UI(大号文字/亮度条/提示)与 PWM 输出
 * 说    明: 亮度限幅由驱动层保证(20~100), UI 与硬件共用一份缓存值
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(旋转联动背光需求)
 **********************************************************************/
static void app_ui_bl_refresh(void)
{
  /* ---- Step1: 驱动层限幅并更新 PWM 输出 ---- */
  backlight_set_percent((uint8_t)s_bl_value);
  s_bl_value = (int8_t)backlight_get_percent();   /* 回读限幅后的实际值 */

  /* ---- Step2: 大号百分比文字 ---- */
  lv_label_set_text_fmt(s_value_label, "%d%%", (int)s_bl_value);

  /* ---- Step3: 亮度条与下方提示 ---- */
  lv_bar_set_value(s_bl_bar, s_bl_value, LV_ANIM_OFF);
  lv_label_set_text_fmt(s_bl_label, "Backlight PWM: %d%%", (int)s_bl_value);
}

/*********************************************************************
 * 函 数 名: app_ui_btn_refresh
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 按翻转状态刷新按钮外观(背景色/文字)
 * 说    明: ON 态蓝色高亮, OFF 态灰色暗显, 每次点按沿事件后调用
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(点按按钮变化需求)
 **********************************************************************/
static void app_ui_btn_refresh(void)
{
  /* ---- Step1: 按状态切换按钮背景色 ---- */
  if(s_btn_state != 0)
  {
    lv_obj_set_style_bg_color(s_btn, UI_BTN_ON_COLOR, 0);
    lv_label_set_text(s_btn_label, "BTN: ON");
  }
  else
  {
    lv_obj_set_style_bg_color(s_btn, UI_BTN_OFF_COLOR, 0);
    lv_label_set_text(s_btn_label, "BTN: OFF");
  }
}

/*********************************************************************
 * 函 数 名: debug_pin_timer_cb
 * 入口参数: timer       定时器句柄(未使用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 100ms 周期刷新编码器三路引脚电平自检行
 * 说    明: 硬件排查利器——A/B 空闲应显示 1(外部上拉 3.3V), P 松开为 1;
 *           旋转/按下时对应数字应实时翻转; 若恒定不变说明信号未进 MCU
 *           (检查接线/JTAG 释放/上拉电源)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(编码器问题排查)
 **********************************************************************/
static void debug_pin_timer_cb(lv_timer_t *timer)
{
  uint8_t pin_a = 0;                            // CH_A 电平
  uint8_t pin_b = 0;                            // CH_B 电平
  uint8_t pin_p = 0;                            // PUSH 电平

  (void)timer;

  /* ---- Step1: 读取驱动层 1ms 采样的引脚电平缓存 ---- */
  encoder_get_pin_levels(&pin_a, &pin_b, &pin_p);

  /* ---- Step2: 更新自检行文字 ---- */
  lv_label_set_text_fmt(s_debug_label, "A:%d B:%d P:%d",
                        (int)pin_a, (int)pin_b, (int)pin_p);
}

/*********************************************************************
 * 函 数 名: app_ui_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 创建交互演示界面(标题/亮度文字/亮度条/翻转按钮/自检行/提示)
 * 说    明: 必须在 LVGL 任务首次刷新前调用(调度器启动前的 main 中
 *           调用, 创建阶段无并发访问风险)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(旋转调背光 + 点按翻转按钮交互);
 *             新增编码器引脚电平自检行(硬件排查)
 **********************************************************************/
void app_ui_init(void)
{
  /* ---- Step1: 顶部标题 ---- */
  s_title_label = lv_label_create(lv_scr_act());
  lv_label_set_text(s_title_label, "Encoder Demo");
  lv_obj_set_style_text_color(s_title_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_28, 0);
  lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 4);

  /* ---- Step2: 中部大号亮度百分比文字 ---- */
  s_value_label = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(s_value_label, lv_color_hex(0xFFC107), 0);
  lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_28, 0);
  lv_obj_align(s_value_label, LV_ALIGN_TOP_MID, 0, 42);

  /* ---- Step3: 背光亮度条(20~100%)与提示文字 ---- */
  s_bl_bar = lv_bar_create(lv_scr_act());
  lv_obj_set_size(s_bl_bar, 200, 16);
  lv_bar_set_range(s_bl_bar, BACKLIGHT_MIN_PERCENT, BACKLIGHT_MAX_PERCENT);
  lv_obj_set_style_bg_color(s_bl_bar, UI_BTN_ON_COLOR, LV_PART_INDICATOR);
  lv_obj_align(s_bl_bar, LV_ALIGN_CENTER, 0, -10);

  s_bl_label = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(s_bl_label, lv_color_white(), 0);
  lv_obj_align(s_bl_label, LV_ALIGN_CENTER, 0, -10);

  /* ---- Step4: 点按翻转按钮(初始 OFF 灰色) ---- */
  s_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(s_btn, 140, 44);
  lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0, -44);
  lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);   /* 输入来自编码器, 禁触屏点击 */

  s_btn_label = lv_label_create(s_btn);               /* 按钮文字(子对象) */
  lv_obj_center(s_btn_label);

  /* ---- Step5: 底部操作提示 ---- */
  s_hint_label = lv_label_create(lv_scr_act());
  lv_label_set_text(s_hint_label, "Rotate: backlight\nPush: toggle button");
  lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x9E9E9E), 0);
  lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -6);

  /* ---- Step5.1: 引脚电平自检行(右上角, 硬件排查用, 100ms 刷新) ---- */
  s_debug_label = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(s_debug_label, lv_color_hex(0x4CAF50), 0);
  lv_label_set_text(s_debug_label, "A:- B:- P:-");
  lv_obj_align(s_debug_label, LV_ALIGN_TOP_RIGHT, -4, 4);
  lv_timer_create(debug_pin_timer_cb, 100, NULL);

  /* ---- Step5.2: SGP30 传感器显示标签(亮度大字与亮度条之间, 左右分列) ---- */
  s_eco2_label = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(s_eco2_label, lv_color_hex(0x4CAF50), 0);
  lv_label_set_text(s_eco2_label, "eCO2: -- ppm");
  lv_obj_align(s_eco2_label, LV_ALIGN_TOP_LEFT, 8, 78);

  s_tvoc_label = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(s_tvoc_label, lv_color_hex(0x26C6DA), 0);
  lv_label_set_text(s_tvoc_label, "TVOC: -- ppb");
  lv_obj_align(s_tvoc_label, LV_ALIGN_TOP_RIGHT, -8, 78);

  /* ---- Step6: 刷新初始显示(亮度 100%, 按钮 OFF) ---- */
  app_ui_bl_refresh();
  app_ui_btn_refresh();
}

/*********************************************************************
 * 函 数 名: app_ui_encoder_event
 * 入口参数: delta        编码器旋转增量(4 倍频计步, 正=顺时针)
 *           push_pressed 按键"松开->按下"沿事件(1=有)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 处理编码器事件: 旋转调节背光亮度(UI 条同步), 点按翻转按钮
 * 说    明: 仅允许 LVGL 任务调用(app_main.c 的 lvgl_task 队列消费处);
 *           EC11 类编码器每格产生 4 个计数, /2 后每格步进 2% 亮度
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(旋转调背光 + 点按翻转按钮交互)
 **********************************************************************/
void app_ui_encoder_event(int32_t delta, int32_t push_pressed)
{
  /* ---- Step1: 旋转增量 -> 背光亮度(每格 2%), UI 与 PWM 同步刷新 ---- */
  if(delta != 0)
  {
    s_bl_value = (int8_t)(s_bl_value + delta / 2);
    if(s_bl_value > BACKLIGHT_MAX_PERCENT){ s_bl_value = BACKLIGHT_MAX_PERCENT; }
    if(s_bl_value < BACKLIGHT_MIN_PERCENT){ s_bl_value = BACKLIGHT_MIN_PERCENT; }
    app_ui_bl_refresh();
  }

  /* ---- Step2: 点按沿事件 -> 翻转按钮状态并刷新外观 ---- */
  if(push_pressed != 0)
  {
    s_btn_state = (uint8_t)(s_btn_state ? 0 : 1);
    app_ui_btn_refresh();
  }
}

/*********************************************************************
 * 函 数 名: app_ui_sgp30_update
 * 入口参数: eco2    eCO2 浓度(ppm)
 *           tvoc    TVOC 浓度(ppb)
 *           valid   1=本次数据有效 0=读取失败
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 刷新 SGP30 传感器的 eCO2/TVOC 显示标签
 * 说    明: 仅允许 LVGL 任务调用(app_main.c 的 lvgl_task 队列消费处);
 *           读取失败/模块未就绪时显示 "--" 便于区分; 传感器初始化后
 *           约 15s 预热期内固定显示 400ppm/0ppb, 属数据手册定义的正常
 *           现象, 并非驱动异常
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求)
 **********************************************************************/
void app_ui_sgp30_update(uint32_t eco2, uint32_t tvoc, uint32_t valid)
{
  /* ---- Step1: 数据有效 -> 显示 eCO2 与 TVOC 实测值 ---- */
  if(valid != 0U)
  {
    lv_label_set_text_fmt(s_eco2_label, "eCO2: %u ppm", (unsigned)eco2);
    lv_label_set_text_fmt(s_tvoc_label, "TVOC: %u ppb", (unsigned)tvoc);
  }
  /* ---- Step2: 数据无效 -> 占位符提示, 不显示陈旧数据 ---- */
  else
  {
    lv_label_set_text(s_eco2_label, "eCO2: -- ppm");
    lv_label_set_text(s_tvoc_label, "TVOC: -- ppb");
  }
}
