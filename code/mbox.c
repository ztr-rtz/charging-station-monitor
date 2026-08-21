/* =====================================================================
 * mbox.c - 线程邮箱的实现
 * ---------------------------------------------------------------------
 * 数据结构：
 *   - struct mbox：全局邮箱，含一把保护“注册表”的锁 rmutex
 *                  + 一串 MboxNode（每注册线程一个）
 *   - MboxNode：每线程的邮箱节点，含独立定长环形队列、独立 qmutex、条件变量 qcond
 *
 * 关键设计：
 *   - 线程名全局唯一（mbox_register 时检查重名）
 *   - send 按“名字”找到对方节点，锁 qmutex 后往其环形队列入队（非阻塞）
 *   - recv 由当前线程查自己的节点，空队列则 pthread_cond_wait 阻塞（不空转）
 *   - 环形队列：head=出队下标, tail=入队下标, count=当前消息数
 *
 * 参考 mailbox 原设计，但修复了：忙等自旋、定长字符串、全局单锁等问题。
 * ===================================================================== */

#include "mbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---------------- 每线程邮箱节点 ---------------- */
typedef struct {
    char       name[MBOX_NAME_LEN];  /* 线程名（注册时写入，全局唯一） */
    pthread_t  tid;                  /* 该线程的线程号，用于 recv 定位自己 */
    mbox_entry entry;                /* 线程入口函数（仅记录，线程由 pthread_create 启动） */
    void      *arg;                  /* 透传给入口函数的参数 */
    int        used;                 /* 1=该节点已被注册占用 */

    /* 定长环形队列：3 个游标实现 FIFO，slots 复用避免每次 malloc */
    MboxMsg         slots[MBOX_MAX_MSG];
    unsigned        head;   /* 出队下标（队首） */
    unsigned        tail;   /* 入队下标（队尾） */
    unsigned        count;  /* 当前队列中的消息条数 */
    pthread_mutex_t qmutex; /* 本队列独立锁（只保护这一个线程的队列） */
    pthread_cond_t  qcond;  /* 条件变量：recv 阻塞于此时，由 send 唤醒 */
} MboxNode;

/* ---------------- 邮箱系统本体 ---------------- */
struct mbox {
    pthread_mutex_t rmutex;                   /* 保护“注册表”本身的锁 */
    MboxNode        nodes[MBOX_MAX_THREADS]; /* 各线程邮箱节点表 */
    int             created;                  /* 构造标记（预留） */
};

/* 按线程名查找节点（调用前提：已持 / 或准备持 rmutex） */
static MboxNode *mbox_find_node(mbox_t *m, const char *name)
{
    for (int i = 0; i < MBOX_MAX_THREADS; i++) {
        if (m->nodes[i].used &&
            strcmp(m->nodes[i].name, name) == 0) {
            return &m->nodes[i];
        }
    }
    return NULL;
}

/* 按“当前线程 id”查找节点（供 recv 使用；当前线程必须已注册） */
static MboxNode *mbox_self_node(mbox_t *m)
{
    pthread_t self = pthread_self();
    for (int i = 0; i < MBOX_MAX_THREADS; i++) {
        if (m->nodes[i].used &&
            pthread_equal(m->nodes[i].tid, self)) {
            return &m->nodes[i];
        }
    }
    return NULL;
}

/* ---------------- 创建邮箱系统 ---------------- */
mbox_t *mbox_create(void)
{
    mbox_t *m = calloc(1, sizeof(mbox_t));   /* calloc：nodes 数组自动清零 */
    if (m == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&m->rmutex, NULL) != 0) {  /* 初始化注册表锁 */
        free(m);
        return NULL;
    }
    m->created = 1;
    return m;
}

/* ---------------- 注册线程 ---------------- */
int mbox_register(mbox_t *m, const char *name, mbox_entry entry, void *arg)
{
    if (m == NULL || name == NULL || entry == NULL) {
        return -1;
    }

    pthread_mutex_lock(&m->rmutex);                     /* ① 锁注册表 */
    if (mbox_find_node(m, name) != NULL) {              /* ② 拒绝重名 */
        pthread_mutex_unlock(&m->rmutex);
        fprintf(stderr, "mbox: 线程名已存在 |%s|\n", name);
        return -1;
    }

    MboxNode *slot = NULL;
    for (int i = 0; i < MBOX_MAX_THREADS; i++) {        /* ③ 找一个空槽 */
        if (!m->nodes[i].used) {
            slot = &m->nodes[i];
            break;
        }
    }
    if (slot == NULL) {                                 /* 槽已满 */
        pthread_mutex_unlock(&m->rmutex);
        fprintf(stderr, "mbox: 线程槽已满\n");
        return -1;
    }

    memset(slot, 0, sizeof(*slot));                     /* ④ 填节点字段 */
    strncpy(slot->name, name, MBOX_NAME_LEN - 1);
    slot->entry = entry;
    slot->arg   = arg;
    slot->used  = 1;
    pthread_mutex_init(&slot->qmutex, NULL);            /* ⑤ 初始化队列锁 */
    pthread_cond_init(&slot->qcond, NULL);              /*    初始化条件变量 */

    if (pthread_create(&slot->tid, NULL, entry, arg) != 0) {  /* ⑥ 启动线程 */
        slot->used = 0;
        pthread_mutex_unlock(&m->rmutex);
        fprintf(stderr, "mbox: 线程创建失败 |%s|\n", name);
        return -1;
    }

    pthread_mutex_unlock(&m->rmutex);                   /* ⑦ 解锁返回 */
    return 0;
}

