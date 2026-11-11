/**
  **************************************************************************
  * @file    flash_drv.h
  * @brief   寄存器级内部 Flash 驱动接口(BOOT/APP 共享)
  *
  * 说明: 仓库 SPL 缺少 at32f403a_407_flash.c 实现, 本模块按
  *       at32f403a_407_flash.h 声明的寄存器语义直接操作 FLASH 外设,
  *       提供 解锁/页擦除/字编程/参数页读写 等原语。
  **************************************************************************
  */

#ifndef __FLASH_DRV_H
#define __FLASH_DRV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"
#include "boot_shared.h"

void flash_drv_unlock(void);                       // 解锁 flash 控制(写 KEY1/KEY2)
void flash_drv_lock(void);                         // 锁定 flash 控制
uint8_t flash_drv_page_erase(uint32_t page_addr);  // 擦除一页(2KB), 1=成功
uint8_t flash_drv_word_program(uint32_t addr, uint32_t data);  // 32 位字编程, 1=成功
uint8_t flash_drv_write(uint32_t addr, const uint8_t *buf, uint32_t len);  // 按字写入(非整字尾部补 0xFF), 1=成功

void bs_param_read(boot_param_t *p);               // 读参数页(magic 无效时输出全 0)
uint8_t bs_param_write(const boot_param_t *p);     // 整页回写参数(擦除+编程), 1=成功

uint32_t bs_crc32_calc(const uint8_t *p, uint32_t len);  // CRC32(IEEE, 无表逐位, 与 zlib.crc32 一致)

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_DRV_H */
