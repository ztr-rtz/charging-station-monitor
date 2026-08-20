/* =====================================================================
 * mbox.h - 线程邮箱模块（按“线程名”寻址的阻塞式消息队列）
 * ---------------------------------------------------------------------
 * 用途：在网关多线程架构中，作为各功能域(采集/处理/存储/UI)之间的解耦通道，
 *       对应 high-level-design.md 的“队列”机制（决策 D2 生产者-消费者）。
 *
 * 设计要点：
 *   - 每个注册线程分配一个 独立定长环形队列 + 独立互斥锁 + 条件变量
 *   - 消息体为定长负载 MBOX_DATA_LEN，可承载结构化二进制（如 EnvRegisters）
 *   - send 非阻塞：队列满返回 -EAGAIN
 *   - recv 阻塞：队列空则睡在条件变量上（不占用 CPU）
 *
 * 使用模式：
 *   mbox_t *m = mbox_create();
 *   mbox_register(m, "proc", proc_thread, m);   // 建线程 + 分配队列
 *   mbox_send(m, "proc", MSG_TYPE, &payload, len); // 别的线程发给它
 *   mbox_recv(m, &type, buf, &len);              // proc 线程自己阻塞收
 * ===================================================================== */

#ifndef MBOX_H
#define MBOX_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

/* ---------------- 邮箱系统常量 ---------------- */
#define MBOX_NAME_LEN     64      /* 线程名最大长度（字节）      */
#define MBOX_MAX_THREADS  16       /* 最多可注册的线程数         */
#define MBOX_MAX_MSG      64    /* 每线程队列的定长槽数       */
#define MBOX_DATA_LEN     256       /* 每条消息负载的最大字节数   */

/* 定长消息：头(type/len) + 定长负载(data) */
typedef struct {
    uint32_t type;                /* 消息类型（见 data_process.h 中枚举） */
    uint16_t len;                 /* 实际有效负载长度，<= MBOX_DATA_LEN   */
    uint8_t  data[MBOX_DATA_LEN]; /* 定长负载（二进制, 直存可承载结构体）  */
} MboxMsg;

/* 线程入口函数类型：返回 void*，接受一个 void* 参数 */
typedef void *(*mbox_entry)(void *arg);

/* 邮箱句柄类型：不透明，具体实现见 mbox.c */
typedef struct mbox mbox_t;

/* 创建一个邮箱系统；失败返回 NULL */
mbox_t *mbox_create(void);

/* 注册一个线程到邮箱：内部创建线程，并为其分配一个独立队列。
 * entry 为该线程的入口函数，arg 原样透传给 entry。成功返回 0，失败返回 -1。 */
int mbox_register(mbox_t *m, const char *name, mbox_entry entry, void *arg);

/* 发送消息到指定“线程名”的队列。非阻塞：队列满返回 -EAGAIN。
 * data 为空且 len==0 表示仅传类型而无负载。 */
int mbox_send(mbox_t *m, const char *to, uint32_t type,
              const void *data, uint16_t len);

/* 从当前线程自己的队列接收一条消息（阻塞）。
 * 成功返回 0，把负载复制到 buf 并把类型/长度回填到 type/len。 */
int mbox_recv(mbox_t *m, uint32_t *type, void *buf, uint16_t *len);

/* 注销一个线程并回收其队列/锁/条件变量 */
int mbox_unregister(mbox_t *m, const char *name);

/* 销毁整个邮箱系统：取消并回收所有已注册线程 */
void mbox_destroy(mbox_t *m);

#endif /* MBOX_H */