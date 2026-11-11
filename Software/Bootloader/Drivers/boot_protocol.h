/**
  **************************************************************************
  * @file    boot_protocol.h
  * @brief   BOOT 升级协议状态机接口
  *
  * 协议帧格式(见 boot_shared.h):
  *   主机->设备: [0xA5][cmd][len16LE][payload]
  *   设备->主机: [0x5A][cmd][status][len16LE][payload]
  *
  * 升级流程: HELLO -> ERASE(按镜像大小擦 APP 区) -> DATA(1KB/包,
  *   等 ACK 顺序发包) -> VERIFY(CRC32 比对) -> DONE(写参数页并跳转新 APP)
  *
  * 线程模型: usb_cdc_on_rx() 在 USB 中断上下文做帧重组(仅写缓冲与
  *   标志), 命令处理(含 flash 擦写)在 boot_protocol_run() 主循环执行,
  *   擦写期间 bulk OUT 端点保持 NAK 形成天然流控。
  **************************************************************************
  */

#ifndef __BOOT_PROTOCOL_H
#define __BOOT_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f403a_407.h"

void boot_protocol_run(void);                      // 协议主循环(不返回, DONE 后内部跳转 APP)
void boot_jump_app(void) __attribute__((noreturn)); // 跳转 APP(boot_main.c 实现, 供协议层 DONE 调用)

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_PROTOCOL_H */
