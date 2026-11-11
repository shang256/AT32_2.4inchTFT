/**
  **************************************************************************
  * @file    boot_shared.h
  * @brief   BOOT/APP 共享定义(Flash 分区/参数页/升级协议帧格式与命令)
  *
  * 说明: 本文件被 BOOT 工程(Software/Bootloader)与 APP 工程(Software/APP)
  *       同时包含, 是两端升级联动的"协议契约", 修改字段必须两端同步;
  *       目录位于 Software/Shared, 由两工程以相对路径共同引用。
  **************************************************************************
  */

#ifndef __BOOT_SHARED_H
#define __BOOT_SHARED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

/* ==================== Flash 分区规划(1MB) ==================== */

/* BOOT 区: 0x08000000 ~ 0x08002FFF, 目标 <=12KB */
#define BS_BOOT_BASE          0x08000000UL                 // BOOT 起始(向量表)
#define BS_BOOT_SIZE          (12UL * 1024UL)              // BOOT 分区大小

/* APP 区: 0x08003000 ~ 0x0807F7FF, 由上位机擦写 */
#define BS_APP_BASE           (0x08000000UL + 0x3000UL)    // APP 链接基址 0x08003000
#define BS_APP_MAX_SIZE       (0x100000UL - 0x3000UL - 0x800UL)  // ~988KB(参数页前)

/* 参数页: 0x0807F800(最后一页 2KB), 存升级标志/APP 尺寸与 CRC */
#define BS_PARAM_PAGE_ADDR    0x0807F800UL                 // 参数页起始地址
#define BS_FLASH_PAGE_SIZE    0x800UL                      // 内部 flash 页大小(2KB)

/* 参数页有效性魔数('B''T''P''1' 小端) */
#define BS_PARAM_MAGIC        0x31505442UL

/* 参数页 flags 位定义 */
#define BS_FLAG_UPDATE_REQ    0x00000001UL                 // 升级请求(APP 置位, BOOT 清零)
#define BS_FLAG_APP_VALID     0x00000002UL                 // APP 镜像有效(BOOT 升级成功后置位)

/** 参数页结构(位于 BS_PARAM_PAGE_ADDR, 更新需整页擦除重编程) */
typedef struct
{
  uint32_t magic;                                            // 参数页魔数(BS_PARAM_MAGIC)
  uint32_t app_size;                                         // APP 镜像字节数
  uint32_t app_crc32;                                        // APP 镜像 CRC32(IEEE 802.3, 与 zlib.crc32 一致)
  uint32_t flags;                                            // BS_FLAG_xxx 标志位
} boot_param_t;

/* ==================== 升级协议(USB CDC 之上) ==================== */

/* 帧头(主机->设备 0xA5, 设备->主机 0x5A) */
#define BS_FRAME_HEAD_HOST    0xA5
#define BS_FRAME_HEAD_DEV     0x5A

/* 协议版本 */
#define BS_PROTO_VER          0x01

/* 帧固定头长度: [head][cmd][len_lo][len_hi] */
#define BS_FRAME_HDR_LEN      4

/* 命令码 */
#define BS_CMD_HELLO          0x01                          // 握手(双方): 查询模式与状态
#define BS_CMD_REBOOT         0x02                          // 重启进 BOOT(仅 APP 响应)
#define BS_CMD_ERASE          0x10                          // 擦除 APP 区(仅 BOOT)
#define BS_CMD_DATA           0x11                          // 数据包 1KB(仅 BOOT)
#define BS_CMD_VERIFY         0x12                          // CRC32 校验(仅 BOOT)
#define BS_CMD_DONE           0x13                          // 升级完成置有效并跳转(仅 BOOT)
#define BS_CMD_ABORT          0x14                          // 中止升级(仅 BOOT)

/* 应答状态码 */
#define BS_STATUS_OK          0x00                          // 成功
#define BS_STATUS_ERR         0x01                          // 失败
#define BS_STATUS_BUSY        0x02                          // 忙

/* HELLO 应答 payload: [mode][proto_ver][app_valid] */
#define BS_MODE_BOOT          0x00                          // 当前运行在 BOOT
#define BS_MODE_APP           0x01                          // 当前运行在 APP

/* 单包数据最大长度(DATA 命令 payload 中 data 字段上限) */
#define BS_DATA_MAX_LEN       1024UL

/* USB 设备识别(自定义 VID/PID, Windows 免驱动枚举为虚拟串口) */
#define BS_USB_VID            0x2E3C
#define BS_USB_PID            0x5741

/* CRC32 结果初值(APP 无效时的占位) */
#define BS_CRC32_INVALID      0xFFFFFFFFUL

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_SHARED_H */
