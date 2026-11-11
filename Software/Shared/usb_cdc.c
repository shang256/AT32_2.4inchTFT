/**
  **************************************************************************
  * @file    usb_cdc.c
  * @brief   精简 USB CDC 虚拟串口设备栈实现(BOOT/APP 共享)
  *
  * 硬件基础: AT32F403A USBFS 设备控制器(寄存器布局见
  *   at32f403a_407_usb.h: ept[8]@0x00 / ctrl@0x40 / intsts@0x44 /
  *   devaddr@0x4C / buftbl@0x50 / cfg@0x60, PMA 包缓冲 0x40006000)。
  *
  * 端点分配(USB 全速):
  *   EP0  控制传输(bMaxPacketSize0=32)
  *   EP1  CDC 数据接口: IN bulk(0x81) + OUT bulk(0x01), 各 64B
  *   EP2  CDC 通信接口通知 IN interrupt(0x82, 描述符存在但不发送)
  *
  * PMA 布局(字索引, 每字 2 字节有效数据占 4 字节地址空间):
  *   BTABLE 描述符 0x00~0x17 | EP0 TX 0x18 | EP0 RX 0x38
  *   EP1 TX(bulk IN) 0x58 | EP1 RX(bulk OUT) 0x98 | EP2 TX(通知) 0xD8
  *
  * EPxR 写规则(F1 兼容): DTOG 位写 0 保持; CTR_TX/CTR_RX 写 0 清除、
  *   写 1 无效; SETUP 标志只读; STAT_TX/STAT_RX 直接写新值。
  *   本文件统一用 cdc_ept_write() 一次完成"清本次 CTR + 设两方向状态"。
  *
  * 时钟: crm_usb_clock_source_select(HICK) 内部 48MHz, 与 PLL 无关。
  **************************************************************************
  */

#include "usb_cdc.h"
#include "boot_shared.h"

/* ==================== PMA 缓冲布局(字索引) ==================== */
#define CDC_PMA_EP0_TX        0x18                    // EP0 IN 缓冲(32B)
#define CDC_PMA_EP0_RX        0x38                    // EP0 OUT 缓冲(32B)
#define CDC_PMA_EP1_TX        0x58                    // EP1 bulk IN 缓冲(64B)
#define CDC_PMA_EP1_RX        0x98                    // EP1 bulk OUT 缓冲(64B)
#define CDC_PMA_EP2_TX        0xD8                    // EP2 通知 IN 缓冲(8B)

/* 端点号 */
#define CDC_EP0               0                       // 控制端点
#define CDC_EP1               1                       // bulk 数据端点(双向)
#define CDC_EP2               2                       // 通知端点(IN)

/* EP0 控制传输最大包长(设备描述符声明 32) */
#define CDC_EP0_MAXPKT        32

/* bulk 最大包长(全速 64) */
#define CDC_BULK_MAXPKT       64

/* 官方宏引用的 PMA 基址全局变量(at32f403a_407_usb.h extern 声明) */
uint32_t g_usb_packet_address = USB_PACKET_BUFFER_ADDRESS;

/* ==================== 模块状态 ==================== */
static volatile uint8_t  s_configured;                // 1=已收到 SET_CONFIGURATION(非 0)
static volatile uint8_t  s_tx_busy;                   // 1=bulk IN 发送进行中
static volatile uint8_t  s_addr_pending;              // SET_ADDRESS 待生效地址(立即生效方案则不用)
static uint8_t           s_line_coding[7];            // CDC 波特率等参数缓存(默认 115200-8-N-1)
static volatile uint16_t s_dtr;                       // SET_CONTROL_LINE_STATE 的 DTR 位

/* EP0 控制传输状态机 */
typedef enum
{
  CDC_EP0_IDLE = 0,                                   // 空闲等待 setup
  CDC_EP0_DATA_IN,                                    // 数据 IN 阶段(发描述符等)
  CDC_EP0_DATA_OUT,                                   // 数据 OUT 阶段(收 line coding)
  CDC_EP0_STATUS_IN                                   // 状态阶段(IN 方向 ZLP)
} cdc_ep0_state_t;

