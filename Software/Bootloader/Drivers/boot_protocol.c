/**
  **************************************************************************
  * @file    boot_protocol.c
  * @brief   BOOT 升级协议状态机实现
  *
  * 实现说明:
  *   - usb_cdc_on_rx() 为 USB 中断回调, 做逐字节帧重组: 找帧头
  *     0xA5 -> 收命令/长度 -> 收满 payload 后置 frame_ready;
  *   - boot_protocol_run() 为主循环, WFI 等待帧就绪后分发命令,
  *     flash 擦写等耗时操作在本上下文执行(擦写期间取指停顿导致
  *     USB 中断延迟, bulk OUT 未重新 VALID 前硬件 NAK, 主机等待);
  *   - DONE 成功后: 写参数页(size/crc/app_valid) -> 应答 OK ->
  *     等发送完成 -> usb_cdc_deinit() -> boot_jump_app();
  *   - 主循环永不超时, 等待主机拖入固件。
  **************************************************************************
  */

#include "boot_protocol.h"
#include "boot_shared.h"
#include "flash_drv.h"
#include "usb_cdc.h"

/* 协议帧 payload 上限(DATA: seq4B + len2B + 1024B 数据 = 1030B) */
#define BP_MAX_PAYLOAD       (4U + 2U + BS_DATA_MAX_LEN)

/* 帧接收缓冲(帧头 4B + payload) */
#define BP_RX_BUF_SIZE       (BS_FRAME_HDR_LEN + BP_MAX_PAYLOAD)

/* 模块状态 */
static uint8_t           s_rx_buf[BP_RX_BUF_SIZE];      // 帧重组缓冲
static volatile uint16_t s_rx_pos;                      // 已写入字节数
static volatile uint16_t s_rx_need;                     // 当前帧 payload 长度
static volatile uint8_t  s_frame_ready;                 // 1=完整帧待处理

/* 发送应答暂存(头 5B + payload) */
static uint8_t s_tx_buf[5U + 8U];

/*********************************************************************
 * 函 数 名: bp_reply
 * 入口参数: cmd       回应的命令码(与请求一致)
 *           status    应答状态(BS_STATUS_xxx)
 *           payload   应答附加数据(可为 0)
 *           len       附加数据长度
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 组装并发送一帧应答([0x5A][cmd][status][len16LE][payload])
 * 说    明: 应答帧短(<=13B), 单次 usb_cdc_send 即可发出; 若上一帧
 *           尚未发完则丢弃本次应答, 由主机超时重发请求保证可靠
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void bp_reply(uint8_t cmd, uint8_t status, const uint8_t *payload, uint16_t len)
{
  /* ---- Step1: 组装应答帧头 ---- */
  s_tx_buf[0] = BS_FRAME_HEAD_DEV;
  s_tx_buf[1] = cmd;
  s_tx_buf[2] = status;
  s_tx_buf[3] = (uint8_t)(len & 0xFFU);                 // len 低字节在前
  s_tx_buf[4] = (uint8_t)(len >> 8);

  /* ---- Step2: 拷贝附加数据并发送 ---- */
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
 * 函 数 名: usb_cdc_on_rx
 * 入口参数: data   bulk OUT 收到的数据(中断上下文)
 *           len    字节数
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: USB 中断回调: 逐字节重组协议帧, 完整帧置就绪标志
 * 说    明: 仅操作缓冲与标志, 不做 flash/延时等耗时操作;
 *           上一帧未消费时丢弃新数据(等 ACK 的主机重发机制兜底)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
void usb_cdc_on_rx(const uint8_t *data, uint16_t len)
{
  uint16_t i;                                           // 数据索引

  for(i = 0; i < len; i++)
  {
    uint8_t b = data[i];                                // 当前字节

    if(s_frame_ready != 0U)
    {
      return;                                           // 上一帧未消费, 丢弃
    }

    if(s_rx_pos < BS_FRAME_HDR_LEN)                     // 帧头接收阶段
    {
      if(s_rx_pos == 0U)
      {
        if(b != BS_FRAME_HEAD_HOST)
        {
          continue;                                     // 丢弃帧头前的杂散字节
        }
        s_rx_buf[0] = b;
        s_rx_pos = 1U;
      }
      else if(s_rx_pos == 1U)
      {
        s_rx_buf[1] = b;                                // 命令码
        s_rx_pos = 2U;
      }
      else if(s_rx_pos == 2U)
      {
        s_rx_buf[2] = b;                                // len 低字节
        s_rx_pos = 3U;
      }
      else
      {
        s_rx_buf[3] = b;                                // len 高字节
        s_rx_pos = 4U;
        s_rx_need = (uint16_t)((uint16_t)s_rx_buf[2] | ((uint16_t)s_rx_buf[3] << 8));
        if(s_rx_need > BP_MAX_PAYLOAD)
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
        s_frame_ready = 1U;                             // 帧完整
      }
    }
  }
}

