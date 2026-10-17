/**
  **************************************************************************
  * @file     app_ui.c
  * @brief    应用层交互演示 UI 实现（环形仪表盘动画 + 编码器联动）
  *
  * 交互设计（240x240 深色主题, 酷炫动画版）：
  *   - 屏幕中央 270° 环形仪表盘(lv_arc)：弧长=背光亮度(20~100%)；
  *     旋转编码器时弧值以 400ms 缓动动画平滑扫动(非瞬时跳变)；
  *     指示弧颜色随亮度三段渐变(20% 紫 -> 60% 青 -> 100% 荧光绿)；
  *   - 弧心大号百分比数字：旋转时上抛-回弹弹跳动画, 颜色与弧同步渐变；
  *   - 弧心下方两行小字显示 SGP30 的 eCO2/TVOC 实时值；
  *   - 屏幕下部按钮(BTN)：点按编码器时状态翻转——背景色 300ms 渐变
  *     过渡动画(灰<->蓝) + 按钮整体弹跳缩放；
  *   - 长按 3s 关机时顶部红色进度提示, 松手未达阈值自动隐藏。
  *
  * 动画实现要点：
  *   - 弧值平滑: lv_anim 对 lv_arc_set_value 做缓动插值, 快速旋转时
  *     每次从"动画中间值"重新起跳, 视觉上连续追踪目标(指针跟手感);
  *   - 弹跳: translate_y 两段式(0 -> -12px -> 0), 纯视觉位移不改变
  *     对象布局 offset 与 align, 弹跳结束自动回到对齐基线(无漂移);
  *   - 颜色渐变: lv_color_mix 按动画进度在起止色间插值。
  *
  * 线程约束：所有 LVGL 接口仅由 LVGL 任务调用（app_main.c 的 lvgl_task）,
  * 编码器事件经 FreeRTOS 队列送达, 本模块不做任何跨线程 UI 访问。
  **************************************************************************
  */

#include "app_ui.h"
#include "backlight.h"
#include "encoder.h"
#include "encoder_accel.h"

/* 界面控件句柄(LVGL 任务上下文访问, 无需保护) */
static lv_obj_t  *s_title_label;      // 顶部标题
static lv_obj_t  *s_bl_arc;           // 中央环形仪表盘(270° 扫弧)
static lv_obj_t  *s_value_label;      // 弧心大号亮度百分比文字(弹跳动画)
static lv_obj_t  *s_btn;              // 点按翻转按钮
static lv_obj_t  *s_btn_label;       // 按钮文字(子对象)
static lv_obj_t  *s_hint_label;      // 底部操作提示
static lv_obj_t  *s_debug_label;     // 引脚电平自检行(硬件排查用)
static lv_obj_t  *s_eco2_label;      // SGP30 eCO2 显示(弧心小字上)
static lv_obj_t  *s_tvoc_label;      // SGP30 TVOC 显示(弧心小字下)
static lv_obj_t  *s_power_label;     // 长按关机进度提示(默认隐藏)

/* 按钮翻转状态: 1=ON(蓝高亮) 0=OFF(灰暗显) */
static uint8_t s_btn_state = 0;

/* 当前亮度百分比(缓存, 初始与背光驱动默认一致) */
static int8_t s_bl_value = 100;

/* 背光调节的编码器变速器实例(变速逻辑见 encoder_accel 通用模块) */
static encoder_accel_t s_bl_accel;

/* 按钮颜色渐变动画的起止色(动画回调中取用) */
static lv_color_t s_btn_color_from;
static lv_color_t s_btn_color_to;

/* 按钮两态配色 */
#define UI_BTN_ON_COLOR    lv_color_hex(0x64DD17)   /* ON: 荧光绿 */
#define UI_BTN_OFF_COLOR   lv_color_hex(0x424242)   /* OFF: 灰色暗显 */

/* 长按关机进度提示的出现阈值(ms): 按住超过该时长开始显示提示 */
#define UI_PWR_HINT_MS     800

