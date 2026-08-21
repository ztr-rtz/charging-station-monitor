/* =====================================================================
 * data_process.c  -  处理域（滤波 / 量程换算 / 告警 / 链路转发）
 * ---------------------------------------------------------------------
 * 作用：搭建“采集 → 处理 → 存储/展示”链路的中游环节，
 *       把采集到的原始值加工成物理量并提供给后续域。
 *
 * 对应关键需求 / 决策：
 *   - FR-P01 环形缓冲30点   FR-P02 滑动平均   FR-P03 中值滤波
 *   - FR-P05 三态告警       FR-P06 迟滞       FR-P07 动作分级
 *   - D9 处理→UI 经共享快照(锁内memcpy)
 *
 * 数据流（本文件完成的环节）：
 *   采集线程 ─(MSG_ENV_DATA)─► proc_thread ──► handle_env_msg
 *       ①原始值入环形缓冲 → ②差异化滤波 → ③量程换算 → ④告警状态机 →
 *       ⑤写共享快照(proc) → ⑥转发给存储线程(MSG_STORE_NODE)
 * ===================================================================== */

#include "data_process.h"
#include "node_snapshot.h"
#include "mbox.h"
#include "filter.h"
#include "alert.h"
#include "storage.h"   /* 引用 StoreMsg 与 TH_STORE_NAME */
#include <stdint.h>
#include <time.h>
#include <stdio.h>

/* ---------------- 每节点每指标 环形缓冲（FR-P01，30点）---------------- */
/* 分指标各存一份历史（数组下标 = 节点快照索引） */
typedef struct {
    RingBuf temp;    /* 温度 ×10   (缓变, 用滑动平均) */
    RingBuf humid;   /* 湿度 ×10   (缓变, 用滑动平均) */
    RingBuf smoke;   /* 烟雾 ADC原始 (尖脉冲, 用中值)   */
    RingBuf volt;    /* 电压 ×100  (缓变, 用滑动平均) */
    RingBuf curr;    /* 电流 ×1000 (尖脉冲, 用中值)   */
} NodeBuffs;
static NodeBuffs g_bufs[NODE_SNAPSHOT_MAX];   /* 数组下标 = 节点快照索引 */

/* ---------------- 每节点每指标告警状态 ---------------- */
/* 三态(正常/预警/告警)，供下一次状态机推进输入 */
static AlmState g_alm[NODE_SNAPSHOT_MAX][5];  /* [温度][湿度][烟雾][电压][电流] */

/* ---------------- 告警阈值表 ----------------
 * 每组 预警(warn) / 告警(alarm) / 迟滞退出(exit) 三个阈值（迟滞=防临界抖动）。
 * 注：FR-P08 要求这些值最终来自配置文件 app.conf，当前为默认参考值。 */
static const AlmTh TH_TEMP = { 45.0f, 55.0f, 52.0f };   /* ℃    */
static const AlmTh TH_HUM  = { 80.0f, 90.0f, 85.0f };   /* %RH  */
static const AlmTh TH_SMK  = { 1800,  2800,  2400  };   /* ADC  */
static const AlmTh TH_VOLT = { 5.0f, 10.0f, 7.0f  };    /* %偏差（相对额定） */
static const AlmTh TH_CURR = { 25.0f, 32.0f, 28.0f };   /* A    */
static const float  V_REF  = 230.0f;                    /* 电压额定参考 (V) */

/* 单调时钟毫秒（CLOCK_MONOTONIC，不受 NTP 校时影响，用于测间隔） */
static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* =====================================================================
 * handle_env_msg - 处理域主流程：滤波→量程→告警→写快照
 * ---------------------------------------------------------------------
 * 步骤：
 *   1) 原始值入对应指标的环形缓冲
 *   2) 差异化滤波 + 量程换算：
 *      温度/湿度/电压 → 滑动平均；烟雾/电流 → 中值；
 *      同理按定标系数转为物理量(℃/%RH/V/A)
 *   3) 告警状态机：电压先算相对额定偏差百分比(取绝对值)再过状态机；
 *      其余指标直接过状态机。≥预警 即置对应状态字 bit1~5。
 *   4) 把结果(EnvProc)写入共享快照，供 UI/存储/MQTT 读取
 * ===================================================================== */
