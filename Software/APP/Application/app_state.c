/**
  **************************************************************************
  * @file     app_state.c
  * @brief    应用层状态机实现（状态表驱动 + 按键触发函数指针分发）
  *
  * 状态流转图：
  *   [空状态 OFF] --(PB3 停机唤醒/首次开机)--> [开机过渡 POWER_ON]
  *   [POWER_ON]   --(300ms 吞键窗口到时)------> [开机 LOGO]
  *   [LOGO]       --(1.5s 到时 / 短按跳过)-----> [骑行仪表盘 DASH]
  *   [DASH]       --(M 键短按, 左滑)-----------> [指南针 COMPASS]
  *   [COMPASS]    --(M 键短按, 右滑)-----------> [DASH(固定顺序循环)]
  *   [DASH/COMPASS] --(长按 3s 关机)-----------> [空状态 OFF]
  *
  * 分层约定：
  *   1. 按键扫描在驱动层(driver_encoder, TMR7 1ms 消抖采样),
  *      本模块只消费语义事件(短按/长按/保持进度), 经 lvgl_task
  *      队列消费处调用 —— 应用层不解析按键电平;
  *   2. 每个状态在状态表中注册 on_enter/on_exit/key_handler 三个
  *      函数指针: on_enter 负责加载对应 UI 页面(满足"状态跳转时的
  *      页面加载在状态机里实现"), key_handler 负责本状态的按键语义;
  *   3. 关机进度提示为各运行页面公共行为, 在事件分发入口统一调用
  *      app_ui_power_hint() 处理; 真实停机序列(熄屏->等松手->停机)
  *      由 encoder_task 执行, 本模块仅在 LONG_PRESS 时切到 OFF 态;
  *   4. 线程约束: 全部 API 仅在 LVGL 任务上下文调用(main 初始化
  *      阶段调用 app_state_init 时调度器未启动, 无并发风险)。
  **************************************************************************
  */

#include "app_state.h"
#include "app_ui.h"
#include "driver_encoder.h"
#include "FreeRTOS.h"
#include "task.h"

/* 开机 LOGO 页显示时长(ms): 到时自动跳转骑行仪表盘 */
#define APP_STATE_LOGO_DURATION_MS   1500U

/* 开机过渡态吞键窗口时长(ms): 躲开唤醒按键沿, 避免开机即跳过 LOGO */
#define APP_STATE_POWER_ON_MS        300U

/* 页面循环表: 短按 M 键按此固定顺序循环切换(未来扩展页面只需增补表项) */
static const app_state_id_t s_page_cycle[2] =
{
  APP_STATE_DASH,       /* 循环第 1 页: 骑行仪表盘 */
  APP_STATE_COMPASS     /* 循环第 2 页: 指南针 */
};

#define APP_STATE_PAGE_CYCLE_NUM  (sizeof(s_page_cycle) / sizeof(s_page_cycle[0]))

/* 当前状态(仅 LVGL 任务访问, 无需保护) */
static app_state_id_t s_cur_state = APP_STATE_OFF;

/* 进入当前状态的时刻(tick): 供过渡延时/LOGO 计时使用 */
static uint32_t s_enter_tick = 0U;

/* 唤醒吞键标志: 唤醒后的首个短按沿丢弃(与唤醒下降沿是同一次按压) */
static uint8_t s_swallow_press = 0U;

/* ---------------- 前向声明(状态表引用的各状态回调) ---------------- */
static void st_off_enter(void);
static void st_off_key(app_state_key_event_t evt, uint32_t hold_ms);
static void st_power_on_enter(void);
static void st_power_on_key(app_state_key_event_t evt, uint32_t hold_ms);
static void st_logo_enter(void);
static void st_logo_exit(void);
static void st_logo_key(app_state_key_event_t evt, uint32_t hold_ms);
static void st_dash_enter(void);
static void st_dash_key(app_state_key_event_t evt, uint32_t hold_ms);
static void st_compass_enter(void);
static void st_compass_key(app_state_key_event_t evt, uint32_t hold_ms);

