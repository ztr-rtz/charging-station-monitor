/* =====================================================================
 * data_process.h - 处理域：消息类型 / 消息体 / 处理线程接口
 * ---------------------------------------------------------------------
 * 处理域位于采集线程与存储/展示线程之间，负责：
 *   接收采集消息(MSG_ENV_DATA) → 滤波/量程/告警 → 写共享快照
 *   并把(原始+滤波)双值转发给存储线程(MSG_STORE_NODE)
 * 邮箱线程名也集中在此定义，供各模块共用（避免重复宏）。
 * ===================================================================== */

#ifndef DATA_PROCESS_H
#define DATA_PROCESS_H

#include <stdint.h>
#include "mbox.h"
#include "modbus_env.h"
#include "mqtt_report.h"

/* ---- 消息类型定义 ---- */
#define MSG_ENV_DATA    1   /* 采集 → 处理：一节点环境数据      */
#define MSG_STORE_NODE  2   /* 处理 → 存储：一节点(原始+滤波)入库 */
#define MSG_MQTT_REPORT 3   /* 处理 → MQTT：通知上报（触发全节点上报） */

/* 采集→处理 的消息负载（装入 MboxMsg.data，定长） */
typedef struct {
    uint8_t      idx;        /* 节点快照索引（对应 node_snapshot 索引） */
    EnvRegisters env;        /* 采集到的环境值（原始定标值）             */
} EnvMsg;

/* ---------- 邮箱线程名（跨模块共用） ---------- */
#define TH_PROC_NAME   "proc"
#define TH_STORE_NAME  "store"
#define TH_UI_NAME     "ui"
#define TH_MQTT_NAME   "mqtt"

/* 处理线程入口：配合 mbox_register(m, "proc", proc_thread, m) 使用。
 * 阻塞 recv → handle_env_msg(滤波/量程/告警) → 写快照 → 转发存储。 */
void *proc_thread(void *arg);

/* 环境数据处理（环形缓冲→差异化滤波→量程换算→告警状态机→写快照）：
 *   温度/湿度/电压 → 滑动平均；烟雾/电流 → 中值滤波。 */
void handle_env_msg(const EnvMsg *m);

#endif /* DATA_PROCESS_H */