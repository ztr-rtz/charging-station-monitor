/* =====================================================================
 * node_snapshot.h - 共享数据快照模块（处理线程→上层 的数据通道）
 * ---------------------------------------------------------------------
 * 对应 high-level-design.md 决策 D9“共享快照，锁内仅 memcpy”：
 *   - 每节点一个带锁的槽位；读写都在锁内只做结构体拷贝
 *   - 供 UI / 存储 / MQTT 等上层线程读取“最新一份”节点状态
 *
 * 数据流：
 *   处理线程 ──node_snapshot_update(env) / update_proc(proc)──► 快照
 *   上层/UI  ──node_snapshot_get──────────────────────────────► 副本
 */

#ifndef NODE_SNAPSHOT_H
#define NODE_SNAPSHOT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>
#include <pthread.h>
#include "modbus_env.h"

/* 处理后（滤波/量程换算）的物理量结果，供 UI / 存储 / MQTT 使用 */
typedef struct {
    float   temp_c;     /* 温度  ℃      */
    float   humid_pct;  /* 湿度  %RH    */
    float   smoke;      /* 烟雾  ADC原始 */
    float   voltage_v;  /* 电压  V      */
    float   current_a;  /* 电流  A      */
    uint16_t status;    /* 越限状态字（bit1~5，来自告警状态机） */
    int64_t ts_ms;      /* 处理时间戳（CLOCK_MONOTONIC ms）   */
} EnvProc;

/* 单个节点的最新采集快照，上层通过 node_snapshot_get 获取 */
typedef struct {
    uint8_t      addr;            /* 从站地址                */
    int          online;          /* 1=在线 0=离线           */
    int64_t      last_update_ms;  /* 最近一次更新时间(ms)     */
    EnvRegisters env;             /* ① 原始采集值           */
    EnvProc      proc;            /* ② 滤波/量程换算后的物理值 */
} NodeSnap;

#define NODE_SNAPSHOT_MAX 32      /* 最多支持的节点数（快照槽位上限） */

/* 初始化共享快照表：为每个节点建槽位+锁，登记地址。
 * addr 为节点地址数组，count <= NODE_SNAPSHOT_MAX。成功返回节点数，失败返回 -1。 */
int node_snapshot_init(const uint8_t *addr, int count);

/* 返回当前快照节点数（供上层遍历 0..count-1） */
int node_snapshot_count(void);

/* 写入原始采集值（处理线程调用）。env 为 NULL 时仅刷新 online/时间戳。
 * 锁内仅 memcpy（D9）。成功返回 0，越界返回 -1。 */
int node_snapshot_update(int idx, const EnvRegisters *env, int online);

/* 写入滤波/量程换算后的物理值 proc（处理线程调用）。
 * 锁内仅 memcpy（D9）。成功返回 0，越界返回 -1。 */
int node_snapshot_update_proc(int idx, const EnvProc *proc, int online);

/* 读取某个节点快照副本到 out（上层/UI 调用）。
 * 锁内仅 memcpy，返回后 out 即脱离共享区可自由使用。成功返回 0，越界/空指针返回 -1。 */
int node_snapshot_get(int idx, NodeSnap *out);

#endif /* NODE_SNAPSHOT_H */