/* 动画参数
 * 说明: 数字弹跳不用 lv_obj_set_y 直接改坐标——本工程 lv_obj_align
 * 会写入 align 样式(如 LV_ALIGN_CENTER), 布局刷新时 lv_obj_refr_pos
 * 在 offset 基础上再叠加居中偏移; 而 lv_obj_get_y 返回的已是叠加后的
 * 实际坐标, 若将其回传给 lv_obj_set_y 会二次叠加居中偏移, 导致数字
 * 每次弹跳都向下漂移并最终移出可见区(即"刚开始有效果后来消失")。
 * 故改用 translate_y(纯视觉位移, 布局中与 offset 相加但不改 align)
 * 实现弹跳; 弧宽呼吸/阴影发光同理均为与布局无关的通用样式 */
#define UI_ANIM_ARC_MS     400    /* 弧值平滑动画时长(ms) */
#define UI_ANIM_BTN_MS     300    /* 按钮颜色渐变时长(ms) */
#define UI_BOUNCE_UP_MS    130    /* 上抛/膨胀段时长(ms) */
#define UI_BOUNCE_BACK_MS  600    /* 回弹段时长(ms, bounce 路径带衰减反弹) */
#define UI_NUM_LIFT_PX     4     /* 数字上抛像素 */
#define UI_ARC_W_NORMAL    8     /* 仪表弧常规宽度(px) */
#define UI_ARC_W_PULSE     15     /* 仪表弧脉冲峰值宽度(px, 膨胀感) */
#define UI_BTN_GLOW_MAX    24     /* 按钮发光阴影峰值宽度(px) */
#define UI_BTN_GLOW_UP_MS  150    /* 发光上升段时长(ms) */

/*********************************************************************
 * 函 数 名: bl_level_color
 * 入口参数: value   亮度百分比(20~100)
 * 出口参数: 无
 * 返 回 值: lv_color_t 亮度对应的渐变色
 * 函数功能: 计算仪表盘指示弧颜色: 20% 紫 -> 60% 亮青 -> 100% 荧光绿
 * 说    明: 两段线性插值, 低亮度偏冷紫提示"暗", 高亮度荧光绿提示"满",
 *           与大号数字颜色共用, 保证弧与文字同色联动
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static lv_color_t bl_level_color(int32_t value)
{
  const lv_color_t c_lo  = lv_color_hex(0x7C4DFF);   /* 20%: 深紫 */
  const lv_color_t c_mid = lv_color_hex(0x00E5FF);   /* 60%: 亮青 */
  const lv_color_t c_hi  = lv_color_hex(0x64DD17);   /* 100%: 荧光绿 */
  uint8_t ratio;

  if(value <= 60)
  {
    ratio = (uint8_t)(((value - 20) * 255) / 40);     /* 20~60: 紫 -> 青 */
    return lv_color_mix(c_mid, c_lo, ratio);          /* ratio=0 全紫 */
  }
  ratio = (uint8_t)(((value - 60) * 255) / 40);       /* 60~100: 青 -> 绿 */
  return lv_color_mix(c_hi, c_mid, ratio);           /* ratio=255 全绿 */
}

/*********************************************************************
 * 函 数 名: bl_arc_anim_cb
 * 入口参数: var   环形仪表盘对象
 *           v     插值中的弧值
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 弧值缓动动画执行回调: 将插值写入仪表盘
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static void bl_arc_anim_cb(void *var, int32_t v)
{
  lv_arc_set_value((lv_obj_t *)var, (int16_t)v);
}

/*********************************************************************
 * 函 数 名: bl_arc_anim_to
 * 入口参数: target   弧值动画目标(20~100)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 启动弧值缓动动画(从当前显示值平滑扫向目标值)
 * 说    明: 每次调用 lv_anim_start 会自动替换同对象同回调的旧动画,
 *           快速旋转时从"动画中间值"重新起跳, 弧连续追踪目标;
 *           当前值==目标值时动画无视觉效果(开机首刷路径安全)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static void bl_arc_anim_to(int32_t target)
{
  lv_anim_t a;

  lv_anim_init(&a);
  lv_anim_set_var(&a, s_bl_arc);
  lv_anim_set_exec_cb(&a, bl_arc_anim_cb);
  lv_anim_set_values(&a, (int32_t)lv_arc_get_value(s_bl_arc), target);
  lv_anim_set_time(&a, UI_ANIM_ARC_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

/*********************************************************************
 * 函 数 名: obj_y_anim_cb
 * 入口参数: var   目标对象
 *           v     插值中的垂直视觉位移(translate_y, px, 负=上移)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 弹跳动画执行回调: 将插值写入对象 translate_y 样式
 * 说    明: 用 translate_y 而非 lv_obj_set_y: 后者改的是布局 offset,
 *           会在布局刷新时再次叠加 align 居中偏移造成数字漂移
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 *              lv_obj_set_y 改为 lv_obj_set_style_translate_y(漂移修复)
 **********************************************************************/
