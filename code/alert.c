/* =====================================================================
 * alert.c - 告警状态机实现
 * ---------------------------------------------------------------------
 * alm_step：通用三态状态机（含迟滞）。
 *
 * 迟滞原则：进入告警用高阈值(alarm)，退出用低阈值(exit)，
 *           使状态不会在阈值边界来回抖动。
 *
 * 转移规则：
 *   prev=OK    val>=alarm → ALARM
 *   prev=OK    warn<=val<alarm → WARN
 *   prev=WARN  val>=alarm → ALARM
 *   prev=WARN  val<exit   → OK
 *   prev=ALARM val<exit   → WARN
 *   （其余情况保持原状态）
 */

#include "alert.h"
#include <stdio.h>

AlmState alm_step(AlmState prev, float val, const AlmTh *th)
{
    switch (prev) {
    case ALM_OK:
        if (val >= th->alarm) {
            return ALM_ALARM;            /* 正常→直接进告警（可跨级） */
        } else if (val >= th->warn) {
            return ALM_WARN;             /* 正常→预警 */
        }
        return ALM_OK;                   /* 保持正常 */

    case ALM_WARN:
        if (val >= th->alarm) {
            return ALM_ALARM;            /* 预警→告警（升级） */
        } else if (val < th->exit) {
            return ALM_OK;               /* 预警→正常（迟滞退出） */
        }
        return ALM_WARN;                 /* 保持在预警区间 */

    case ALM_ALARM:
        if (val < th->exit) {
            return ALM_OK;               /* 告警→正常（迟滞退出） */
        }
        return ALM_ALARM;                /* 保持告警 */

    default:
        return ALM_OK;                   /* 异常输入兜底 */
    }
}