static cdc_ep0_state_t s_ep0_state;                   // EP0 当前阶段
static uint16_t        s_ep0_tx_total;                // 数据 IN 总字节数
static uint16_t        s_ep0_tx_sent;                 // 数据 IN 已发送
static uint16_t        s_ep0_out_expect;              // 数据 OUT 期望字节数
static uint16_t        s_ep0_out_got;                 // 数据 OUT 已接收
static const uint8_t  *s_ep0_tx_src;                  // 数据 IN 源指针(描述符)

/* EP0 接收/setup 缓冲(需保持静态生命周期, 回调可能延迟读取) */
static uint8_t s_ep0_rx_buf[CDC_EP0_MAXPKT];

/* ==================== USB 描述符 ==================== */

/* 设备描述符(18B, IAD 组合设备) */
static const uint8_t s_dev_desc[18] =
{
  0x12,                                               // bLength
  0x01,                                               // bDescriptorType(设备)
  0x00, 0x02,                                         // bcdUSB 2.00
  0xEF, 0x02, 0x01,                                   // 类:Misc/IAD 组合
  CDC_EP0_MAXPKT,                                     // bMaxPacketSize0
  (uint8_t)(BS_USB_VID & 0xFF), (uint8_t)(BS_USB_VID >> 8),
  (uint8_t)(BS_USB_PID & 0xFF), (uint8_t)(BS_USB_PID >> 8),
  0x00, 0x01,                                         // bcdDevice 1.00
  0x01,                                               // iManufacturer(字符串 1)
  0x02,                                               // iProduct(字符串 2)
  0x00,                                               // iSerialNumber(无)
  0x01                                                // bNumConfigurations
};

/* 配置描述符(75B: IAD + 通信接口 + CDC 功能头 + 通知 EP + 数据接口 + 双 bulk EP) */
static const uint8_t s_cfg_desc[75] =
{
  /* Configuration */
  0x09, 0x02, 75, 0x00,                               // 总长 75
  0x02,                                               // bNumInterfaces(2)
  0x01, 0x00,                                         // bConfigurationValue/字符串
  0x80, 0x32,                                         // 总线供电/500mA
  /* IAD: 接口 0+1 组合成一个 CDC 功能 */
  0x08, 0x0B, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00,
  /* Interface 0: 通信类 */
  0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
  /* CDC Header */
  0x05, 0x24, 0x00, 0x10, 0x01,
  /* CDC Call Management */
  0x05, 0x24, 0x01, 0x00, 0x01,
  /* CDC ACM */
  0x04, 0x24, 0x02, 0x02,
  /* CDC Union(主接口 0, 从接口 1) */
  0x05, 0x24, 0x06, 0x00, 0x01,
  /* EP2 IN 中断(通知, 8B/10ms) */
  0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0A,
  /* Interface 1: 数据类 */
  0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
  /* EP1 IN bulk(64B) */
  0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
  /* EP1 OUT bulk(64B) */
  0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00
};

/* 字符串 0: 语言 ID(英语-美国) */
static const uint8_t s_str_lang[4] = { 0x04, 0x03, 0x09, 0x04 };

/* 字符串 1: 厂商名 */
static const uint8_t s_str_manu[10] =
{
  0x0A, 0x03, 'A', 0, 'T', 0, '3', 0, '2', 0
};

/* 字符串 2: 产品名 */
static const uint8_t s_str_prod[24] =
{
  0x18, 0x03,
  'A', 0, 'T', 0, '3', 0, '2', 0, ' ', 0,
  'U', 0, 'p', 0, 'g', 0, 'r', 0, 'a', 0, 'd', 0, 'e', 0
};

/* CDC 默认线路参数(115200-8-N-1, 内容对枚举无影响) */
static const uint8_t s_line_coding_default[7] =
{
  0x00, 0xC2, 0x01, 0x00,                             // 115200bps(小端)
  0x00, 0x00, 0x08                                    // 1 停止位/无校验/8 数据位
};

/* ==================== PMA 读写 ==================== */