static void obj_y_anim_cb(void *var, int32_t v)
{
  lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/*********************************************************************
 * 函 数 名: obj_y_ready_cb
 * 入口参数: a   第一段(上抛)动画句柄(取 var 复用对象)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 弹跳第二段: 上抛到位后以 bounce 路径回落至 translate_y=0
 *           (数字落地再弹两下的"回弹"效果, 归零后自动回到对齐基线)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 *              回落终点由绝对坐标改为 translate_y=0(漂移修复)
 **********************************************************************/
static void obj_y_ready_cb(lv_anim_t *a)
{
  lv_anim_t b;

  lv_anim_init(&b);
  lv_anim_set_var(&b, a->var);
  lv_anim_set_exec_cb(&b, obj_y_anim_cb);
  lv_anim_set_values(&b, -UI_NUM_LIFT_PX, 0);             /* 峰值 -> 基线 */
  lv_anim_set_time(&b, UI_BOUNCE_BACK_MS);
  lv_anim_set_path_cb(&b, lv_anim_path_bounce);           /* 衰减反弹 */
  lv_anim_start(&b);
}

/*********************************************************************
 * 函 数 名: obj_y_bounce
 * 入口参数: obj        弹跳目标对象
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 对象垂直弹跳动画: translate_y 从当前值快速上抛 12px,
 *           再以 bounce 路径回落到 0(对齐基线)
 * 说    明: 全程只改 translate_y(视觉位移), 不动布局 offset 与 align,
 *           故不会像 lv_obj_set_y 那样叠加居中偏移导致数字漂移;
 *           段1从当前 translate_y 起跳, 快速重复调用时 lv_anim_start
 *           自动删除旧动画, 视觉连续无跳变
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 *              改用 translate_y 实现, 去掉 base_y 参数(漂移修复)
 **********************************************************************/
static void obj_y_bounce(lv_obj_t *obj)
{
  lv_anim_t a;
  int32_t cur = (int32_t)lv_obj_get_style_translate_y(obj, 0);

  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, obj_y_anim_cb);
  lv_anim_set_values(&a, cur, -UI_NUM_LIFT_PX);              /* 当前 -> 上抛峰值 */
  lv_anim_set_time(&a, UI_BOUNCE_UP_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, obj_y_ready_cb);
  lv_anim_start(&a);
}

/*********************************************************************
 * 函 数 名: arc_width_anim_cb
 * 入口参数: var   环形仪表盘对象
 *           v     插值中的弧宽度
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 弧宽呼吸动画执行回调: 同步写背景弧与指示弧宽度
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static void arc_width_anim_cb(void *var, int32_t v)
{
  lv_obj_set_style_arc_width((lv_obj_t *)var, (lv_coord_t)v, LV_PART_MAIN);
  lv_obj_set_style_arc_width((lv_obj_t *)var, (lv_coord_t)v, LV_PART_INDICATOR);
}

/*********************************************************************
 * 函 数 名: arc_width_ready_cb
 * 入口参数: a   第一段(膨胀)动画句柄
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 弧宽呼吸第二段: 膨胀到位后以 bounce 路径回落常规宽度
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static void arc_width_ready_cb(lv_anim_t *a)
{
  lv_anim_t b;

  (void)a;
  lv_anim_init(&b);
  lv_anim_set_var(&b, s_bl_arc);
  lv_anim_set_exec_cb(&b, arc_width_anim_cb);
  lv_anim_set_values(&b, UI_ARC_W_PULSE, UI_ARC_W_NORMAL);   /* 峰值 -> 常规 */
  lv_anim_set_time(&b, UI_BOUNCE_BACK_MS);
  lv_anim_set_path_cb(&b, lv_anim_path_bounce);              /* 衰减回落 */
  lv_anim_start(&b);
}

