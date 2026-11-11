/**
  **************************************************************************
  * @file    flash_drv.c
  * @brief   寄存器级内部 Flash 驱动实现(BOOT/APP 共享)
  *
  * 实现要点:
  *   - 寄存器操作遵循 at32f403a_407_flash.h 的位定义:
  *     unlock 写 KEY1(0x45670123)/KEY2(0xCDEF89AB) 解锁;
  *     擦页 = ctrl.secers 置位 + addr 装载 + ctrl.erstr 启动, 轮询 sts.obf;
  *     编程 = ctrl.fprgm 置位 + 直接写目标地址, 轮询 sts.obf;
  *     prgmerr/epperr 为错误标志, sts 写 0 清除。
  *   - 参数页(bs_param)读写为 BOOT/APP 联动的标志契约, 更新采取
  *     "RAM 缓存 -> 整页擦除 -> 逐字回写"流程(flash 只能 1->0)。
  *   - 调用约束: 严禁在中断上下文擦写(擦写期间取指停顿会丢 USB 时序),
  *     BOOT 在协议主循环、APP 在任务上下文中调用。
  **************************************************************************
  */

#include "flash_drv.h"

/* flash 解锁键值(见 at32f403a_407_flash.h) */
#define FD_UNLOCK_KEY1      0x45670123UL                  // 解锁顺序键 1
#define FD_UNLOCK_KEY2      0xCDEF89ABUL                  // 解锁顺序键 2

/* 错误标志集合(prgmerr/epperr, 见 at32f403a_407_flash.h bank1 定义) */
#define FD_ERR_FLAGS        0x00000014UL                  // bit2 prgmerr | bit4 epperr

/*********************************************************************
 * 函 数 名: fd_wait_idle
 * 入口参数: timeout   忙等待超时循环次数(近似, 每次约数个周期)
 * 出口参数: 无
 * 返 回 值: uint8_t 1=空闲 0=超时仍忙
 * 函数功能: 轮询 flash 操作忙标志 sts.obf 直至空闲或超时
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static uint8_t fd_wait_idle(uint32_t timeout)
{
  while(FLASH->sts_bit.obf != 0U)
  {
    if(timeout != 0U)
    {
      timeout--;
      if(timeout == 0U)
      {
        return 0U;                                            // 超时仍忙
      }
    }
  }
  return 1U;
}

/*********************************************************************
 * 函 数 名: fd_clear_errors
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 清除 flash 错误标志(prgmerr/epperr, 写 0 清除)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
static void fd_clear_errors(void)
{
  FLASH->sts = ~FD_ERR_FLAGS;                                 // 目标位写 0 清除
}

/*********************************************************************
 * 函 数 名: flash_drv_unlock
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 解锁 flash 操作(依次写入两个解锁键)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
void flash_drv_unlock(void)
{
  FLASH->unlock = FD_UNLOCK_KEY1;                             // 顺序键 1
  FLASH->unlock = FD_UNLOCK_KEY2;                             // 顺序键 2
}

/*********************************************************************
 * 函 数 名: flash_drv_lock
 * 入口参数: 无
 * 出口参数: 无
 * 返 回 值: void
 * 函数功能: 锁定 flash 操作(置位 ctrl.oplk, 防误写)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
void flash_drv_lock(void)
{
  FLASH->ctrl_bit.oplk = 1;                                   // 置位后 flash 控制加锁
}

/*********************************************************************
 * 函 数 名: flash_drv_page_erase
 * 入口参数: page_addr   页起始地址(2KB 对齐)
 * 出口参数: 无
 * 返 回 值: uint8_t 1=擦除成功 0=失败(超时/错误标志)
 * 函数功能: 擦除一页内部 flash(2KB)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
uint8_t flash_drv_page_erase(uint32_t page_addr)
{
  /* ---- Step1: 等待上次操作完成 ---- */
  if(fd_wait_idle(0x00FFFFFFU) == 0U)
  {
    return 0U;
  }

  /* ---- Step2: 配置扇区(页)擦除并装载地址, 启动 ---- */
  FLASH->ctrl_bit.secers = 1;                                 // 选择扇区擦除
  FLASH->addr = page_addr;                                    // 页起始地址
  FLASH->ctrl_bit.erstr = 1;                                  // 启动擦除

  /* ---- Step3: 等待完成 ---- */
  if(fd_wait_idle(0x00FFFFFFU) == 0U)
  {
    FLASH->ctrl_bit.secers = 0;
    return 0U;
  }
  FLASH->ctrl_bit.secers = 0;                                 // 复位擦除选择

  /* ---- Step4: 检查错误标志 ---- */
  if((FLASH->sts & FD_ERR_FLAGS) != 0U)
  {
    fd_clear_errors();
    return 0U;
  }
  return 1U;
}

/*********************************************************************
 * 函 数 名: flash_drv_word_program
 * 入口参数: addr   目标地址(4 字节对齐)
 *           data   32 位数据
 * 出口参数: 无
 * 返 回 值: uint8_t 1=编程成功 0=失败
 * 函数功能: 向内部 flash 编程一个 32 位字
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
uint8_t flash_drv_word_program(uint32_t addr, uint32_t data)
{
  /* ---- Step1: 等待上次操作完成 ---- */
  if(fd_wait_idle(0x00FFFFFFU) == 0U)
  {
    return 0U;
  }

  /* ---- Step2: 置位编程使能后直接写目标地址 ---- */
  FLASH->ctrl_bit.fprgm = 1;
  *(volatile uint32_t *)addr = data;

  /* ---- Step3: 等待完成并复位编程使能 ---- */
  if(fd_wait_idle(0x00FFFFFFU) == 0U)
  {
    FLASH->ctrl_bit.fprgm = 0;
    return 0U;
  }
  FLASH->ctrl_bit.fprgm = 0;

  /* ---- Step4: 检查错误标志 ---- */
  if((FLASH->sts & FD_ERR_FLAGS) != 0U)
  {
    fd_clear_errors();
    return 0U;
  }
  return 1U;
}

