/**
  **************************************************************************
  * @file    app_usb_upgrade.c
  * @brief   APP 端 USB 在线升级触发实现(TypeC 虚拟串口)
  *
  * 实现说明:
  *   - 常驻 USB CDC(共享栈 Software/Shared/usb_cdc.c), 协议子集:
  *     HELLO -> 应答 [mode=APP][版本][APP 有效];
  *     REBOOT -> 中断内仅置 volatile 标志 + 应答 OK;
  *   - 线程约束(本工程核心规则):
  *     USB 中断回调绝不触碰 LVGL/FreeRTOS API(优先级数值 0xE/0xF
  *     大于 configMAX_SYSCALL_INTERRUPT_PRIORITY=0x50, 不属于 RTOS
  *     管辖层); flash 参数页写入与复位在任务上下文执行(擦写期间
  *     取指停顿不影响中断安全性);
  *   - 复位流程: 置参数页 BS_FLAG_UPDATE_REQ -> 等 100ms 让应答发出
  *     -> NVIC_SystemReset(), BOOT 上电见标志即进升级模式。
  **************************************************************************
  */

#include "app_usb_upgrade.h"
#include "boot_shared.h"
#include "flash_drv.h"
#include "usb_cdc.h"
#include "FreeRTOS.h"
#include "task.h"

/* APP 端接收帧缓冲(APP 相关帧均 <= 12B: 头 4B + payload 8B) */
#define AU_RX_BUF_SIZE     (BS_FRAME_HDR_LEN + 8U)

static uint8_t           s_rx_buf[AU_RX_BUF_SIZE];   // 帧重组缓冲
static volatile uint16_t s_rx_pos;                   // 已写入字节数
static volatile uint16_t s_rx_need;                  // 当前帧 payload 长度
static volatile uint8_t  s_reboot_req;               // 1=收到 REBOOT 命令待处理

/* 应答帧暂存(头 5B + payload 3B) */
static uint8_t s_tx_buf[8];

/*********************************************************************
 * 函 数 名: au_reply
 * 入口参数: cmd       回应的命令码
 *           status    应答状态(BS_STATUS_xxx)
 *           payload   应答附加数据(可为 0)
 *           len       附加数据长度
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 组装并发送一帧应答(USB 中断上下文安全, 无阻塞)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(APP 在线升级需求)
 **********************************************************************/
static void au_reply(uint8_t cmd, uint8_t status, const uint8_t *payload, uint16_t len)
{
  /* ---- Step1: 组装应答帧头 ---- */
  s_tx_buf[0] = BS_FRAME_HEAD_DEV;
  s_tx_buf[1] = cmd;
  s_tx_buf[2] = status;
  s_tx_buf[3] = (uint8_t)(len & 0xFFU);
  s_tx_buf[4] = (uint8_t)(len >> 8);

  /* ---- Step2: 拷贝附加数据并发送(忙则丢弃, 主机重发兜底) ---- */
  if((payload != (const uint8_t *)0) && (len != 0U))
  {
    uint16_t i;                                         // 拷贝索引
    for(i = 0; i < len; i++)
    {
      s_tx_buf[5U + i] = payload[i];
    }
  }
  (void)usb_cdc_send(s_tx_buf, (uint16_t)(5U + len));
}

/*********************************************************************
 * 函 数 名: au_handle_frame
 * 入口参数: frame   完整协议帧(4B 头 + payload)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 处理一帧完整命令(仅 HELLO/REBOOT, 其余回 ERR)
 * 说    明: 在 USB 中断上下文执行, 只做应答与置标志
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(APP 在线升级需求)
 **********************************************************************/
static void au_handle_frame(const uint8_t *frame)
{
  uint8_t cmd = frame[1];                               // 命令码

  /* ---- HELLO: 握手应答(mode=APP) ---- */
  if(cmd == BS_CMD_HELLO)
  {
    uint8_t rsp[3];                                     // [mode][ver][app_valid]
    boot_param_t param;                                 // 参数页快照(只读)

    bs_param_read(&param);
    rsp[0] = BS_MODE_APP;
    rsp[1] = BS_PROTO_VER;
    rsp[2] = ((param.flags & BS_FLAG_APP_VALID) != 0U) ? 1U : 0U;
    au_reply(BS_CMD_HELLO, BS_STATUS_OK, rsp, 3U);
    return;
  }

  /* ---- REBOOT: 置升级请求标志(写参数页与复位在任务上下文执行) ---- */
  if(cmd == BS_CMD_REBOOT)
  {
    s_reboot_req = 1U;
    au_reply(BS_CMD_REBOOT, BS_STATUS_OK, (const uint8_t *)0, 0U);
    return;
  }

  /* ---- 其余命令(ERASE/DATA 等)仅 BOOT 支持 ---- */
  au_reply(cmd, BS_STATUS_ERR, (const uint8_t *)0, 0U);
}