/*********************************************************************
 * 函 数 名: arc_width_pulse
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 仪表弧宽度呼吸脉冲: 旋转时弧由常规宽度膨胀到峰值再
 *           衰减回常规(替代缩放动画的"膨胀感")
 * 说    明: 段1从当前宽度(可能是上次动画中间值)起跳, 快速旋转时
 *           弧持续保持膨胀状态, 停止旋转后衰减回落
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static void arc_width_pulse(void)
{
  lv_anim_t a;

  lv_anim_init(&a);
  lv_anim_set_var(&a, s_bl_arc);
  lv_anim_set_exec_cb(&a, arc_width_anim_cb);
  lv_anim_set_values(&a,
                     (int32_t)lv_obj_get_style_arc_width(s_bl_arc, LV_PART_MAIN),
                     UI_ARC_W_PULSE);                        /* 当前 -> 峰值 */
  lv_anim_set_time(&a, UI_BOUNCE_UP_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, arc_width_ready_cb);
  lv_anim_start(&a);
}

/*********************************************************************
 * 函 数 名: btn_glow_anim_cb
 * 入口参数: var   按钮对象
 *           v     插值中的阴影宽度
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 按钮发光动画执行回调: 将插值写入阴影宽度样式
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static void btn_glow_anim_cb(void *var, int32_t v)
{
  lv_obj_set_style_shadow_width((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/*********************************************************************
 * 函 数 名: btn_glow_ready_cb
 * 入口参数: a   第一段(点亮)动画句柄
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 发光第二段: 阴影达峰值后以 bounce 路径衰减熄灭
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static void btn_glow_ready_cb(lv_anim_t *a)
{
  lv_anim_t b;

  (void)a;
  lv_anim_init(&b);
  lv_anim_set_var(&b, s_btn);
  lv_anim_set_exec_cb(&b, btn_glow_anim_cb);
  lv_anim_set_values(&b, UI_BTN_GLOW_MAX, 0);                /* 峰值 -> 熄灭 */
  lv_anim_set_time(&b, UI_BOUNCE_BACK_MS);
  lv_anim_set_path_cb(&b, lv_anim_path_bounce);              /* 衰减闪烁 */
  lv_anim_start(&b);
}

/*********************************************************************
 * 函 数 名: btn_glow_pulse
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 按钮发光脉冲: 阴影快速点亮至峰值再衰减熄灭,
 *           颜色取按钮目标态色(替代缩放动画的切换仪式感)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static void btn_glow_pulse(void)
{
  lv_anim_t a;

  /* ---- Step1: 配置阴影参数(颜色=目标态色, 无偏移向外泛光) ---- */
  lv_obj_set_style_shadow_color(s_btn, s_btn_color_to, 0);
  lv_obj_set_style_shadow_ofs_x(s_btn, 0, 0);
  lv_obj_set_style_shadow_ofs_y(s_btn, 0, 0);
  lv_obj_set_style_shadow_spread(s_btn, 0, 0);

  /* ---- Step2: 阴影宽度 0 -> 峰值(点亮), 随后 ready 回调衰减熄灭 ---- */
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_btn);
  lv_anim_set_exec_cb(&a, btn_glow_anim_cb);
  lv_anim_set_values(&a, 0, UI_BTN_GLOW_MAX);
  lv_anim_set_time(&a, UI_BTN_GLOW_UP_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, btn_glow_ready_cb);
  lv_anim_start(&a);
}