/*********************************************************************
 * 函 数 名: bp_handle_hello
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 处理 HELLO 握手: 应答模式(BOOT)/协议版本/APP 有效性
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void bp_handle_hello(void)
{
  boot_param_t param;                                   // 参数页快照
  uint8_t rsp[3];                                       // 应答 payload

  bs_param_read(&param);
  rsp[0] = BS_MODE_BOOT;                                // 当前运行模式
  rsp[1] = BS_PROTO_VER;                                // 协议版本
  rsp[2] = ((param.flags & BS_FLAG_APP_VALID) != 0U) ? 1U : 0U;   // APP 有效
  bp_reply(BS_CMD_HELLO, BS_STATUS_OK, rsp, 3U);
}

/*********************************************************************
 * 函 数 名: bp_handle_erase
 * 入口参数: payload   [size_lo][size_hi..](4B 小端 APP 镜像大小)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 擦除 APP 区(按镜像大小向上取整页数逐页擦除)
 * 说    明: 全区擦除约 10s(484 页 x ~20ms), 应答在擦完后发出,
 *           主机需将写超时设 >=30s
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void bp_handle_erase(const uint8_t *payload)
{
  uint32_t size;                                        // APP 镜像大小
  uint32_t pages;                                       // 待擦页数
  uint32_t i;                                           // 页循环索引

  /* ---- Step1: 解析并校验镜像大小 ---- */
  size = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
       | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
  if((size == 0U) || (size > BS_APP_MAX_SIZE))
  {
    bp_reply(BS_CMD_ERASE, BS_STATUS_ERR, (const uint8_t *)0, 0U);
    return;
  }

  /* ---- Step2: 逐页擦除 APP 区 ---- */
  pages = (size + BS_FLASH_PAGE_SIZE - 1U) / BS_FLASH_PAGE_SIZE;
  for(i = 0; i < pages; i++)
  {
    if(flash_drv_page_erase(BS_APP_BASE + i * BS_FLASH_PAGE_SIZE) == 0U)
    {
      bp_reply(BS_CMD_ERASE, BS_STATUS_ERR, (const uint8_t *)0, 0U);
      return;
    }
  }
  bp_reply(BS_CMD_ERASE, BS_STATUS_OK, (const uint8_t *)0, 0U);
}

/*********************************************************************
 * 函 数 名: bp_handle_data
 * 入口参数: payload   [seq(4B)][len(2B)][data(len)]
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 将一包镜像数据(<=1KB)按序号写入 APP 区 flash
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void bp_handle_data(const uint8_t *payload)
{
  uint32_t seq;                                         // 包序号(0 起, 单位 1KB)
  uint16_t dlen;                                        // 本包数据长度
  uint32_t addr;                                        // 写入目标地址

  /* ---- Step1: 解析并校验 ---- */
  seq = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
      | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
  dlen = (uint16_t)((uint16_t)payload[4] | ((uint16_t)payload[5] << 8));
  addr = BS_APP_BASE + seq * BS_DATA_MAX_LEN;
  if((dlen == 0U) || (dlen > BS_DATA_MAX_LEN)
     || ((addr + dlen) > (BS_APP_BASE + BS_APP_MAX_SIZE)))
  {
    bp_reply(BS_CMD_DATA, BS_STATUS_ERR, (const uint8_t *)0, 0U);
    return;
  }

  /* ---- Step2: 写入 flash(尾部非整字自动补 0xFF) ---- */
  if(flash_drv_write(addr, &payload[6], dlen) == 0U)
  {
    bp_reply(BS_CMD_DATA, BS_STATUS_ERR, (const uint8_t *)0, 0U);
    return;
  }

  /* ---- Step3: 应答当前序号(主机按序核对) ---- */
  {
    uint8_t rsp[2];                                     // 序号应答
    rsp[0] = (uint8_t)(seq & 0xFFU);
    rsp[1] = (uint8_t)(seq >> 8);
    bp_reply(BS_CMD_DATA, BS_STATUS_OK, rsp, 2U);
  }
}

/*********************************************************************
 * 函 数 名: bp_handle_verify
 * 入口参数: payload   [size(4B)][crc32(4B)](均为小端)
 * 出口参数: 无
 * 返回值说明: 无
 * 返 回 值: void
 * 函数功能: 计算已写入 APP 区的 CRC32 并与主机期望值比对
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void bp_handle_verify(const uint8_t *payload)
{
  uint32_t size;                                        // 镜像大小
  uint32_t crc_host;                                    // 主机期望 CRC32
  uint32_t crc_dev;                                     // 设备计算 CRC32

  /* ---- Step1: 解析参数 ---- */
  size = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
       | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
  crc_host = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8)
           | ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24);
  if((size == 0U) || (size > BS_APP_MAX_SIZE))
  {
    bp_reply(BS_CMD_VERIFY, BS_STATUS_ERR, (const uint8_t *)0, 0U);
    return;
  }

  /* ---- Step2: 全区 CRC32 计算(无表逐位, 1MB 约 0.3s) ---- */
  crc_dev = bs_crc32_calc((const uint8_t *)BS_APP_BASE, size);

  /* ---- Step3: 比对应答 ---- */
  bp_reply(BS_CMD_VERIFY,
           (crc_dev == crc_host) ? BS_STATUS_OK : BS_STATUS_ERR,
           (const uint8_t *)0, 0U);
}

