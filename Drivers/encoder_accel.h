/**
  **************************************************************************
  * @file     encoder_accel.h
  * @brief    旋转编码器变速(加速度)通用模块接口
  *
  * 功能说明：
  *   将编码器原始累计增量(delta)按"慢速精准、快速加速"的策略映射为
  *   无量纲"步数"输出。慢速时每格(4 计数)对应 1 步, 快速时每格步进
  *   随速度放大, 不足一格的计数由实例内部累积, 保证任意转速不丢步。
  *
  * 与具体应用解耦：
  *   本模块只做"编码器计数 -> 步数"的变速换算, 不关心"1 步"代表什么
  *   (亮度 1% / 音量 1 格 / 菜单 1 项...), 由调用方按需定义步的物理
  *   含义。每个使用场景维护独立的 encoder_accel_t 实例, 互不干扰。
  **************************************************************************
  */

#ifndef __ENCODER_ACCEL_H
#define __ENCODER_ACCEL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 编码器变速器实例(每个使用场景各自维护一个, 状态互不干扰) */
typedef struct
{
  int32_t accum;   /* 计数累积器: 不足一格的部分保留, 保证精准不丢步 */
} encoder_accel_t;

void encoder_accel_init(encoder_accel_t *accel);                         /* 初始化变速器实例(清零累积器) */
int32_t encoder_accel_feed(encoder_accel_t *accel, int32_t delta);       /* 喂入编码器增量, 返回变速后的"步数"(1 步 = 慢速 1 格) */

#ifdef __cplusplus
}
#endif

#endif /* __ENCODER_ACCEL_H */