/*********************************************************************
 * 函 数 名: btn_color_anim_cb
 * 入口参数: var   按钮对象(未直接使用, 操作全局 s_btn)
 *           v     混合进度(0=起始色 255=目标色)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 按钮背景色渐变动画执行回调: 按进度在起止色间插值
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求)
 **********************************************************************/
static void btn_color_anim_cb(void *var, int32_t v)
{
  (void)var;
  lv_obj_set_style_bg_color(s_btn, lv_color_mix(s_btn_color_to,
                                               s_btn_color_from,
                                               (uint8_t)v), 0);
}

/*********************************************************************
 * 函 数 名: app_ui_bl_refresh
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 按当前亮度值刷新 UI(弧动画/渐变色/弹跳数字)与 PWM 输出
 * 说    明: 亮度限幅由驱动层保证(20~100), UI 与硬件共用一份缓存值;
 *           弧值经缓动动画扫向新值, 弧与数字颜色同步渐变,
 *           数字同时播放弹跳动画
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(旋转联动背光需求);
 *     修改日期: 2026年10月17日
 *     修改内容: 亮度条改为环形仪表盘, 新增弧值缓动/渐变色/数字弹跳动画
 **********************************************************************/
static void app_ui_bl_refresh(void)
{
  lv_color_t level_color;

  /* ---- Step1: 驱动层限幅并更新 PWM 输出 ---- */
  backlight_set_percent((uint8_t)s_bl_value);
  s_bl_value = (int8_t)backlight_get_percent();   /* 回读限幅后的实际值 */

  /* ---- Step2: 渐变色(弧指示器与大号数字共用) ---- */
  level_color = bl_level_color((int32_t)s_bl_value);

  /* ---- Step3: 弧值缓动动画扫向新值, 弧颜色即时渐变 ---- */
  bl_arc_anim_to((int32_t)s_bl_value);
  lv_obj_set_style_arc_color(s_bl_arc, level_color, LV_PART_INDICATOR);

  /* ---- Step4: 弧心大号百分比文字(重新居中 + 颜色联动 + 垂直弹跳) ---- */
  lv_label_set_text_fmt(s_value_label, "%d%%", (int)s_bl_value);
  lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, -16);   /* 文字宽度变化后保持居中 */
  lv_obj_set_style_text_color(s_value_label, level_color, 0);
  obj_y_bounce(s_value_label);   /* translate_y 弹跳, 不传绝对坐标避免漂移 */

  /* ---- Step5: 弧宽呼吸脉冲(膨胀感) ---- */
  arc_width_pulse();
}

/*********************************************************************
 * 函 数 名: app_ui_btn_apply
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 按翻转状态立即设置按钮外观(无动画, 供初始化使用)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(酷炫动画需求, 从 btn_refresh 拆分)
 **********************************************************************/