/*********************************************************************
 * 函 数 名: bp_handle_done
 * 入口参数: payload   [size(4B)][crc32(4B)](均为小端)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 升级收尾: 写参数页(有效标志) -> 应答 -> 跳转新 APP
 * 说    明: 应答发出后才跳转; 参数页写失败则回 ERR 等待主机重试
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void bp_handle_done(const uint8_t *payload)
{
  boot_param_t param;                                   // 新参数页内容
  uint32_t size;                                        // 镜像大小
  uint32_t crc;                                         // 镜像 CRC32
  uint32_t wait;                                        // 发送等待循环计数

  /* ---- Step1: 解析并组装新参数页 ---- */
  size = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
       | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
  crc = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8)
      | ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24);
  if((size == 0U) || (size > BS_APP_MAX_SIZE))
  {
    bp_reply(BS_CMD_DONE, BS_STATUS_ERR, (const uint8_t *)0, 0U);
    return;
  }
  param.magic = BS_PARAM_MAGIC;
  param.app_size = size;
  param.app_crc32 = crc;
  param.flags = BS_FLAG_APP_VALID;                      // 升级请求同时清零

  /* ---- Step2: 写参数页(失败回 ERR) ---- */
  if(bs_param_write(&param) == 0U)
  {
    bp_reply(BS_CMD_DONE, BS_STATUS_ERR, (const uint8_t *)0, 0U);
    return;
  }

  /* ---- Step3: 应答 OK 并等待发出(最多约 100ms) ---- */
  bp_reply(BS_CMD_DONE, BS_STATUS_OK, (const uint8_t *)0, 0U);
  wait = 0x00FFFFFFU;
  while((usb_cdc_send_idle() == 0U) && (wait-- != 0U))
  {
  }

  /* ---- Step4: 断开 USB 并跳转新 APP(不返回) ---- */
  usb_cdc_deinit();
  boot_jump_app();
}

/*********************************************************************
 * 函 数 名: boot_protocol_run
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 升级协议主循环: WFI 等帧 -> 分发命令(永不主动退出)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
void boot_protocol_run(void)
{
  s_rx_pos = 0;
  s_rx_need = 0;
  s_frame_ready = 0;

  for(;;)
  {
    /* ---- Step1: 等待完整帧(USB 中断唤醒) ---- */
    if(s_frame_ready == 0U)
    {
      __WFI();
      continue;
    }

    /* ---- Step2: 分发命令(payload 指针跳过 4B 帧头) ---- */
    {
      uint8_t  cmd = s_rx_buf[1];                       // 命令码
      const uint8_t *payload = &s_rx_buf[BS_FRAME_HDR_LEN];

      switch(cmd)
      {
      /* ---- HELLO: 握手应答 ---- */
      case BS_CMD_HELLO:
        bp_handle_hello();
        break;
      /* ---- ERASE: 擦除 APP 区 ---- */
      case BS_CMD_ERASE:
        if(s_rx_need >= 4U)
        {
          bp_handle_erase(payload);
        }
        else
        {
          bp_reply(BS_CMD_ERASE, BS_STATUS_ERR, (const uint8_t *)0, 0U);
        }
        break;
      /* ---- DATA: 写入一包镜像数据 ---- */
      case BS_CMD_DATA:
        if(s_rx_need >= 6U)
        {
          bp_handle_data(payload);
        }
        else
        {
          bp_reply(BS_CMD_DATA, BS_STATUS_ERR, (const uint8_t *)0, 0U);
        }
        break;
      /* ---- VERIFY: CRC32 校验 ---- */
      case BS_CMD_VERIFY:
        if(s_rx_need >= 8U)
        {
          bp_handle_verify(payload);
        }
        else
        {
          bp_reply(BS_CMD_VERIFY, BS_STATUS_ERR, (const uint8_t *)0, 0U);
        }
        break;
      /* ---- DONE: 收尾并跳转 ---- */
      case BS_CMD_DONE:
        if(s_rx_need >= 8U)
        {
          bp_handle_done(payload);
        }
        else
        {
          bp_reply(BS_CMD_DONE, BS_STATUS_ERR, (const uint8_t *)0, 0U);
        }
        break;
      /* ---- ABORT: 中止(复位协议状态, flash 不动) ---- */
      case BS_CMD_ABORT:
        bp_reply(BS_CMD_ABORT, BS_STATUS_OK, (const uint8_t *)0, 0U);
        break;
      /* ---- REBOOT: 已在 BOOT, 忽略 ---- */
      case BS_CMD_REBOOT:
        bp_reply(BS_CMD_REBOOT, BS_STATUS_ERR, (const uint8_t *)0, 0U);
        break;
      /* ---- 未知命令 ---- */
      default:
        bp_reply(cmd, BS_STATUS_ERR, (const uint8_t *)0, 0U);
        break;
      }
    }

    /* ---- Step3: 释放帧缓冲 ---- */
    s_rx_pos = 0;
    s_rx_need = 0;
    s_frame_ready = 0;
  }
}
