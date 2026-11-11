/**
  **************************************************************************
  * @file    boot_main.c
  * @brief   BOOT(Bootloader)入口: 启动决策(跳 APP / 进 USB 升级模式)
  *
  * 启动流程:
  *   上电 -> 时钟 240MHz(USB 48MHz 取自 HICK) -> 读参数页 -> 决策:
  *     a) 参数页有升级请求(APP 在线触发置位)      -> USB 升级模式
  *     b) APP 无效(空片/标志缺失/向量非法)        -> USB 升级模式
  *     c) PB3(编码器按键)按住上电(强制升级安全网) -> USB 升级模式
  *     d) 其余                                   -> 直接跳转 APP
  *
  * 跳转安全: 升级路径跳转前由协议层完成 usb_cdc_deinit(); 直接
  * 跳转路径 USB 从未初始化, 无需清理; BOOT 不使用 SysTick。
  **************************************************************************
  */

#include "at32f403a_407.h"
#include "boot_shared.h"
#include "flash_drv.h"
#include "usb_cdc.h"
#include "boot_protocol.h"

/*********************************************************************
 * 函 数 名: boot_system_clock_config
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 配置系统时钟 240MHz(HICK 8MHz 二分频 4MHz 入 PLL 60 倍频)
 * 说    明: 与 APP 工程时钟配置一致; USB 48MHz 独立取自 HICK
 *           (crm_usb_clock_source_select, 见 usb_cdc_init), 不受影响
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求, 从 APP 工程迁移)
 **********************************************************************/
static void boot_system_clock_config(void)
{
  /* ---- Step1: 复位时钟管理器, 恢复默认 ---- */
  crm_reset();

  /* ---- Step2: 使能内部高速时钟 HICK(8MHz) ---- */
  crm_clock_source_enable(CRM_CLOCK_SOURCE_HICK, TRUE);
  while(crm_flag_get(CRM_HICK_STABLE_FLAG) != SET)
  {
  }

  /* ---- Step3: PLL: HICK/2=4MHz 入 PLL, 60 倍频输出 240MHz ---- */
  crm_pll_config(CRM_PLL_SOURCE_HICK, CRM_PLL_MULT_60, CRM_PLL_OUTPUT_RANGE_GT72MHZ);
  crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
  while(crm_flag_get(CRM_PLL_STABLE_FLAG) != SET)
  {
  }

  /* ---- Step4: 总线分频: AHB 不分频, APB1/APB2 二分频 ---- */
  crm_ahb_div_set(CRM_AHB_DIV_1);
  crm_apb1_div_set(CRM_APB1_DIV_2);
  crm_apb2_div_set(CRM_APB2_DIV_2);

  /* ---- Step5: 顺滑切换 PLL 为系统时钟源 ---- */
  crm_auto_step_mode_enable(TRUE);
  crm_sysclk_switch(CRM_SCLK_PLL);
  while(crm_sysclk_switch_status_get() != CRM_SCLK_PLL)
  {
  }
  crm_auto_step_mode_enable(FALSE);
  system_core_clock_update();
}

/*********************************************************************
 * 函 数 名: boot_pb3_pressed
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: uint8_t 1=PB3 按住(强制升级) 0=未按
 * 函数功能: 检测编码器按键 PB3 是否按住(上电瞬间的强制升级通道)
 * 说    明: PB3 复位默认为 JTAG(JTDO), 需先释放为 GPIO
 *           (SWJTAG_MUX_010, 仅 SWD 调试, 与 APP 工程一致);
 *           按键低有效(一脚接 GND, 内部上拉)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static uint8_t boot_pb3_pressed(void)
{
  gpio_init_type gpio_init_struct;                      // GPIO 配置结构

  /* ---- Step1: 使能 IOMUX/GPIOB 时钟并释放 JTAG ---- */
  crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  gpio_pin_remap_config(SWJTAG_MUX_010, TRUE);          // PB3/PB4 -> GPIO(仅 SWD)

  /* ---- Step2: PB3 输入上拉 ---- */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
  gpio_init_struct.gpio_pull = GPIO_PULL_UP;
  gpio_init_struct.gpio_pins = GPIO_PINS_3;
  gpio_init(GPIOB, &gpio_init_struct);

  /* ---- Step3: 低电平=按住 ---- */
  return (gpio_input_data_bit_read(GPIOB, GPIO_PINS_3) == RESET) ? 1U : 0U;
}