static void app_ui_btn_apply(void)
{
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
 * 函 数 名: app_ui_btn_refresh
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 按翻转状态播放按钮切换动画(背景色渐变 + 发光脉冲)
 * 说    明: ON 态蓝色高亮, OFF 态灰色暗显; 颜色在起止色间 300ms
 *           渐变过渡, 按钮同步阴影点亮-衰减(发光脉冲)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(点按按钮变化需求);
 *     修改日期: 2026年10月17日
 *     修改内容: 切换由瞬变改为颜色渐变+发光脉冲动画(酷炫动画需求)
 **********************************************************************/
static void app_ui_btn_refresh(void)
{
  lv_anim_t a;

  /* ---- Step1: 记录起止色(起点=当前显示色, 支持动画中途反向切换) ---- */
  s_btn_color_from = lv_obj_get_style_bg_color(s_btn, 0);
  if(s_btn_state != 0)
  {
    s_btn_color_to = UI_BTN_ON_COLOR;
    lv_label_set_text(s_btn_label, "BTN: ON");
  }
  else
  {
    s_btn_color_to = UI_BTN_OFF_COLOR;
    lv_label_set_text(s_btn_label, "BTN: OFF");
  }

  /* ---- Step2: 背景色 300ms 渐变过渡 ---- */
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_btn);
  lv_anim_set_exec_cb(&a, btn_color_anim_cb);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_time(&a, UI_ANIM_BTN_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  /* ---- Step3: 按钮发光脉冲(阴影点亮->衰减, 颜色=目标态色) ---- */
  btn_glow_pulse();
}

/*********************************************************************
 * 函 数 名: debug_pin_timer_cb
 * 入口参数: timer       定时器句柄(未使用)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 100ms 周期刷新编码器三路引脚电平自检行
 * 说    明: 硬件排查利器——A/B 空闲应显示 1(外部上拉 3.3V), P(PB3) 空闲
 *           为 1(内部上拉, 按键一脚接 GND, 按下接地变 0);
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
 * 函数功能: 创建交互演示界面(标题/环形仪表盘/翻转按钮/自检行/提示)
 * 说    明: 必须在 LVGL 任务首次刷新前调用(调度器启动前的 main 中
 *           调用, 创建阶段无并发访问风险);
 *           布局(240x240): 顶部标题+自检行 -> 中央 120x120 环形
 *           仪表盘(弧心嵌大号百分比与 eCO2/TVOC 两行小字) ->
 *           下部翻转按钮 -> 底部操作提示
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(旋转调背光 + 点按翻转按钮交互);
 *             新增编码器引脚电平自检行(硬件排查);
 *     修改日期: 2026年10月17日
 *     修改内容: 亮度条重构为环形仪表盘, eCO2/TVOC 移入弧心,
 *               新增全套切换动画(酷炫动画需求)
 **********************************************************************/
void app_ui_init(void)
{
  /* ---- Step0: 初始化背光编码器变速器实例(变速逻辑见 encoder_accel 模块) ---- */
  encoder_accel_init(&s_bl_accel);

  /* ---- Step1: 顶部标题 ---- */
  s_title_label = lv_label_create(lv_scr_act());
  lv_label_set_text(s_title_label, "Demo~");
  lv_obj_set_style_text_color(s_title_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_28, 0);
  lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 4);

  /* ---- Step2: 中央 270° 环形仪表盘(圆头弧, 隐藏 knob) ---- */
  s_bl_arc = lv_arc_create(lv_scr_act());
  lv_obj_set_size(s_bl_arc, 120, 120);
  lv_obj_align(s_bl_arc, LV_ALIGN_TOP_MID, 0, 36);
  lv_arc_set_rotation(s_bl_arc, 135);                      /* 起点左下, 经典仪表造型 */
  lv_arc_set_bg_angles(s_bl_arc, 0, 270);                  /* 270° 扫弧 */
  lv_arc_set_range(s_bl_arc, BACKLIGHT_MIN_PERCENT, BACKLIGHT_MAX_PERCENT);
  lv_arc_set_value(s_bl_arc, s_bl_value);
  lv_obj_clear_flag(s_bl_arc, LV_OBJ_FLAG_CLICKABLE);      /* 不可点击即不可拖动调节, 输入只来自编码器 */
  lv_obj_set_style_arc_width(s_bl_arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_bl_arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_bl_arc, lv_color_hex(0x26262E), LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_bl_arc, bl_level_color((int32_t)s_bl_value),
                              LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(s_bl_arc, TRUE, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(s_bl_arc, TRUE, LV_PART_INDICATOR);
  lv_obj_remove_style(s_bl_arc, NULL, LV_PART_KNOB);       /* 隐藏指示端圆点 */

  /* ---- Step3: 弧心大号亮度百分比文字(垂直弹跳动画) ---- */
  s_value_label = lv_label_create(s_bl_arc);
  lv_obj_set_style_text_color(s_value_label, bl_level_color((int32_t)s_bl_value), 0);
  lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_28, 0);
  lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, -16);

  /* ---- Step4: 弧心下方两行 SGP30 小字(eCO2/TVOC) ---- */
  s_eco2_label = lv_label_create(s_bl_arc);
  lv_obj_set_style_text_color(s_eco2_label, lv_color_hex(0x4CAF50), 0);
  lv_label_set_text(s_eco2_label, "eCO2: --");
  lv_obj_align(s_eco2_label, LV_ALIGN_CENTER, 0, 8);

  s_tvoc_label = lv_label_create(s_bl_arc);
  lv_obj_set_style_text_color(s_tvoc_label, lv_color_hex(0x26C6DA), 0);
  lv_label_set_text(s_tvoc_label, "TVOC: --");
  lv_obj_align(s_tvoc_label, LV_ALIGN_CENTER, 0, 24);

  /* ---- Step5: 点按翻转按钮(初始 OFF 灰色, 发光脉冲动画) ---- */
  s_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(s_btn, 140, 44);
  lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);   /* 输入来自编码器, 禁触屏点击 */

  s_btn_label = lv_label_create(s_btn);               /* 按钮文字(子对象) */
  lv_obj_center(s_btn_label);

  /* ---- Step6: 底部操作提示 ---- */
  s_hint_label = lv_label_create(lv_scr_act());
  lv_label_set_text(s_hint_label, "Rotate: backlight\nPush: toggle button");
  lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x9E9E9E), 0);
  lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -6);

  /* ---- Step6.1: 引脚电平自检行(右上角, 硬件排查用, 100ms 刷新) ---- */
  s_debug_label = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(s_debug_label, lv_color_hex(0x4CAF50), 0);
  lv_label_set_text(s_debug_label, "A:- B:- P:-");
  lv_obj_align(s_debug_label, LV_ALIGN_TOP_RIGHT, -4, 4);
  lv_timer_create(debug_pin_timer_cb, 100, NULL);

  /* ---- Step6.2: 长按关机进度提示(顶部, 出现时盖住标题, 默认隐藏) ---- */
  s_power_label = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(s_power_label, lv_color_hex(0xE53935), 0);
  lv_label_set_text(s_power_label, "Hold 3s: power off");
  lv_obj_align(s_power_label, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_add_flag(s_power_label, LV_OBJ_FLAG_HIDDEN);

  /* ---- Step7: 刷新初始显示(亮度 100%, 按钮 OFF 立即应用不播动画) ---- */
  app_ui_bl_refresh();
  app_ui_btn_apply();
}