/* ---------------- 状态表: 按状态下标注册回调函数指针 ---------------- */
// 状态操作表: 与 app_state_id_t 枚举一一对应,
// 每个状态注册自己的页面加载(on_enter)与按键触发(key_handler)函数
static const app_state_ops_t s_state_ops[APP_STATE_NUM] =
{
  /* APP_STATE_OFF: 空状态(停机), 不加载页面, 忽略一切按键 */
  { st_off_enter,          NULL,           st_off_key          },
  /* APP_STATE_POWER_ON: 开机过渡态, 加载 LOGO 页并吞掉唤醒按键沿 */
  { st_power_on_enter,     NULL,           st_power_on_key     },
  /* APP_STATE_LOGO: 开机 LOGO 态, 短按跳过, 到时自动进仪表盘 */
  { st_logo_enter,         st_logo_exit,   st_logo_key         },
  /* APP_STATE_DASH: 骑行仪表盘态, M 键左滑进指南针, 长按关机 */
  { st_dash_enter,         NULL,           st_dash_key         },
  /* APP_STATE_COMPASS: 指南针态, M 键右滑回仪表盘(固定顺序循环) */
  { st_compass_enter,      NULL,           st_compass_key      }
};

/*********************************************************************
 * 函 数 名: sm_transition
 * 入口参数: next    目标状态
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 执行状态跳转: 调用旧状态 on_exit -> 切状态 ->
 *           记录进入时刻 -> 调用新状态 on_enter(加载对应 UI 页面)
 * 说    明: 仅本文件内部使用; 进入 DASH 后清吞键标志(LOGO 已结束,
 *           后续短按均为有效 M 键)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
static void sm_transition(app_state_id_t next)
{
  /* ---- Step1: 旧状态退出回调(未注册则跳过) ---- */
  if(s_state_ops[s_cur_state].on_exit != NULL)
  {
    s_state_ops[s_cur_state].on_exit();
  }

  /* ---- Step2: 切换状态并记录进入时刻(供过渡延时/LOGO 计时) ---- */
  s_cur_state = next;
  s_enter_tick = (uint32_t)xTaskGetTickCount();

  /* ---- Step3: LOGO 结束进入运行页面后, 唤醒吞键窗口关闭 ---- */
  if((next == APP_STATE_DASH) || (next == APP_STATE_COMPASS))
  {
    s_swallow_press = 0U;
  }

  /* ---- Step4: 新状态进入回调(加载对应 UI 页面) ---- */
  if(s_state_ops[next].on_enter != NULL)
  {
    s_state_ops[next].on_enter();
  }
}

/*********************************************************************
 * 函 数 名: page_cycle_advance
 * 入口参数: cur     当前页面态(DASH 或 COMPASS)
 * 出口参数: 无
 * 返 回 值: app_state_id_t 循环表中的下一页面态
 * 函数功能: 按固定顺序返回页面循环表的下一个页面(短按 M 键的目标页)
 * 说    明: 当前两页为 仪表盘<->指南针; 未来扩展页面只需增补
 *           s_page_cycle 表项, 本函数与状态表无需改动
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(多页面循环切换需求)
 **********************************************************************/
static app_state_id_t page_cycle_advance(app_state_id_t cur)
{
  uint32_t i;                                            // 循环索引

  /* ---- Step1: 在循环表中定位当前页面 ---- */
  for(i = 0U; i < APP_STATE_PAGE_CYCLE_NUM; i++)
  {
    if(s_page_cycle[i] == cur)
    {
      break;
    }
  }

  /* ---- Step2: 未命中(异常防御)默认回仪表盘, 否则取下一项(回绕) ---- */
  if(i >= APP_STATE_PAGE_CYCLE_NUM)
  {
    return APP_STATE_DASH;
  }
  return s_page_cycle[(i + 1U) % APP_STATE_PAGE_CYCLE_NUM];
}

/* ================= 各状态回调实现 ================= */

/*********************************************************************
 * 函 数 名: st_off_enter / st_off_key
 * 入口参数: evt       按键语义事件(未使用)
 *           hold_ms   长按保持时长(未使用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 空状态(停机关机态): 不加载页面, 忽略一切按键事件;
 *           停机期间任务冻结, 本状态仅作为状态机的逻辑记录
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
static void st_off_enter(void)
{
}

static void st_off_key(app_state_key_event_t evt, uint32_t hold_ms)
{
  (void)evt;
  (void)hold_ms;
}

/*********************************************************************
 * 函 数 名: st_power_on_enter / st_power_on_key
 * 入口参数: evt       按键语义事件(未使用)
 *           hold_ms   长按保持时长(未使用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 开机过渡态: 进入时加载 LOGO 页(无动画)并置吞键标志;
 *           本状态忽略一切按键(躲开唤醒按压沿), 吞键窗口(300ms)
 *           到时由 app_state_tick 切入 LOGO 态开始计时
 * 说    明: 唤醒下降沿与短按沿共用 PB3, 唤醒后的首个按键沿属同
 *           一次按压, 须丢弃避免"开机即跳过 LOGO"
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
static void st_power_on_enter(void)
{
  /* ---- Step1: 置吞键标志(窗口内的按键沿丢弃) ---- */
  s_swallow_press = 1U;

  /* ---- Step2: 加载 LOGO 页(无动画, 过渡期即显示开机画面) ---- */
  app_ui_page_show(APP_UI_PAGE_LOGO, APP_UI_ANIM_NONE);
}