/*********************************************************************
 * 函 数 名: cdc_pma_read
 * 入口参数: dst          目标缓冲
 *           pma_word_off PMA 起始字索引(BTABLE 描述符中的地址值)
 *           n            字节数
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 从 USB PMA 包缓冲读 n 字节到目标缓冲
 * 说    明: PMA 每个 16 位有效字占 4 字节地址空间(参照官方
 *           usb_read_packet 的双自增指针模式)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void cdc_pma_read(uint8_t *dst, uint16_t pma_word_off, uint16_t n)
{
  volatile uint16_t *s = (volatile uint16_t *)(g_usb_packet_address + (uint32_t)pma_word_off * 2U);
  uint16_t i;                                         // 16 位字循环索引

  for(i = 0; i < (uint16_t)(n >> 1); i++)
  {
    uint16_t w = *s;                                  // 读一个 16 位有效字
    s += 2;                                           // 跨过 32 位地址间隔
    dst[i * 2U] = (uint8_t)w;
    dst[i * 2U + 1U] = (uint8_t)(w >> 8);
  }
  if((n & 1U) != 0U)
  {
    dst[n - 1U] = (uint8_t)(*s);                      // 尾部单字节
  }
}

/*********************************************************************
 * 函 数 名: cdc_pma_write
 * 入口参数: src          源缓冲
 *           pma_word_off PMA 起始字索引
 *           n            字节数
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 将 n 字节写入 USB PMA 包缓冲
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void cdc_pma_write(const uint8_t *src, uint16_t pma_word_off, uint16_t n)
{
  volatile uint16_t *d = (volatile uint16_t *)(g_usb_packet_address + (uint32_t)pma_word_off * 2U);
  uint16_t i;                                         // 16 位字循环索引

  for(i = 0; i < (uint16_t)(n >> 1); i++)
  {
    *d = (uint16_t)((uint16_t)src[i * 2U] | ((uint16_t)src[i * 2U + 1U] << 8));
    d += 2;                                           // 跨过 32 位地址间隔
  }
  if((n & 1U) != 0U)
  {
    *d = (uint16_t)src[n - 1U];                       // 尾部单字节
  }
}

/* ==================== 端点寄存器安全写 ==================== */

/*********************************************************************
 * 函 数 名: cdc_ept_write
 * 入口参数: ep         端点号(0~3)
 *           stat_tx    新的 TX 方向状态(USB_TX_xxx, 0=保持不变)
 *           stat_rx    新的 RX 方向状态(USB_RX_xxx, 0=保持不变)
 *           clr_flags  需要清除的完成标志(USB_TXTC/USB_RXTC 按位或, 0=不清)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 一次写 EPxR 完成"清完成标志 + 同时设置 TX/RX 两方向状态"
 * 说    明: 官方 SET_TXSTS/SET_RXSTS 宏会清掉另一方向状态字段,
 *           对 EP0(双向)与 EP1(双向 bulk)不适用, 故统一走本函数;
 *           DTOG 位恒写 0(保持), SETUP 标志为只读自然保留
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void cdc_ept_write(uint8_t ep, uint32_t stat_tx, uint32_t stat_rx, uint32_t clr_flags)
{
  uint32_t v = USB->ept[ep];                          // 现值(EPTADDR/TYPE/SETUP 等保留)

  v &= ~(USB_TXSTS | USB_RXSTS | USB_TXDTS | USB_RXDTS | USB_TXTC | USB_RXTC);
  v |= (stat_tx & USB_TXSTS) | (stat_rx & USB_RXSTS);
  v &= ~clr_flags;                                    // 指定位写 0 清除完成标志
  USB->ept[ep] = v;
}

/* ==================== EP0 控制传输 ==================== */