void handle_env_msg(const EnvMsg *m)
{
    int idx = m->idx;
    if (idx < 0 || idx >= NODE_SNAPSHOT_MAX) {
        return;
    }

    /* 1. 原始值入环形缓冲（每节点每指标独立） */
    ring_push(&g_bufs[idx].temp,  m->env.temperature);
    ring_push(&g_bufs[idx].humid, m->env.humidity);
    ring_push(&g_bufs[idx].smoke, m->env.smoke);
    ring_push(&g_bufs[idx].volt,  (int32_t)m->env.voltage);
    ring_push(&g_bufs[idx].curr,  (int32_t)m->env.current);

    /* 2./3. 差异化滤波 + 量程换算 → 物理量 */
    EnvProc p;
    p.temp_c    = (float)filt_sma(&g_bufs[idx].temp, FILTER_WIN)   / 10.0f;
    p.humid_pct = (float)filt_sma(&g_bufs[idx].humid, FILTER_WIN)  / 10.0f;
    p.smoke     = (float)filt_median(&g_bufs[idx].smoke, FILTER_WIN);
    p.voltage_v = (float)filt_sma(&g_bufs[idx].volt, FILTER_WIN)   / 100.0f;
    p.current_a = (float)filt_median(&g_bufs[idx].curr, FILTER_WIN)/ 1000.0f;

    /* 4. 电压偏差百分比（上下对称取绝对值），供电压告警判定 */
    int volt_dev_pct = (int)( (p.voltage_v - V_REF) / V_REF * 100.0f );
    if (volt_dev_pct < 0) volt_dev_pct = -volt_dev_pct;

    /* 4(续). 各指标三态状态机推进 */
    AlmState *s = g_alm[idx];
    s[0] = alm_step(s[0], p.temp_c,    &TH_TEMP);
    s[1] = alm_step(s[1], p.humid_pct, &TH_HUM);
    s[2] = alm_step(s[2], p.smoke,     &TH_SMK);
    s[3] = alm_step(s[3], (float)volt_dev_pct, &TH_VOLT);
    s[4] = alm_step(s[4], p.current_a, &TH_CURR);

    /* 5. 合并成状态字 bit1~5（对应 MB_ENV_ST_* 位；bit0 故障位暂未查）*/
    uint16_t st = 0;
    if (s[0] >= ALM_WARN) st |= MB_ENV_ST_TEMP_ALM;
    if (s[1] >= ALM_WARN) st |= MB_ENV_ST_HUMID_ALM;
    if (s[2] >= ALM_WARN) st |= MB_ENV_ST_SMOKE_ALM;
    if (s[3] >= ALM_WARN) st |= MB_ENV_ST_VOLT_ALM;
    if (s[4] >= ALM_WARN) st |= MB_ENV_ST_CURR_ALM;
    p.status = st;
    p.ts_ms  = mono_ms();

    /* 6. 把处理结果写入共享快照（加锁 memcpy，D9） */
    node_snapshot_update_proc(idx, &p, 1);
}

/* 处理线程持有的邮箱句柄（供转发给 store 使用） */
static mbox_t *g_proc_mbox;

/* =====================================================================
 * proc_send_to_store
 * ---------------------------------------------------------------------
 * 处理线程 → 存储线程：把“原始值 + 滤波后物理量”双份打包成 StoreMsg，
 * 经邮箱 “store” 投递，接通持久化链路（SQLite 落库在后续填充）。 */
static void proc_send_to_store(int idx, const EnvRegisters *raw, const EnvProc *proc)
{
    StoreMsg sm;
    sm.idx    = (uint8_t)idx;
    sm.online = 1;
    sm.raw    = *raw;
    sm.proc   = *proc;
    mbox_send(g_proc_mbox, TH_STORE_NAME, MSG_STORE_NODE, &sm, sizeof(sm));
}

/* 处理线程 → MQTT 线程：通知有新数据，触发全节点上报 */
static void proc_send_to_mqtt(int idx)
{
    uint8_t dummy = (uint8_t)idx;
    mbox_send(g_proc_mbox, TH_MQTT_NAME, MSG_MQTT_REPORT, &dummy, 1);
}

/* =====================================================================
 * proc_thread - 处理线程入口
 * ---------------------------------------------------------------------
 * 阻塞收邮箱 → ① 原始值写共享快照 → ② handle_env_msg(滤波/量程/告警/写proc)
 *            → ③ 从共享快照取回处理结果 → ④ 转发给存储线程
 *
 * 配合 mbox_register(m, "proc", proc_thread, m) 使用。 */
void *proc_thread(void *arg)
{
    mbox_t    *m = (mbox_t *)arg;
    uint32_t   type;
    uint16_t   len;
    EnvMsg     msg;
    EnvProc    proc;

    g_proc_mbox = m;   /* 记住邮箱，供后续转发给 store */

    while (1) {
        if (mbox_recv(m, &type, &msg, &len) == 0 && type == MSG_ENV_DATA) {
            if (len == sizeof(EnvMsg)) {
                node_snapshot_update(msg.idx, &msg.env, 1);  /* ① 原始值 */
                handle_env_msg(&msg);                         /* ② 滤波→告警→proc */

                /* ③④ 取回刚写的 proc，转发给存储线程持久化 */
                NodeSnap s;
                if (node_snapshot_get(msg.idx, &s) == 0) {
                    proc = s.proc;
                    proc_send_to_store(msg.idx, &msg.env, &proc);
                    proc_send_to_mqtt(msg.idx);
                }
            }
        }
    }
    return NULL;
}