static void st_power_on_key(app_state_key_event_t evt, uint32_t hold_ms)
{
  (void)evt;
  (void)hold_ms;
}

/*********************************************************************
 * 函 数 名: st_logo_enter / st_logo_exit / st_logo_key
 * 入口参数: evt       按键语义事件
 *           hold_ms   长按保持时长
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 开机 LOGO 态: 进入时加载 LOGO 页并开始计时, 1.5s 到时
 *           由 app_state_tick 自动跳转仪表盘; 短按跳过 LOGO 直接
 *           进仪表盘; 长按仍可关机(公共提示走分发入口)
 * 说    明: LOGO 显示时长 = 过渡态 300ms + LOGO 态 1500ms
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
static void st_logo_enter(void)
{
  app_ui_page_show(APP_UI_PAGE_LOGO, APP_UI_ANIM_NONE);
}

static void st_logo_exit(void)
{
}

static void st_logo_key(app_state_key_event_t evt, uint32_t hold_ms)
{
  (void)hold_ms;

  /* ---- Step1: 短按跳过 LOGO, 直接进骑行仪表盘 ---- */
  if(evt == APP_KEY_SHORT_PRESS)
  {
    sm_transition(APP_STATE_DASH);
  }

  /* ---- Step2: 长按 3s -> 关机切空状态(停机序列由 encoder_task 执行) ---- */
  if(evt == APP_KEY_LONG_PRESS)
  {
    sm_transition(APP_STATE_OFF);
  }
  /* ---- Step3: 保持事件由分发入口统一处理提示, 此处无动作 ---- */
}

/*********************************************************************
 * 函 数 名: st_dash_enter / st_dash_key
 * 入口参数: evt       按键语义事件
 *           hold_ms   长按保持时长
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 骑行仪表盘态: 进入时右滑加载仪表盘页; M 键短按左滑切
 *           指南针(固定顺序循环); 长按 3s 关机切空状态
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
static void st_dash_enter(void)
{
  /* ---- Step1: 加载骑行仪表盘页(从指南针切回时右滑过场) ---- */
  app_ui_page_show(APP_UI_PAGE_DASH, APP_UI_ANIM_RIGHT);
}

static void st_dash_key(app_state_key_event_t evt, uint32_t hold_ms)
{
  (void)hold_ms;

  /* ---- Step1: M 键短按 -> 循环表下一页(当前为指南针, 左滑过场) ---- */
  if(evt == APP_KEY_SHORT_PRESS)
  {
    sm_transition(page_cycle_advance(APP_STATE_DASH));
  }

  /* ---- Step2: 长按 3s -> 关机切空状态(停机序列由 encoder_task 执行) ---- */
  if(evt == APP_KEY_LONG_PRESS)
  {
    sm_transition(APP_STATE_OFF);
  }
}

/*********************************************************************
 * 函 数 名: st_compass_enter / st_compass_key
 * 入口参数: evt       按键语义事件
 *           hold_ms   长按保持时长
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 指南针态: 进入时左滑加载指南针页; M 键短按按循环表切回
 *           仪表盘(右滑过场); 长按 3s 关机切空状态
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
static void st_compass_enter(void)
{
  /* ---- Step1: 加载指南针页(从仪表盘切入时左滑过场) ---- */
  app_ui_page_show(APP_UI_PAGE_COMPASS, APP_UI_ANIM_LEFT);
}

static void st_compass_key(app_state_key_event_t evt, uint32_t hold_ms)
{
  (void)hold_ms;

  /* ---- Step1: M 键短按 -> 循环表下一页(当前为仪表盘, 右滑过场) ---- */
  if(evt == APP_KEY_SHORT_PRESS)
  {
    sm_transition(page_cycle_advance(APP_STATE_COMPASS));
  }

  /* ---- Step2: 长按 3s -> 关机切空状态(停机序列由 encoder_task 执行) ---- */
  if(evt == APP_KEY_LONG_PRESS)
  {
    sm_transition(APP_STATE_OFF);
  }
}

/* ================= 对外 API ================= */