/*********************************************************************
 * 函 数 名: ep0_start_in_zlp
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: EP0 发送 0 长包进入/完成状态阶段(IN 方向)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void ep0_start_in_zlp(void)
{
  USB_SET_TXLEN(CDC_EP0, 0);                          // 发送长度 0
  cdc_ept_write(CDC_EP0, USB_TX_VALID, 0, USB_TXTC);  // 启动 IN ZLP
  s_ep0_state = CDC_EP0_STATUS_IN;
}

/*********************************************************************
 * 函 数 名: ep0_stall
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: EP0 双向 STALL(不支持的控制请求), 等主机发起新 setup
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void ep0_stall(void)
{
  s_ep0_state = CDC_EP0_IDLE;
  cdc_ept_write(CDC_EP0, USB_TX_STALL, USB_RX_STALL, USB_TXTC | USB_RXTC);
}

/*********************************************************************
 * 函 数 名: ep0_send_data
 * 入口参数: src       数据源(描述符等)
 *           total     本次控制 IN 传输总长(min(描述符长, wLength))
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 启动 EP0 数据 IN 阶段(首包立即写入 PMA 并置 VALID)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void ep0_send_data(const uint8_t *src, uint16_t total)
{
  uint16_t n = (total > CDC_EP0_MAXPKT) ? CDC_EP0_MAXPKT : total;   // 首包长度

  s_ep0_tx_src = src;
  s_ep0_tx_total = total;
  s_ep0_tx_sent = 0;
  cdc_pma_write(src, CDC_PMA_EP0_TX, n);              // 首包进 PMA
  USB_SET_TXLEN(CDC_EP0, n);
  s_ep0_tx_sent = n;
  s_ep0_state = CDC_EP0_DATA_IN;
  cdc_ept_write(CDC_EP0, USB_TX_VALID, 0, USB_TXTC);
}

/*********************************************************************
 * 函 数 名: ep0_get_string_desc
 * 入口参数: index   字符串索引(1=厂商 2=产品)
 * 出口参数: 无
 * 返 回 值: const uint8_t* 字符串描述符指针(未知索引返回 0)
 * 函数功能: 按索引查找字符串描述符
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static const uint8_t *ep0_get_string_desc(uint16_t index)
{
  if(index == 0U)
  {
    return s_str_lang;                                // 语言 ID
  }
  if(index == 1U)
  {
    return s_str_manu;                                // 厂商名
  }
  if(index == 2U)
  {
    return s_str_prod;                                // 产品名
  }
  return (const uint8_t *)0;                          // 不支持
}

/*********************************************************************
 * 函 数 名: ep0_handle_setup
 * 入口参数: setup   8 字节 setup 包(已从 PMA 读出)
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 解析并分发 EP0 控制请求(标准请求 + CDC 类请求)
 * 说    明: SET_ADDRESS 立即写 devaddr(F1 惯例, 主机容忍);
 *           描述符请求按 wLength 截断发送; 不支持请求回 STALL
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void ep0_handle_setup(const uint8_t *setup)
{
  uint8_t  type = setup[0];                           // bmRequestType
  uint8_t  req  = setup[1];                           // bRequest
  uint16_t val  = (uint16_t)((uint16_t)setup[2] | ((uint16_t)setup[3] << 8));   // wValue
  uint16_t idx  = (uint16_t)((uint16_t)setup[4] | ((uint16_t)setup[5] << 8));   // wIndex
  uint16_t len  = (uint16_t)((uint16_t)setup[6] | ((uint16_t)setup[7] << 8));   // wLength

  (void)idx;

  /* ---- Step1: 标准设备请求(bmRequestType 高位 0x00/0x80) ---- */
  if((type & 0x60U) == 0x00U)
  {
    if((type & 0x80U) != 0U)                          // 主机读(数据 IN)
    {
      if(req == 0x00U)                                // GET_STATUS
      {
        static const uint8_t sts[2] = { 0x00, 0x00 }; // 总线供电/无远程唤醒
        ep0_send_data(sts, (len > 2U) ? 2U : len);
        return;
      }
      if(req == 0x06U)                                // GET_DESCRIPTOR
      {
        uint8_t dtype = (uint8_t)(val >> 8);          // 描述符类型
        uint8_t didx = (uint8_t)(val & 0xFFU);        // 描述符索引
        const uint8_t *d = (const uint8_t *)0;
        uint16_t dlen = 0;

        if(dtype == 0x01U)                            // 设备描述符
        {
          d = s_dev_desc;
          dlen = 18U;
        }
        else if(dtype == 0x02U)                       // 配置描述符
        {
          d = s_cfg_desc;
          dlen = 75U;
        }
        else if(dtype == 0x03U)                       // 字符串描述符
        {
          d = ep0_get_string_desc(didx);
          dlen = (d != (const uint8_t *)0) ? d[0] : 0U;
        }
        if(d != (const uint8_t *)0)
        {
          ep0_send_data(d, (len < dlen) ? len : dlen);   // 按请求长度截断
          return;
        }
        ep0_stall();                                  // 不支持的描述符(qualifier 等)
        return;
      }
      if(req == 0x0AU)                                // GET_INTERFACE
      {
        static const uint8_t ifn[1] = { 0x00 };
        ep0_send_data(ifn, (len > 1U) ? 1U : len);
        return;
      }
      ep0_stall();
      return;
    }
    /* ---- 主机写(无数据/数据 OUT) ---- */
    if(req == 0x05U)                                  // SET_ADDRESS
    {
      USB->devaddr = 0x80UL | (uint32_t)(val & 0x7FU);   // 立即生效(含使能位)
      ep0_start_in_zlp();
      return;
    }
    if(req == 0x09U)                                  // SET_CONFIGURATION
    {
      s_configured = (val != 0U) ? 1U : 0U;
      ep0_start_in_zlp();
      return;
    }
    if((req == 0x01U) || (req == 0x03U))              // CLEAR/SET_FEATURE
    {
      ep0_start_in_zlp();                             // 简化: 不处理端点 halt 恢复
      return;
    }
    if(req == 0x0BU)                                  // SET_INTERFACE
    {
      ep0_start_in_zlp();
      return;
    }
    ep0_stall();
    return;
  }

  /* ---- Step2: CDC 类请求(接口接收方 0x20/0xA0) ---- */
  if((type & 0x60U) == 0x20U)
  {
    if(req == 0x20U)                                  // SET_LINE_CODING(数据 OUT 7B)
    {
      s_ep0_out_expect = 7U;
      s_ep0_out_got = 0;
      s_ep0_state = CDC_EP0_DATA_OUT;
      cdc_ept_write(CDC_EP0, 0, USB_RX_VALID, USB_RXTC);
      return;
    }
    if(req == 0x21U)                                  // GET_LINE_CODING(数据 IN 7B)
    {
      ep0_send_data(s_line_coding, (len > 7U) ? 7U : len);
      return;
    }
    if(req == 0x22U)                                  // SET_CONTROL_LINE_STATE
    {
      s_dtr = (uint16_t)(val & 0x01U);                // bit0=DTR
      ep0_start_in_zlp();
      return;
    }
    ep0_stall();
    return;
  }

  /* ---- Step3: 其余类别的请求一律 STALL ---- */
  ep0_stall();
}

