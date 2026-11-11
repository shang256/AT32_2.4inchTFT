/**
  **************************************************************************
  * @file    app_usb_upgrade.h
  * @brief   APP 端 USB 在线升级触发接口(TypeC 虚拟串口, 拖入即升级)
  *
  * 职责: 运行中的 APP 常驻 USB CDC, 应答上位机握手; 收到 REBOOT
  *       命令后在任务上下文写参数页升级请求并复位进 BOOT。
  **************************************************************************
  */

#ifndef __APP_USB_UPGRADE_H
#define __APP_USB_UPGRADE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

void app_usb_upgrade_init(void);                 // 初始化 USB CDC 并连接总线(main 驱动初始化后调用)
uint8_t app_usb_upgrade_poll_reboot(void);       // 检测重启请求(任务上下文周期调用, 返回 1=已复位不返回)

#ifdef __cplusplus
}
#endif

#endif /* __APP_USB_UPGRADE_H */
