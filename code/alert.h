/* =====================================================================
 * alert.h - 告警模块：三态状态机(正常→预警→告警) + 迟滞(防临界抖动)
 * ---------------------------------------------------------------------
 * 需求 FR-P05(三态) / FR-P06(迟滞) / FR-P07(动作分级)
 * 决策 D7：进入与退出用不同阈值——进入高、退出低，防临界抖动。
 *
 * 状态转移（以温度为例：预警45℃ 告警55℃ 迟滞退出52℃）：
 *   NORMAL ──≥warn──► WARN（仅本地标注，不上云）
 *   WARN   ──≥alarm─► ALARM（蜂鸣+即时上云）
 *   WARN   ──<exit──► NORMAL
 *   ALARM  ──<exit──► WARN
 *
 * 使用：为每节点每指标各保留一个 AlmState，调用 alm_step 推进即可。
 */

#ifndef ALERT_H
#define ALERT_H

#include <stdint.h>

/* 三态告警级别 */
typedef enum {
    ALM_OK    = 0,  /* 正常   */
    ALM_WARN  = 1,  /* 预警   */
    ALM_ALARM = 2   /* 告警   */
} AlmState;

/* 每个指标一份阈值：进入预警 / 进入告警 / 迟滞退出 */
typedef struct {
    float warn;    /* 预警阈值（进入 WARN 的门限）    */
    float alarm;   /* 告警阈值（进入 ALARM 的门限）   */
    float exit;    /* 迟滞退出阈值（从 WARN/ALARM 回退） */
} AlmTh;

/* 通用状态机：给定上一状态、当前值、阈值表，返回新状态 */
AlmState alm_step(AlmState prev, float val, const AlmTh *th);

#endif /* ALERT_H */