/* ==================== 中断处理 ==================== */

/*********************************************************************
 * 函 数 名: cdc_ep0_isr
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: EP0 控制传输中断处理(RX: setup/OUT 数据/状态 ZLP, IN: 续发)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void cdc_ep0_isr(void)
{
  uint32_t epr = USB->ept[CDC_EP0];                   // 现值(判断完成标志)

  /* ---- Step1: RX 方向事件(setup / OUT 数据 / OUT 状态 ZLP) ---- */
  if((epr & USB_RXTC) != 0U)
  {
    if((epr & USB_SETUPTC) != 0U)                     // setup 包
    {
      uint16_t n = USB_GET_RX_LEN(CDC_EP0);           // 应为 8
      if(n > CDC_EP0_MAXPKT)
      {
        n = CDC_EP0_MAXPKT;
      }
      cdc_pma_read(s_ep0_rx_buf, CDC_PMA_EP0_RX, n);
      cdc_ept_write(CDC_EP0, 0, USB_RX_VALID, USB_RXTC);   // 清标志并恢复接收
      ep0_handle_setup(s_ep0_rx_buf);
      return;
    }
    /* 普通 OUT 包: 数据阶段或状态 ZLP */
    {
      uint16_t n = USB_GET_RX_LEN(CDC_EP0);

      if(s_ep0_state == CDC_EP0_DATA_OUT)             // SET_LINE_CODING 数据阶段
      {
        cdc_pma_read(s_line_coding, CDC_PMA_EP0_RX, (n < 7U) ? n : 7U);
        ep0_start_in_zlp();                           // 数据收齐 -> 状态阶段
      }
      else                                            // 状态阶段 ZLP(数据 IN 完成)
      {
        s_ep0_state = CDC_EP0_IDLE;
        cdc_ept_write(CDC_EP0, 0, USB_RX_VALID, USB_RXTC);
      }
    }
    return;
  }

  /* ---- Step2: TX 方向事件(数据 IN 续发或状态 ZLP 完成) ---- */
  if((epr & USB_TXTC) != 0U)
  {
    if(s_ep0_state == CDC_EP0_DATA_IN)
    {
      if(s_ep0_tx_sent < s_ep0_tx_total)              // 还有数据 -> 续发下一包
      {
        uint16_t rem = s_ep0_tx_total - s_ep0_tx_sent;
        uint16_t n = (rem > CDC_EP0_MAXPKT) ? CDC_EP0_MAXPKT : rem;
        cdc_pma_write(&s_ep0_tx_src[s_ep0_tx_sent], CDC_PMA_EP0_TX, n);
        USB_SET_TXLEN(CDC_EP0, n);
        s_ep0_tx_sent += n;
        cdc_ept_write(CDC_EP0, USB_TX_VALID, 0, USB_TXTC);
      }
      else                                            // 发完 -> 等主机 OUT ZLP 结束
      {
        s_ep0_state = CDC_EP0_IDLE;
        cdc_ept_write(CDC_EP0, 0, USB_RX_VALID, USB_TXTC);
      }
    }
    else                                              // 状态 ZLP 发送完成
    {
      s_ep0_state = CDC_EP0_IDLE;
      cdc_ept_write(CDC_EP0, USB_TX_NAK, 0, USB_TXTC);
    }
  }
}