/*********************************************************************
 * 函 数 名: app_state_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 初始化状态机: 复位为空状态, 再视同"按键唤醒"走首次
 *           开机流程(POWER_ON 吞键窗口 -> LOGO -> 仪表盘)
 * 说    明: 在 main() 中 app_ui_page_create() 之后调用(调度器启动
 *           前, LVGL 调用无并发风险); 上电默认关机态由
 *           lowpower_boot_wait() 保证, 返回即"已按键开机"
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
void app_state_init(void)
{
  /* ---- Step1: 复位为空状态(尚未注册任何页面显示) ---- */
  s_cur_state = APP_STATE_OFF;
  s_swallow_press = 0U;
  s_enter_tick = 0U;

  /* ---- Step2: 视同按键唤醒, 走 POWER_ON -> LOGO 开机流程 ---- */
  app_state_wakeup();
}

/*********************************************************************
 * 函 数 名: app_state_key_event
 * 入口参数: evt       按键语义事件(短按/长按/保持进度)
 *           hold_ms   长按保持时长(ms, 松开归 0)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 按键语义事件分发入口: 关机进度提示为各运行页面公共
 *           行为统一在此处理; 短按在吞键窗口内丢弃(唤醒沿);
 *           其余事件分发给当前状态注册的 key_handler 函数指针
 * 说    明: 仅允许 LVGL 任务调用(lvgl_task 的队列消费处)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
void app_state_key_event(app_state_key_event_t evt, uint32_t hold_ms)
{
  /* ---- Step1: 长按关机进度提示(含松开归 0 隐藏), 各状态行为一致 ---- */
  if((evt == APP_KEY_HOLD_PROGRESS) || (evt == APP_KEY_LONG_PRESS))
  {
    app_ui_power_hint((int32_t)hold_ms);
  }

  /* ---- Step2: 唤醒吞键窗口内的首个短按沿丢弃(与唤醒同一次按压) ---- */
  if((evt == APP_KEY_SHORT_PRESS) && (s_swallow_press != 0U))
  {
    s_swallow_press = 0U;
    return;
  }

  /* ---- Step3: 分发给当前状态注册的按键触发函数指针 ---- */
  s_state_ops[s_cur_state].key_handler(evt, hold_ms);
}

/*********************************************************************
 * 函 数 名: app_state_wakeup
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 停机唤醒开机事件: 清屏残留的关机提示后切入开机过渡态
 *           (POWER_ON 吞键窗口 -> LOGO -> 仪表盘, 重走完整开机流程)
 * 说    明: 仅允许 LVGL 任务调用; 只在空状态下响应(防止误报的
 *           唤醒消息打断运行中的页面)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
void app_state_wakeup(void)
{
  /* ---- Step1: 仅空状态(停机唤醒)响应, 其他状态忽略 ---- */
  if(s_cur_state != APP_STATE_OFF)
  {
    return;
  }

  /* ---- Step2: 清除停机前残留的关机提示, 熄屏状态归位 ---- */
  app_ui_power_hint(0);

  /* ---- Step3: 切入开机过渡态(加载 LOGO 并吞掉唤醒按键沿) ---- */
  sm_transition(APP_STATE_POWER_ON);
}

/*********************************************************************
 * 函 数 名: app_state_tick
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 状态机周期调度(由 lvgl_task 每周期调用):
 *           1. POWER_ON 态吞键窗口到时(300ms) -> LOGO 态;
 *           2. LOGO 态显示时长到时(1500ms) -> 骑行仪表盘态
 * 说    明: 仅允许 LVGL 任务调用; 计时基于 xTaskGetTickCount
 *           (1ms tick, 与 LV_TICK_CUSTOM 同源)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
void app_state_tick(void)
{
  uint32_t elapsed;   // 当前状态已停留时长(ms)

  /* ---- Step1: 计算当前状态已停留时长(tick 回绕安全的无符号减法) ---- */
  elapsed = (uint32_t)xTaskGetTickCount() - s_enter_tick;

  /* ---- Step2: 开机过渡态: 吞键窗口到时切入 LOGO 开始计时 ---- */
  if(s_cur_state == APP_STATE_POWER_ON)
  {
    if(elapsed >= APP_STATE_POWER_ON_MS)
    {
      sm_transition(APP_STATE_LOGO);
    }
  }
  /* ---- Step3: LOGO 态: 显示时长到时自动进入骑行仪表盘 ---- */
  else if(s_cur_state == APP_STATE_LOGO)
  {
    if(elapsed >= APP_STATE_LOGO_DURATION_MS)
    {
      sm_transition(APP_STATE_DASH);
    }
  }
  else
  {
    /* 运行页面/空状态无周期性动作 */
  }
}

/*********************************************************************
 * 函 数 名: app_state_get
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: app_state_id_t 当前状态
 * 函数功能: 查询状态机当前状态(调试/联动用)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月13日
 *     修改内容: 新增函数(状态机需求)
 **********************************************************************/
app_state_id_t app_state_get(void)
{
  return s_cur_state;
}