/*********************************************************************
 * 函 数 名: boot_app_valid
 * 入口参数: param   参数页快照(bs_param_read 已填充)
 * 出口参数: 无
 * 返 回 值: uint8_t 1=APP 有效可跳转 0=无效
 * 函数功能: APP 有效性判定: 有效标志 + 镜像大小合法 + 栈顶/复位向量
 *           落在合法范围(不做全镜像 CRC, 上电延迟控制)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static uint8_t boot_app_valid(const boot_param_t *param)
{
  uint32_t sp;                                          // APP 栈顶(向量表[0])
  uint32_t pc;                                          // APP 入口(向量表[1])

  /* ---- Step1: 参数页标志与大小 ---- */
  if(((param->flags & BS_FLAG_APP_VALID) == 0U)
     || (param->app_size == 0U) || (param->app_size > BS_APP_MAX_SIZE))
  {
    return 0U;
  }

  /* ---- Step2: 向量合法性: 栈顶落在 96KB SRAM, 入口落在 APP 区 ---- */
  sp = *(volatile uint32_t *)BS_APP_BASE;
  pc = *(volatile uint32_t *)(BS_APP_BASE + 4U);
  if((sp < 0x20000000UL) || (sp > (0x20000000UL + 0x18000UL)))
  {
    return 0U;
  }
  if((pc < BS_APP_BASE) || (pc >= (BS_APP_BASE + BS_APP_MAX_SIZE)))
  {
    return 0U;
  }
  return 1U;
}

/*********************************************************************
 * 函 数 名: boot_jump_app
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void(不返回)
 * 函数功能: 跳转 APP: 重定位向量表 -> 设置主栈指针 -> 跳复位向量
 * 说    明: 调用前必须保证外设已清理(USB 由协议层 deinit; BOOT
 *           不用 SysTick); 代码与数据在 BOOT 区, APP 全新启动
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
void boot_jump_app(void)
{
  uint32_t sp;                                          // APP 栈顶
  uint32_t pc;                                          // APP 复位向量
  void (*app_entry)(void);                              // APP 入口函数指针

  /* ---- Step1: 取 APP 向量表前两项 ---- */
  sp = *(volatile uint32_t *)BS_APP_BASE;
  pc = *(volatile uint32_t *)(BS_APP_BASE + 4U);
  app_entry = (void (*)(void))pc;

  /* ---- Step2: 重定位向量表到 APP 基址 ---- */
  SCB->VTOR = BS_APP_BASE;

  /* ---- Step3: 设置主栈指针并跳转 ---- */
  __set_MSP(sp);
  app_entry();
  for(;;)
  {
  }                                                     // 不应到达
}

/*********************************************************************
 * 函 数 名: main
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: int(不返回)
 * 函数功能: BOOT 入口: 时钟初始化后按 参数页/APP 有效性/按键 决策
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
int main(void)
{
  boot_param_t param;                                   // 参数页快照

  /* ---- Step1: 系统时钟 240MHz ---- */
  boot_system_clock_config();

  /* ---- Step2: 读参数页并决策 ---- */
  bs_param_read(&param);
  if((((param.flags & BS_FLAG_UPDATE_REQ) != 0U)        // a) APP 在线触发的升级请求
      || (boot_app_valid(&param) == 0U)                 // b) APP 无效
      || (boot_pb3_pressed() != 0U)))                   // c) 按住 PB3 强制升级
  {
    /* ---- Step3: 进入 USB 升级模式(flash 解锁 + CDC + 协议主循环) ---- */
    flash_drv_unlock();
    usb_cdc_init();
    boot_protocol_run();                                // 不返回(DONE 后跳转 APP)
  }

  /* ---- Step4: 直接跳转 APP ---- */
  boot_jump_app();
  for(;;)
  {
  }
}