/*********************************************************************
 * 函 数 名: cdc_epts_config
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: (重)配置全部端点的类型/描述符/初始状态(总线复位后调用)
 * 说    明: EP1 为同号双向 bulk(TX=IN / RX=OUT), 一次配置两个方向;
 *           EP2 通知端点只配置为 NAK(描述符存在但不发送通知)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void cdc_epts_config(void)
{
  /* ---- Step1: EP0 控制端点(描述符 + 双向初始状态) ---- */
  USB_SET_EPT_ADDRESS(CDC_EP0, 0);
  USB_SET_TRANS_TYPE(CDC_EP0, USB_EPT_CONTROL);
  USB_SET_TX_ADDRESS(CDC_EP0, CDC_PMA_EP0_TX);
  USB_SET_RX_ADDRESS(CDC_EP0, CDC_PMA_EP0_RX);
  USB_CLEAR_TXDTS(CDC_EP0);                           // 清数据翻转同步
  USB_CLEAR_RXDTS(CDC_EP0);
  USB_SET_RXLEN(CDC_EP0, CDC_EP0_MAXPKT);             // RX 缓冲容量
  USB_SET_TXLEN(CDC_EP0, 0);
  cdc_ept_write(CDC_EP0, USB_TX_NAK, USB_RX_VALID, USB_TXTC | USB_RXTC);

  /* ---- Step2: EP1 bulk 数据端点(双向) ---- */
  USB_SET_EPT_ADDRESS(CDC_EP1, 1);
  USB_SET_TRANS_TYPE(CDC_EP1, USB_EPT_BULK);
  USB_SET_TX_ADDRESS(CDC_EP1, CDC_PMA_EP1_TX);
  USB_SET_RX_ADDRESS(CDC_EP1, CDC_PMA_EP1_RX);
  USB_CLEAR_TXDTS(CDC_EP1);
  USB_CLEAR_RXDTS(CDC_EP1);
  USB_SET_RXLEN(CDC_EP1, CDC_BULK_MAXPKT);
  USB_SET_TXLEN(CDC_EP1, 0);
  s_tx_busy = 0;
  cdc_ept_write(CDC_EP1, USB_TX_NAK, USB_RX_VALID, USB_TXTC | USB_RXTC);

  /* ---- Step3: EP2 通知端点(IN, 仅存在不使用) ---- */
  USB_SET_EPT_ADDRESS(CDC_EP2, 2);
  USB_SET_TRANS_TYPE(CDC_EP2, USB_EPT_INT);
  USB_SET_TX_ADDRESS(CDC_EP2, CDC_PMA_EP2_TX);
  USB_CLEAR_TXDTS(CDC_EP2);
  USB_SET_TXLEN(CDC_EP2, 0);
  cdc_ept_write(CDC_EP2, USB_TX_NAK, USB_RX_DISABLE, USB_TXTC);
}

