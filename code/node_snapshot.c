/* =====================================================================
 * node_snapshot.c  -  共享数据快照模块
 * ---------------------------------------------------------------------
 * 作用：为“处理线程（写）”和“上层线程（读，如 UI/展示）”提供一份
 *       每节点最新的数据快照，并用每节点一把互斥锁保证并发安全。
 *
 * 设计要点（对应 high-level-design.md 决策 D9“共享快照，锁内仅 memcpy”）：
 *   - 每个节点一个槽位 NodeSnap，与一个互斥锁 locks[i] 一一对应；
 *   - 读写都在锁内只做“结构体拷贝”，解锁后再由调用方使用，
 *     从而把临界区做到最小，读者几乎感觉不到加锁开销；
 *   - get 取回的是快照副本，后续对副本的任何修改都不会影响共享区。
 *
 * 线程关系：
 *   采集线程 ─(邮箱)─► 处理线程 ──update──► 快照表 ──get──► 上层/UI
 * ===================================================================== */

#include "node_snapshot.h"
#include <string.h>
#include <time.h>

/* ---------------- 模块内部（静态）全局数据 ---------------- */

/* 快照主体数组：与 locks 数组按下标一一对应，snaps[i] 受 locks[i] 保护 */
static NodeSnap        snaps[NODE_SNAPSHOT_MAX];

/* 每节点一把互斥锁：保证同一时刻只有一个人能读写该节点的快照槽 */
static pthread_mutex_t locks[NODE_SNAPSHOT_MAX];

/* 当前已登记的快照节点数（由 node_snapshot_init 设置） */
static int             count = 0;

/* 取得单调时钟的毫秒数（CLOCK_MONOTONIC 不受 NTP 校时影响，适合测间隔） */
static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* 秒 * 1000 + 纳秒 / 1000000 = 毫秒 */
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* =====================================================================
 * node_snapshot_init
 * ---------------------------------------------------------------------
 * 初始化共享快照表：为每个节点建好一个带锁的快照槽位并登记其从站地址。
 *
 * 参数:
 *   addr : 节点从站地址数组
 *   cnt  : 节点个数（必须 1..NODE_SNAPSHOT_MAX）
 *
 * 返回:
 *   成功返回登记的快照节点数 count；
 *   参数非法（NULL / cnt<=0 / 超出上限）返回 -1。
 *
 * 说明：锁必须在这里显式 pthread_mutex_init，否则后续加锁会失败。
 * ===================================================================== */
int node_snapshot_init(const uint8_t *addr, int cnt)
{
    int i;

    /* ① 入参校验：地址表非空、节点数合法 */
    if (addr == NULL || cnt <= 0 || cnt > NODE_SNAPSHOT_MAX) {
        return -1;
    }

    count = 0;                 /* ② 清空计数器（支持重复初始化） */
    for (i = 0; i < cnt; i++) { /* ③ 逐个节点初始化 */
        pthread_mutex_init(&locks[i], NULL);   /* a. 创建/初始化该节点的锁 */
        memset(&snaps[i], 0, sizeof(snaps[i]));/* b. 快照槽清零（env/状态全为 0） */
        snaps[i].addr       = addr[i];          /* c. 登记该节点从站地址 */
        snaps[i].online     = 0;                /* d. 初始视为离线（等首包到来） */
        snaps[i].last_update_ms = 0;            /* e. 时间戳清零 */
        count++;                                /* f. 节点计数 +1 */
    }

    return count;              /* ④ 返回建好的快照节点总数 */
}

/* =====================================================================
 * node_snapshot_count
 * ---------------------------------------------------------------------
 * 返回当前快照节点数，供上层用 0..count-1 遍历读取。
 * ===================================================================== */
int node_snapshot_count(void)
{
    return count;
}

/* =====================================================================
 * node_snapshot_update
 * ---------------------------------------------------------------------
 * 写入（处理线程调用）：更新某个节点快照的数据与在线状态。
 *
 * 参数:
 *   idx    : 节点快照索引 [0, count)
 *   env    : 采集到的环境数据；为 NULL 表示“不改数据，只刷新状态/时间戳”
 *   online : 1=在线 0=离线
 *
 * 返回: 0 成功；-1 索引越界。
 *
 * 线程安全：全程持该节点锁，锁内仅做拷贝，之后立即解锁（D9）。
 * ===================================================================== */
int node_snapshot_update(int idx, const EnvRegisters *env, int online)
{
    /* 索引越界检查 */
    if (idx < 0 || idx >= count) {
        return -1;
    }

    pthread_mutex_lock(&locks[idx]);   /* 进入临界区：写该节点快照 */

    if (env != NULL) {
        snaps[idx].env = *env;         /* 拷贝采集数据（结构体整体拷贝） */
    }
    snaps[idx].online = online;        /* 更新在线标志 */
    snaps[idx].last_update_ms = mono_ms(); /* 记录本次更新时间 */

    pthread_mutex_unlock(&locks[idx]); /* 退出临界区 */

    return 0;
}

/* =====================================================================
 * node_snapshot_update_proc
 * ---------------------------------------------------------------------
 * 处理线程调用：把“滤波/量程换算后”的物理值 proc 写入该节点快照，
 * 供上层/UI 读取现实物理量（℃、V、A…）。
 *
 * 参数:
 *   idx    : 节点快照索引 [0, count)
 *   proc   : 处理后的物理量集合
 *   online : 1=在线 0=离线
 *
 * 返回: 0 成功；-1 索引越界。
 *
 * 线程安全：与 node_snapshot_update 同一套锁，锁内仅 memcpy（D9）。
 * ===================================================================== */
int node_snapshot_update_proc(int idx, const EnvProc *proc, int online)
{
    if (idx < 0 || idx >= count) {
        return -1;
    }

    pthread_mutex_lock(&locks[idx]);      /* 进入临界区 */

    if (proc != NULL) {
        snaps[idx].proc = *proc;          /* 拷贝滤波/量程后的物理值 */
    }
    snaps[idx].online         = online;
    snaps[idx].last_update_ms = mono_ms();

    pthread_mutex_unlock(&locks[idx]);    /* 退出临界区 */
    return 0;
}

/* =====================================================================
 * node_snapshot_get
 * ----------------------------------------------------------------------
 * 读取（上层/UI 线程调用）：取某个节点快照的副本到 out。
 *
 * 参数:
 *   idx : 节点快照索引 [0, count)
 *   out : 输出缓冲，调用方提供；返回后 out 即与共享区脱离、可自由使用
 *
 * 返回: 0 成功；-1 越界或 out 为空。
 *
 * 线程安全：锁内仅做结构体赋值拷贝，避免读者拿到“写一半”的数据。
 * ===================================================================== */
int node_snapshot_get(int idx, NodeSnap *out)
{
    /* 参数与索引校验 */
    if (out == NULL || idx < 0 || idx >= count) {
        return -1;
    }

    pthread_mutex_lock(&locks[idx]);   /* 进入临界区：读该节点快照 */
    *out = snaps[idx];                 /* 结构体整体拷贝到调用方缓冲 */
    pthread_mutex_unlock(&locks[idx]); /* 退出临界区 */

    return 0;
}