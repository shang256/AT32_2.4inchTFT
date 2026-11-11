/**
  **************************************************************************
  * @file    usb_cdc.h
  * @brief   精简 USB CDC 虚拟串口设备栈接口(BOOT/APP 共享)
  *
  * 说明: 基于 AT32F403A USBFS 设备控制器(F1 风格, PA11=DM/PA12=DP),
  *       寄存器级实现枚举与 CDC 批量收发, Windows 免驱动枚举为 COM 口;
  *       收到 bulk 数据时在 USB 中断上下文回调 usb_cdc_on_rx(),
  *       该回调由使用方(BOOT 协议层/APP 升级层)实现。
  **************************************************************************
  */

#ifndef __USB_CDC_H
#define __USB_CDC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"
#include <stdint.h>

void usb_cdc_init(void);                      // 初始化 USB 外设并连接总线(上电)
void usb_cdc_deinit(void);                    // 断开总线并复位 USB 外设(跳转前清理)
uint8_t usb_cdc_ready(void);                  // 1=已枚举配置完成(可收发)
uint8_t usb_cdc_send(const uint8_t *buf, uint16_t len);  // 启动 bulk IN 发送, 1=已启动 0=忙/未就绪
uint8_t usb_cdc_send_idle(void);              // 1=上次发送已完成(可再 send)

/* 使用方必须实现的 bulk 数据接收回调(USB 中断上下文, 不可阻塞/擦写 flash) */
void usb_cdc_on_rx(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __USB_CDC_H */