/*********************************************************************
 * 函 数 名: encoder_apply_delta
 * 入口参数: delta   编码器旋转增量(4 倍频计步, 正=顺时针)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 将编码器增量经变速模块换算后应用到背光亮度并刷新 UI
 * 说    明: 变速策略(慢速精准/快速加速)由通用模块 encoder_accel 提供,
 *           本函数只负责把"步数"换算成亮度 1% 并限幅刷新
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月17日
 *     修改内容: 新增函数(变速调节需求)
 *              变速逻辑抽离至 encoder_accel 通用模块
 **********************************************************************/
static void encoder_apply_delta(int32_t delta)
{
  int32_t change;   /* 变速后的步数(1 步 = 1% 亮度) */
  int32_t new_val;  /* 限幅后的亮度中间值 */

  /* ---- Step1: 经通用变速模块换算为步数 ---- */
  change = encoder_accel_feed(&s_bl_accel, delta);
  if(change == 0){ return; }

  /* ---- Step2: 应用到亮度并限幅(中间量用 int32 避免 int8 溢出) ---- */
  new_val = (int32_t)s_bl_value + change;
  if(new_val > BACKLIGHT_MAX_PERCENT){ new_val = BACKLIGHT_MAX_PERCENT; }
  if(new_val < BACKLIGHT_MIN_PERCENT){ new_val = BACKLIGHT_MIN_PERCENT; }
  s_bl_value = (int8_t)new_val;

  /* ---- Step3: 刷新弧/数字/颜色/动画 ---- */
  app_ui_bl_refresh();
}