/*********************************************************************
 * 函 数 名: cdc_usb_isr
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: USB 总中断处理(复位/传输完成), 由高/低两个向量共同调用
 * 说    明: AT32 硬件将 CTR 事件路由至高优先级向量、复位等其余
 *           事件路由至低优先级向量, 两个入口统一调本函数(标志
 *           已清则自然空转, 重复调用无害); intsts 目标位写 0 清除
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void cdc_usb_isr(void)
{
  uint32_t sts = USB->intsts;                         // 中断状态快照

  /* ---- Step1: 总线复位 -> 恢复地址 0 并重配全部端点 ---- */
  if((sts & USB_RST_FLAG) != 0U)
  {
    USB->intsts = ~USB_RST_FLAG;                      // 写 0 清标志
    s_configured = 0;
    s_tx_busy = 0;
    s_ep0_state = CDC_EP0_IDLE;
    USB->devaddr = 0x80UL;                            // 地址 0 + 使能
    cdc_epts_config();
    return;
  }

  /* ---- Step2: 传输完成事件(按端点分发) ---- */
  if((sts & USB_TC_FLAG) != 0U)
  {
    uint8_t  ep   = (uint8_t)(sts & 0x0FU);           // 事件端点号
    uint8_t  inout = (uint8_t)((sts >> 4) & 0x01U);   // 1=IN 完成

    if(ep == CDC_EP0)
    {
      cdc_ep0_isr();                                  // 控制传输
    }
    else if(ep == CDC_EP1)
    {
      if(inout != 0U)                                 // bulk IN 完成
      {
        s_tx_busy = 0;                                // 允许下一次发送
        cdc_ept_write(CDC_EP1, USB_TX_NAK, 0, USB_TXTC);
      }
      else                                            // bulk OUT 数据到达
      {
        uint16_t n = USB_GET_RX_LEN(CDC_EP1);
        static uint8_t rxbuf[CDC_BULK_MAXPKT];        // 接收暂存(回调同步消费)

        if(n > CDC_BULK_MAXPKT)
        {
          n = CDC_BULK_MAXPKT;
        }
        cdc_pma_read(rxbuf, CDC_PMA_EP1_RX, n);
        cdc_ept_write(CDC_EP1, 0, USB_RX_VALID, USB_RXTC);   // 先恢复接收
        usb_cdc_on_rx(rxbuf, n);                      // 交给使用方(中断上下文)
      }
    }
    else if(ep == CDC_EP2)
    {
      cdc_ept_write(CDC_EP2, USB_TX_NAK, 0, USB_TXTC);   // 通知端点空事件
    }
  }
}

/* 高/低优先级 USB 中断入口(向量名见 startup_at32f403a_407.s) */
void USBFS_H_CAN1_TX_IRQHandler(void)                 // USB 高优先级(CTR)
{
  cdc_usb_isr();
}

void USBFS_L_CAN1_RX0_IRQHandler(void)                // USB 低优先级(复位等)
{
  cdc_usb_isr();
}

/* ==================== 对外接口 ==================== */

