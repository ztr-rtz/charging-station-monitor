/* =====================================================================
 * storage.h - 存储域（SQLite）：持久化历史数据 ［框架线程］
 * ---------------------------------------------------------------------
 * 需求 FR-S01~S06，三张表：
 *   - node_data  历史数据：存“原始值(EnvRegisters) + 滤波换算后物理量(EnvProc)”双份
 *   - cache_queue 断网补传缓存（带时间戳，恢复后按序补传）
 *   - alarm_log   告警日志（含阈值快照）
 *
 * 线程方式：存储作为“store”邮箱收件方，由处理线程经 MSG_STORE_NODE 投递。
 * 目前为框架线程，SQLite 连接 / 建表 / 落库逻辑留空（见 storage.c TODO）。
 * ===================================================================== */

#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include "mbox.h"
#include "node_snapshot.h"

/* 处理线程 → 存储线程 的负载（存“原始值 + 滤波值”双份） */
typedef struct {
    uint8_t      idx;       /* 节点快照索引            */
    int          online;    /* 在线状态(1/0)           */
    EnvRegisters raw;       /* ① 原始采集值           */
    EnvProc      proc;      /* ② 滤波/量程换算后物理量  */
} StoreMsg;

/* 存储线程入口：配合 mbox_register(m, "store", storage_thread, m) 使用。
 * 阻塞 recv → 落 SQLite（框架：当前留空，后续填充）。 */
void *storage_thread(void *arg);

#endif /* STORAGE_H */