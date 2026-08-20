/* =====================================================================
 * filter.h - 滤波模块（环形缓冲 + 差异化滤波）
 * ---------------------------------------------------------------------
 * 需求 FR-P01(环形缓冲30点) / FR-P02(滑动平均) / FR-P03(中值滤波)
 * 决策 D6：按指标噪声特性选型——
 *   温度/湿度/电压 → 滑动平均(缓变信号, 压随机噪声)
 *   烟雾/电流       → 中值滤波(含尖峰脉冲, 去离群点)
 *
 * 统一用 int32 缓冲，可容纳温度(×10)/湿度(×10)/烟雾(ADC)/电压(×100)/电流(×1000)。
 */

#ifndef FILTER_H
#define FILTER_H

#include <stdint.h>
#include <stddef.h>
#include "modbus_env.h"

/* ---------------- 常量 ---------------- */
#define HIST_LEN   30   /* 环形缓冲长度（FR-P01）                  */
#define FILTER_WIN   5   /* 滤波窗口（FR-P02/03，最终来自 app.conf） */

/* 每节点一份历史缓冲；数组下标 = 节点快照索引 */
typedef struct {
    int32_t hist[HIST_LEN];   /* 环形历史数据（各指标按需存入定标原始值） */
    int     head;             /* 写指针：下一个要写入的位置              */
    int     count;            /* 已写入的有效点数（<= HIST_LEN）         */
} RingBuf;

/* 把一个采集值压入环形缓冲（写 head，满则覆盖最旧） */
void   ring_push(RingBuf *rb, int32_t val);

/* 滑动平均：对最近 min(win, count) 个点求整数均值（温度/湿度/电压用） */
int32_t filt_sma(RingBuf *rb, int win);

/* 中值滤波：取最近 min(win, count) 个点排序取中位（烟雾/电流用） */
int32_t filt_median(RingBuf *rb, int win);

#endif /* FILTER_H */