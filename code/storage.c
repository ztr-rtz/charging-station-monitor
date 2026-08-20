/* =====================================================================
 * storage.c - 存储域线程入口（框架占位）
 * ---------------------------------------------------------------------
 * 职责：作为"store"邮箱收件方，阻塞接收处理线程投递的 StoreMsg，
 *       把"原始值 + 滤波物理量"双份持久化到 SQLite（后续填充）。
 *
 * 当前状态：仅搭好线程收消息的框架，SQLite 连接 / WAL 模式 / 建表 /
 *           双值入库(node_data) / 缓存(cache_queue) / 告警日志(alarm_log)
 *           均留空待后续实现（对应 FR-S01~S06）。
 *
 * 消息来源：处理线程(proc) → mbox_send("store", MSG_STORE_NODE, ...)
 */
#include "storage.h"
#include <stdint.h>

void *storage_thread(void *arg)
{
    mbox_t    *m = (mbox_t *)arg;
    uint32_t   type;
    uint16_t   len;
    StoreMsg   msg;

    while (1) {
        if (mbox_recv(m, &type, &msg, &len) == 0) {
            /* TODO: 后续填充
             *   - sqlite3_open(db) + PRAGMA journal_mode=WAL
             *   - 建表 node_data / cache_queue / alarm_log
             *   - 按 type 落库：MSG_STORE_NODE → 存 msg.raw(原始) + msg.proc(滤波后)
             *   - 断网时转缓存、告警时写 alarm_log
             */
            (void)type;
        }
    }
    return NULL;
}