/* ---------------- 发送（非阻塞） ---------------- */
int mbox_send(mbox_t *m, const char *to, uint32_t type,
              const void *data, uint16_t len)
{
    if (m == NULL || to == NULL ||
        (len > 0 && data == NULL) || len > MBOX_DATA_LEN) {
        return -1;
    }

    pthread_mutex_lock(&m->rmutex);                     /* ① 查收件人（持注册表锁） */
    MboxNode *node = mbox_find_node(m, to);
    if (node == NULL) {
        pthread_mutex_unlock(&m->rmutex);
        fprintf(stderr, "mbox: 找不到收件人 |%s|\n", to);
        return -ENOENT;
    }

    pthread_mutex_lock(&node->qmutex);                  /* ② 锁目标队列 */
    pthread_mutex_unlock(&m->rmutex);                   /*    注册表锁可释放 */

    if (node->count >= MBOX_MAX_MSG) {                  /* ③ 队列满 → 非阻塞失败 */
        pthread_mutex_unlock(&node->qmutex);
        return -EAGAIN;
    }

    MboxMsg *msg = &node->slots[node->tail];            /* ④ 写入队尾槽 */
    msg->type = type;
    msg->len  = len;
    if (len > 0) {
        memcpy(msg->data, data, len);
    }
    node->tail = (node->tail + 1) % MBOX_MAX_MSG;       /* ⑤ 队尾后移(环形) */
    node->count++;

    pthread_cond_signal(&node->qcond);                   /* ⑥ 唤醒可能阻塞的收件人 */
    pthread_mutex_unlock(&node->qmutex);
    return 0;
}

/* ---------------- 接收（阻塞） ---------------- */
int mbox_recv(mbox_t *m, uint32_t *type, void *buf, uint16_t *len)
{
    if (m == NULL || type == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    pthread_mutex_lock(&m->rmutex);                     /* ① 定位当前线程节点 */
    MboxNode *node = mbox_self_node(m);
    pthread_mutex_unlock(&m->rmutex);
    if (node == NULL) {
        fprintf(stderr, "mbox: 当前线程未注册\n");
        return -1;
    }

    pthread_mutex_lock(&node->qmutex);                   /* ② 锁队列 */
    while (node->count == 0) {                          /* ③ 队列空则阻塞等待 */
        pthread_cond_wait(&node->qcond, &node->qmutex); /*    wait 会原子释放锁 */
    }

    MboxMsg *msg = &node->slots[node->head];            /* ④ 取队首消息 */
    *type = msg->type;
    uint16_t n = (msg->len > MBOX_DATA_LEN) ? MBOX_DATA_LEN : msg->len;
    if (n > 0) {
        memcpy(buf, msg->data, n);
    }
    *len = n;

    node->head = (node->head + 1) % MBOX_MAX_MSG;       /* ⑤ 队首后移 */
    node->count--;

    pthread_mutex_unlock(&node->qmutex);
    return 0;
}

/* ---------------- 非阻塞接收 ---------------- */
int mbox_recv_nowait(mbox_t *m, uint32_t *type, void *buf, uint16_t *len)
{
    if (m == NULL || type == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    pthread_mutex_lock(&m->rmutex);
    MboxNode *node = mbox_self_node(m);
    pthread_mutex_unlock(&m->rmutex);
    if (node == NULL) {
        return -1;
    }

    pthread_mutex_lock(&node->qmutex);
    if (node->count == 0) {
        pthread_mutex_unlock(&node->qmutex);
        return -1;   /* 队列空，立即返回（不阻塞） */
    }

    MboxMsg *msg = &node->slots[node->head];
    *type = msg->type;
    uint16_t n = (msg->len > MBOX_DATA_LEN) ? MBOX_DATA_LEN : msg->len;
    if (n > 0) {
        memcpy(buf, msg->data, n);
    }
    *len = n;

    node->head = (node->head + 1) % MBOX_MAX_MSG;
    node->count--;

    pthread_mutex_unlock(&node->qmutex);
    return 0;
}

/* ---------------- 注销单个线程 ---------------- */
int mbox_unregister(mbox_t *m, const char *name)
{
    if (m == NULL || name == NULL) {
        return -1;
    }
    pthread_mutex_lock(&m->rmutex);
    MboxNode *node = mbox_find_node(m, name);
    if (node == NULL) {
        pthread_mutex_unlock(&m->rmutex);
        return -ENOENT;
    }
    node->used = 0;
    pthread_cancel(node->tid);                /* 取消线程 */
    pthread_join(node->tid, NULL);           /* 回收线程结束 */
    pthread_mutex_destroy(&node->qmutex);    /* 销毁队列锁 */
    pthread_cond_destroy(&node->qcond);      /* 销毁条件变量 */
    pthread_mutex_unlock(&m->rmutex);
    return 0;
}

/* ---------------- 销毁整个邮箱系统 ---------------- */
void mbox_destroy(mbox_t *m)
{
    if (m == NULL) {
        return;
    }
    pthread_mutex_lock(&m->rmutex);
    for (int i = 0; i < MBOX_MAX_THREADS; i++) {
        MboxNode *node = &m->nodes[i];
        if (node->used) {                    /* 逐线程取消 + 回收资源 */
            node->used = 0;
            pthread_cancel(node->tid);
            pthread_join(node->tid, NULL);
            pthread_mutex_destroy(&node->qmutex);
            pthread_cond_destroy(&node->qcond);
        }
    }
    pthread_mutex_unlock(&m->rmutex);
    pthread_mutex_destroy(&m->rmutex);      /* 释放注册表锁 */
    free(m);
}