/*********************************************************************
 * 函 数 名: usb_cdc_init
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 初始化 USB 外设(HICK 48MHz 时钟 + 端点配置)并连接总线
 * 说    明: 寄存器序列参照官方 usb_dev_init/usb_connect;
 *           puo=0 使能 D+ 1.5k 上拉(实测官方语义, puo=1 断开)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
void usb_cdc_init(void)
{
  uint8_t i;                                          // line coding 拷贝索引

  /* ---- Step1: 状态复位 + 默认线路参数 ---- */
  s_configured = 0;
  s_tx_busy = 0;
  s_ep0_state = CDC_EP0_IDLE;
  s_dtr = 0;
  for(i = 0; i < 7U; i++)
  {
    s_line_coding[i] = s_line_coding_default[i];
  }

  /* ---- Step2: 时钟与中断(USB 48MHz 取自内部 HICK, 不依赖 PLL) ---- */
  crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_USB_PERIPH_CLOCK, TRUE);
  crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK);
  NVIC_SetPriority(USBFS_H_CAN1_TX_IRQn, 0x0EU);      // 低优先级(数值大)
  NVIC_SetPriority(USBFS_L_CAN1_RX0_IRQn, 0x0FU);
  NVIC_ClearPendingIRQ(USBFS_H_CAN1_TX_IRQn);         // 清跳转/复位残留 pending
  NVIC_ClearPendingIRQ(USBFS_L_CAN1_RX0_IRQn);
  NVIC_EnableIRQ(USBFS_H_CAN1_TX_IRQn);
  NVIC_EnableIRQ(USBFS_L_CAN1_RX0_IRQn);

  /* ---- Step3: 外设复位后按官方序列初始化 ---- */
  crm_periph_reset(CRM_USB_PERIPH_RESET, TRUE);
  crm_periph_reset(CRM_USB_PERIPH_RESET, FALSE);
  USB->ctrl_bit.csrst = 0;                            // 清核复位
  USB->intsts = 0;                                    // 清中断状态
  USB->buftbl = 0;                                    // BTABLE 位于 PMA 0
  USB->devaddr = 0x80UL;                              // 地址 0 + 使能
  USB->ctrl |= USB_RST_INT | USB_TC_INT;              // 仅使能复位/完成中断

  /* ---- Step4: 端点配置并连接总线 ---- */
  cdc_epts_config();
  USB->ctrl_bit.disusb = 0;                           // 使能 PHY
  USB->cfg_bit.puo = 0;                               // D+ 上拉生效(主机开始枚举)
}

/*********************************************************************
 * 函 数 名: usb_cdc_deinit
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 断开总线并复位 USB 外设(跳转 APP 前清理, 保证干净状态)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
void usb_cdc_deinit(void)
{
  /* ---- Step1: 断开 D+ 上拉(主机侧设备消失) ---- */
  USB->cfg_bit.puo = 1;                               // 关上拉
  USB->ctrl_bit.disusb = 1;                           // 关 PHY

  /* ---- Step2: 关中断并复位外设 ---- */
  NVIC_DisableIRQ(USBFS_H_CAN1_TX_IRQn);
  NVIC_DisableIRQ(USBFS_L_CAN1_RX0_IRQn);
  crm_periph_reset(CRM_USB_PERIPH_RESET, TRUE);
  crm_periph_reset(CRM_USB_PERIPH_RESET, FALSE);
  s_configured = 0;
  s_tx_busy = 0;
}

/*********************************************************************
 * 函 数 名: usb_cdc_ready
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: uint8_t 1=已配置完成(可收发 bulk)
 * 函数功能: 查询 USB 枚举状态
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
uint8_t usb_cdc_ready(void)
{
  return s_configured;
}

/*********************************************************************
 * 函 数 名: usb_cdc_send_idle
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: uint8_t 1=上次发送已完成(可启动新发送)
 * 函数功能: 查询 bulk IN 发送空闲状态
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
uint8_t usb_cdc_send_idle(void)
{
  return (s_tx_busy == 0U) ? 1U : 0U;
}

/*********************************************************************
 * 函 数 名: usb_cdc_send
 * 入口参数: buf   发送数据
 *           len   字节数(<=64, 0=ZLP)
 * 出口参数: 无
 * 返 回 值: uint8_t 1=已启动 0=忙或未就绪(调用方可重试)
 * 函数功能: 启动一次 bulk IN 发送(单包, 超过 64B 由调用方分包)
 * 说    明: 典型调用场景是中断内应答(帧短), 忙时直接丢弃由
 *           主机超时重发保证可靠
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
uint8_t usb_cdc_send(const uint8_t *buf, uint16_t len)
{
  /* ---- Step1: 就绪与空闲检查 ---- */
  if((s_configured == 0U) || (s_tx_busy != 0U) || (len > CDC_BULK_MAXPKT))
  {
    return 0U;
  }

  /* ---- Step2: 数据进 PMA 并启动 IN 传输 ---- */
  cdc_pma_write(buf, CDC_PMA_EP1_TX, len);
  USB_SET_TXLEN(CDC_EP1, len);
  s_tx_busy = 1;
  cdc_ept_write(CDC_EP1, USB_TX_VALID, 0, USB_TXTC);
  return 1U;
}