/*********************************************************************
 * 函 数 名: app_ui_encoder_event
 * 入口参数: delta        编码器旋转增量(4 倍频计步, 正=顺时针)
 *           push_pressed 按键"松开->按下"沿事件(1=有)
 *           hold_ms      当前连续按下时长(ms): >=3000 显示"Power off",
 *                        >=800 显示保持提示, 0=取消提示
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 处理编码器事件: 旋转调节背光亮度(弧形仪表动画联动),
 *           点按翻转按钮(颜色渐变+弹跳动画), 长按显示关机进度提示
 * 说    明: 仅允许 LVGL 任务调用(app_main.c 的 lvgl_task 队列消费处);
 *           旋转经 encoder_apply_delta 变速映射: 慢速每格 1% 精准,
 *           快速按速度放大步进(详见该函数); 长按进度由 encoder_task
 *           每 20ms 随事件携带, 松开归 0 时自动隐藏提示(取消关机)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月14日
 *     修改内容: 新增函数(旋转调背光 + 点按翻转按钮交互);
 *     修改日期: 2026年10月17日
 *     修改内容: 签名增加 hold_ms, 新增长按 3s 关机进度提示(低功耗需求);
 *               旋转/点按路径升级为弧形仪表+渐变+弹跳动画(酷炫动画需求)
 **********************************************************************/
void app_ui_encoder_event(int32_t delta, int32_t push_pressed, int32_t hold_ms)
{
  /* ---- Step1: 旋转增量 -> 变速背光亮度, 仪表盘动画联动 ---- */
  if(delta != 0)
  {
    encoder_apply_delta(delta);
  }

  /* ---- Step2: 点按沿事件 -> 翻转按钮状态并播放切换动画 ---- */
  if(push_pressed != 0)
  {
    s_btn_state = (uint8_t)(s_btn_state ? 0 : 1);
    app_ui_btn_refresh();
  }

  /* ---- Step3: 长按关机进度提示(达阈值提示关机, 松开归 0 自动隐藏) ---- */
  if(hold_ms >= (int32_t)ENC_PUSH_LONG_PRESS_MS)
  {
    lv_label_set_text(s_power_label, "Power off...");
  }
  else if(hold_ms >= (int32_t)UI_PWR_HINT_MS)
  {
    lv_label_set_text(s_power_label, "Hold 3s: power off");
  }

  if(hold_ms >= (int32_t)UI_PWR_HINT_MS)
  {
    lv_obj_clear_flag(s_power_label, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    lv_obj_add_flag(s_power_label, LV_OBJ_FLAG_HIDDEN);
  }
}

/*********************************************************************
 * 函 数 名: app_ui_sgp30_update
 * 入口参数: eco2    eCO2 浓度(ppm)
 *           tvoc    TVOC 浓度(ppb)
 *           valid   1=本次数据有效 0=读取失败
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 刷新 SGP30 传感器的 eCO2/TVOC 显示标签(弧心两行小字)
 * 说    明: 仅允许 LVGL 任务调用(app_main.c 的 lvgl_task 队列消费处);
 *           读取失败/模块未就绪时显示 "--" 便于区分; 传感器初始化后
 *           约 15s 预热期内固定显示 400ppm/0ppb, 属数据手册定义的正常
 *           现象, 并非驱动异常
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年10月15日
 *     修改内容: 新增函数(SGP30 移植需求);
 *     修改日期: 2026年10月17日
 *     修改内容: 标签移入弧心, 显示格式精简(酷炫动画需求配套布局)
 **********************************************************************/
void app_ui_sgp30_update(uint32_t eco2, uint32_t tvoc, uint32_t valid)
{
  /* ---- Step1: 数据有效 -> 显示 eCO2 与 TVOC 实测值 ---- */
  if(valid != 0U)
  {
    lv_label_set_text_fmt(s_eco2_label, "eCO2: %u", (unsigned)eco2);
    lv_label_set_text_fmt(s_tvoc_label, "TVOC: %u", (unsigned)tvoc);
  }
  /* ---- Step2: 数据无效 -> 占位符提示, 不显示陈旧数据 ---- */
  else
  {
    lv_label_set_text(s_eco2_label, "eCO2: --");
    lv_label_set_text(s_tvoc_label, "TVOC: --");
  }
}