/*********************************************************************
 * 函 数 名: flash_drv_write
 * 入口参数: addr   目标起始地址(4 字节对齐)
 *           buf    数据缓冲
 *           len    字节数
 * 出口参数: 无
 * 返 回 值: uint8_t 1=全部写入成功 0=失败
 * 函数功能: 按字向 flash 写入任意长度数据(非整字尾部补 0xFF)
 * 说    明: 补 0xFF 不影响 APP 尾部(擦除态即 0xFF, 且上位机
 *           CRC 按实际镜像长度计算)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
uint8_t flash_drv_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
  uint32_t i;                                                 // 字索引
  uint32_t word;                                              // 组装的 32 位字
  uint32_t words = (len + 3U) / 4U;                           // 向上取整的写入字数

  for(i = 0; i < words; i++)
  {
    /* ---- Step1: 组装一个字(越界字节补 0xFF) ---- */
    word = (uint32_t)buf[i * 4U];
    word |= ((i * 4U + 1U) < len) ? ((uint32_t)buf[i * 4U + 1U] << 8)  : (0xFFUL << 8);
    word |= ((i * 4U + 2U) < len) ? ((uint32_t)buf[i * 4U + 2U] << 16) : (0xFFUL << 16);
    word |= ((i * 4U + 3U) < len) ? ((uint32_t)buf[i * 4U + 3U] << 24) : (0xFFUL << 24);

    /* ---- Step2: 编程 ---- */
    if(flash_drv_word_program(addr + i * 4U, word) == 0U)
    {
      return 0U;
    }
  }
  return 1U;
}

/*********************************************************************
 * 函 数 名: bs_param_read
 * 入口参数: p   参数页输出缓冲(调用方提供)
 * 出口参数: *p 读到的参数(magic 无效时输出全 0)
 * 返 回 值: void
 * 函数功能: 读取参数页并做 magic 校验
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
void bs_param_read(boot_param_t *p)
{
  const boot_param_t *rp = (const boot_param_t *)BS_PARAM_PAGE_ADDR;   // 参数页影子

  /* ---- Step1: magic 匹配则直接读出 ---- */
  if(rp->magic == BS_PARAM_MAGIC)
  {
    p->magic = rp->magic;
    p->app_size = rp->app_size;
    p->app_crc32 = rp->app_crc32;
    p->flags = rp->flags;
  }
  /* ---- Step2: 首次上电/参数页损坏 -> 输出全 0(视为无有效 APP) ---- */
  else
  {
    p->magic = 0;
    p->app_size = 0;
    p->app_crc32 = 0;
    p->flags = 0;
  }
}

/*********************************************************************
 * 函 数 名: bs_param_write
 * 入口参数: p   待写入的参数(调用方填充完整字段)
 * 出口参数: 无
 * 返 回 值: uint8_t 1=写入成功 0=失败
 * 函数功能: 参数页整页回写(擦除 + 逐字编程), 调用前需 flash_drv_unlock
 * 说    明: flash 写只能 1->0, 任一字段变化都必须整页擦除重写
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
uint8_t bs_param_write(const boot_param_t *p)
{
  /* ---- Step1: 擦除参数页 ---- */
  if(flash_drv_page_erase(BS_PARAM_PAGE_ADDR) == 0U)
  {
    return 0U;
  }

  /* ---- Step2: 按字回写结构体 ---- */
  if(flash_drv_word_program(BS_PARAM_PAGE_ADDR + 0U, p->magic) == 0U)
  {
    return 0U;
  }
  if(flash_drv_word_program(BS_PARAM_PAGE_ADDR + 4U, p->app_size) == 0U)
  {
    return 0U;
  }
  if(flash_drv_word_program(BS_PARAM_PAGE_ADDR + 8U, p->app_crc32) == 0U)
  {
    return 0U;
  }
  if(flash_drv_word_program(BS_PARAM_PAGE_ADDR + 12U, p->flags) == 0U)
  {
    return 0U;
  }
  return 1U;
}

/*********************************************************************
 * 函 数 名: bs_crc32_calc
 * 入口参数: p     数据起始地址(可指向 flash 映射区)
 *           len   字节数
 * 出口参数: 无
 * 返 回 值: uint32_t CRC32 值(IEEE 802.3, 与 Python zlib.crc32 一致)
 * 函数功能: 无查表逐位 CRC32 计算(省 1KB 表, 240MHz 下 1MB 约 0.3s)
 * 修改历史:
 *   1.修改人: CodeBuddy
 *     修改日期: 2026年11月11日
 *     修改内容: 新增函数(BOOT 升级需求)
 **********************************************************************/
uint32_t bs_crc32_calc(const uint8_t *p, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;                                // 初值
  uint32_t i;                                                 // 位循环索引

  while(len-- != 0U)
  {
    crc ^= (uint32_t)*p++;
    for(i = 0; i < 8U; i++)
    {
      if((crc & 1UL) != 0U)
      {
        crc = (crc >> 1) ^ 0xEDB88320UL;                      // 反射多项式
      }
      else
      {
        crc >>= 1;
      }
    }
  }
  return crc ^ 0xFFFFFFFFUL;                                  // 输出反转
}