/*********************************************************************
 * 函 数 名: usb_cdc_on_rx
 * 入口参数: data   bulk OUT 收到的数据(USB 中断上下文)
 *           len    字节数
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: USB 中断回调: 逐字节重组协议帧, 完整帧立即分发
 * 说    明: APP 相关帧极短(<=12B), 单个 64B USB 包内必完整;
 *           严格不触碰 LVGL/FreeRTOS API
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(APP 在线升级需求)
 **********************************************************************/
void usb_cdc_on_rx(const uint8_t *data, uint16_t len)
{
  uint16_t i;                                           // 数据索引

  for(i = 0; i < len; i++)
  {
    uint8_t b = data[i];                                // 当前字节

    if(s_rx_pos < BS_FRAME_HDR_LEN)                     // 帧头接收阶段
    {
      if(s_rx_pos == 0U)
      {
        if(b != BS_FRAME_HEAD_HOST)
        {
          continue;                                     // 丢弃帧头前杂散字节
        }
        s_rx_buf[0] = b;
        s_rx_pos = 1U;
      }
      else if(s_rx_pos == 1U)
      {
        s_rx_buf[1] = b;
        s_rx_pos = 2U;
      }
      else if(s_rx_pos == 2U)
      {
        s_rx_buf[2] = b;
        s_rx_pos = 3U;
      }
      else
      {
        s_rx_buf[3] = b;
        s_rx_pos = 4U;
        s_rx_need = (uint16_t)((uint16_t)s_rx_buf[2] | ((uint16_t)s_rx_buf[3] << 8));
        if(s_rx_need > (AU_RX_BUF_SIZE - BS_FRAME_HDR_LEN))
        {
          s_rx_pos = 0U;                                // 超长帧, 重新同步
        }
      }
    }
    else                                                // payload 接收阶段
    {
      s_rx_buf[s_rx_pos++] = b;
      if((uint16_t)(s_rx_pos - BS_FRAME_HDR_LEN) >= s_rx_need)
      {
        au_handle_frame(s_rx_buf);                      // 帧完整, 立即处理
        s_rx_pos = 0U;
        s_rx_need = 0U;
      }
    }
  }
}

/*********************************************************************
 * 函 数 名: app_usb_upgrade_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 初始化 APP 端 USB CDC(时钟 HICK 48MHz + 枚举 + 连接)
 * 说    明: 在 main 驱动初始化后调用; USB 中断优先级低于 RTOS
 *           系统调用层, 与 FreeRTOS 调度互不干扰
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(APP 在线升级需求)
 **********************************************************************/
void app_usb_upgrade_init(void)
{
  s_rx_pos = 0;
  s_rx_need = 0;
  s_reboot_req = 0;
  usb_cdc_init();
}

/*********************************************************************
 * 函 数 名: app_usb_upgrade_poll_reboot
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: uint8_t 1=已执行复位(不返回) 0=无重启请求
 * 函数功能: 检测并执行"重启进 BOOT"流程(任务上下文周期调用)
 * 说    明: 流程: 参数页置升级请求(保留 APP 有效标志) -> 等
 *           100ms 让应答发完 -> 系统复位; BOOT 见请求进升级模式
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(APP 在线升级需求)
 **********************************************************************/
uint8_t app_usb_upgrade_poll_reboot(void)
{
  boot_param_t param;                                   // 参数页快照

  /* ---- Step1: 无请求则直接返回 ---- */
  if(s_reboot_req == 0U)
  {
    return 0U;
  }

  /* ---- Step2: 参数页置升级请求(保留原 APP 描述字段) ---- */
  bs_param_read(&param);
  if(param.magic != BS_PARAM_MAGIC)
  {
    param.app_size = 0;
    param.app_crc32 = 0;
    param.flags = 0;
  }
  param.magic = BS_PARAM_MAGIC;
  param.flags |= BS_FLAG_UPDATE_REQ;                    // 追加升级请求位
  flash_drv_unlock();
  (void)bs_param_write(&param);
  flash_drv_lock();

  /* ---- Step3: 留出时间让 REBOOT 应答发出, 然后系统复位 ---- */
  vTaskDelay(pdMS_TO_TICKS(100));
  NVIC_SystemReset();
  return 1U;                                            // 不会执行到这里
}
