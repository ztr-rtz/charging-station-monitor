/* =====================================================================
 * main.c - 充电站环境检测网关 · 主程序（单进程多线程）
 * ---------------------------------------------------------------------
 * 线程编排：
 *   采集线程(poll_thread)   : Modbus 主站 round-robin 轮询，产生 EnvMsg
 *   处理线程(proc_thread)   : 滤波/量程/告警，写共享快照，转发存储
 *   存储线程(storage_thread): SQLite 持久化（框架）
 *   展示线程(lvgl_ui_thread): LVGL 屏幕（框架）
 *   主线程                  : 上层展示（当前 printf 模拟，后续 LVGL）
 *
 * 数据流：
 *   采集 ──邮箱──► 处理 ──邮箱──► 存储
 *                        └──共享快照──► 主线程/LVGL
 *
 * 对应需求：FR-G03~G07(轮询/超时/离线)，FR-G04(2s/200ms)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

#include "uart.h"
#include "modbus_env.h"
#include "node_snapshot.h"
#include "mbox.h"
#include "data_process.h"
#include "storage.h"
#include "lvgl_ui.h"

/* ---------------- 采集参数（FR-G04 / FR-G05） ---------------- */
#define MAX_NODES      32        /* 最多节点数                      */
#define POLL_CYCLE_S    2       /* round-robin 轮询周期(秒) FR-G04 */
#define NODE_TIMEOUT_MS 200     /* 单节点应答超时(毫秒)     FR-G04 */
#define RX_GAP_MS       20       /* 帧间静默间隔，判定一帧结束     */
#define MAX_FAIL        3        /* 连续失败阈值，达此判离线 FR-G05 */
#define OFFLINE_PROBE_N 5       /* 离线节点每 N 轮探测 1 次 FR-G05 */

/* 邮箱线程名统一在 data_process.h 定义（TH_PROC_NAME/TH_STORE_NAME/TH_UI_NAME） */

/* ---------------- 节点注册表条目 ---------------- */
typedef struct {
    uint8_t addr;            /* 从站地址            */
    int     online;          /* 1=在线 0=离线       */
    int     fail_count;      /* 连续失败计数         */
    int     skip_rounds_left;/* 离线时剩余跳过轮数   */
} Node;

/* 默认从站地址表（按需求 NFR-M02 应改为从配置文件读取） */
static const uint8_t DEFAULT_NODES[] = { 1, 2, 3 };

/* ------- 供线程与信号处理共享的全局状态 ------- */
static volatile sig_atomic_t g_running = 1;   /* 退出标志（信号置 0） */
static int   g_fd;                            /* 串口文件描述符       */
static int   g_node_cnt;                      /* 节点数量             */
static int   g_rs485_kernel;                  /* 1=内核接管RS485方向  */
static Node  g_nodes[MAX_NODES];              /* 节点注册表           */
static mbox_t *g_mbox;                        /* 全局线程邮箱         */

/* 信号处理：收到 SIGINT/SIGTERM 置退出标志 */
static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* 单调时钟毫秒（CLOCK_MONOTONIC，不受 NTP 影响，用于测间隔） */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ============ 整帧收集 ============
 * 在 timeout_ms 内把响应字节收齐：帧间静默 >= RX_GAP_MS 视为一帧结束。
 * 返回收到的总字节数（可能 0=超时）。 */
static size_t read_frame(uint8_t *buf, size_t cap, int timeout_ms)
{
    size_t total = 0;
    long   deadline = now_ms() + timeout_ms;

    while (total < cap) {
        long remain = deadline - now_ms();
        if (remain <= 0) {
            break;
        }
        int step = (remain > RX_GAP_MS) ? RX_GAP_MS : (int)remain;
        ssize_t n = serial_read(g_fd, buf + total, cap - total, step);
        if (n < 0) {
            break;
        }
        if (n == 0) {
            if (total > 0) {
                break;      /* 已有数据且遇到静默：帧结束 */
            }
            continue;       /* 还没收到任何字节：继续等 */
        }
        total += (size_t)n;
    }
    return total;
}

/* ================= 采集线程（生产者） =================
 * Modbus 主站轮询各节点，成功则把 EnvMsg 经邮箱投递给处理线程。 */
static void *poll_thread(void *arg)
{
    (void)arg;
    uint8_t      req[8];      /* 请求帧缓冲 */
    uint8_t      rbuf[64];    /* 响应帧缓冲 */
    EnvRegisters env;         /* 解析出的环境数据 */

    while (g_running) {
        long round_start = now_ms();

        for (int i = 0; i < g_node_cnt && g_running; i++) {
            Node *nd = &g_nodes[i];

            /* 离线节点降频探测：跳过若干轮后再探测一次 */
            if (!nd->online && nd->skip_rounds_left > 0) {
                nd->skip_rounds_left--;
                continue;
            }

            /* ① 组装 0x03 读请求帧 */
            modbus_env_build_read(nd->addr, req, sizeof(req));

            /* ② 发送：手动方向则先切发送态 */
            if (!g_rs485_kernel) {
                serial_rs485_dir(g_fd, 1);
            }
            if (serial_write(g_fd, req, sizeof(req)) < 0) {
                if (!g_rs485_kernel) {
                    serial_rs485_dir(g_fd, 0);
                }
                continue;
            }
            tcdrain(g_fd);             /* 等帧完全发完再切方向 */
            tcflush(g_fd, TCIFLUSH);   /* 丢弃残留输入 */
            if (!g_rs485_kernel) {
                serial_rs485_dir(g_fd, 0);   /* 切回接收态 */
            }

            /* ③ 收响应帧（200ms 超时） */
            size_t rlen = read_frame(rbuf, sizeof(rbuf), NODE_TIMEOUT_MS);

            /* ④ 解析并投递 */
            if (rlen >= 5 &&
                modbus_env_parse_read(nd->addr, rbuf, rlen, &env)) {
                if (!nd->online) {
                    fprintf(stderr, "[节点 %d] 恢复在线\n", nd->addr);
                }
                nd->online = 1;
                nd->fail_count = 0;

                /* 组 EnvMsg 并经邮箱发给处理线程 */
                EnvMsg msg;
                msg.idx = (uint8_t)i;
                msg.env = env;
                mbox_send(g_mbox, TH_PROC_NAME, MSG_ENV_DATA,
                          &msg, sizeof(msg));
            } else {
                /* 失败：fail_count++，连续 3 次判离线 */
                nd->fail_count++;
                if (nd->online && nd->fail_count >= MAX_FAIL) {
                    nd->online = 0;
                    nd->skip_rounds_left = OFFLINE_PROBE_N - 1;
                    fprintf(stderr, "[节点 %d] 判离线，每 %d 轮探测一次\n",
                            nd->addr, OFFLINE_PROBE_N);
                    node_snapshot_update(i, NULL, 0);   /* 快照标离线 */
                }
            }
        }

        /* 补睡：保持整轮周期 = POLL_CYCLE_S */
        long elapsed = now_ms() - round_start;
        long sleep_ms = POLL_CYCLE_S * 1000L - elapsed;
        if (sleep_ms > 0) {
            usleep((useconds_t)sleep_ms * 1000);
        }
    }
    return NULL;
}

/* ================= 上层展示（消费共享快照） =================
 * 屏幕(LVGL)以“滤波/量程换算后的物理量 proc”为主显示，
 * 原始定标值(env)仅作本地参考。当前先用 printf 模拟 UI 展示。 */
static void print_node(const NodeSnap *s)
{
    const EnvProc *p = &s->proc;
    printf("节点%u[%s | t+%" PRId64 "ms] "
           "温度=%.1f℃ 湿度=%.1f%% 烟雾=%.0f "
           "电压=%.2fV 电流=%.3fA 告警=0x%04X (原始: 温度%d/湿度%u/电压%u/电流%u)\n",
           (unsigned)s->addr,
           s->online ? "在线" : "离线", (int64_t)s->last_update_ms,
           p->temp_c, p->humid_pct, p->smoke,
           p->voltage_v, p->current_a,
           (unsigned)p->status,
           (int)s->env.temperature, (unsigned)s->env.humidity,
           (unsigned)s->env.voltage, (unsigned)s->env.current);
}

/* ================= 主程序 ================= */
int main(int argc, char **argv)
{
    const char *dev_path = "/dev/ttymxc2";   /* 默认串口（可被 argv 覆盖） */
    int baud = 9600;                          /* 默认波特率               */

    /* 命令行：argv[1]=串口  argv[2]=波特率  argv[3..]=节点地址 */
    if (argc > 1) {
        dev_path = argv[1];
    }
    if (argc > 2) {
        baud = atoi(argv[2]);
    }
    g_node_cnt = (argc > 3) ? (argc - 3)
                            : (int)(sizeof(DEFAULT_NODES) / sizeof(DEFAULT_NODES[0]));
    if (g_node_cnt > MAX_NODES) {
        g_node_cnt = MAX_NODES;
    }

    /* 1. 打开串口 + 配置 RS485 方向 */
    g_fd = serial_open(dev_path, baud, NODE_TIMEOUT_MS);
    if (g_fd < 0) {
        fprintf(stderr, "打开串口 %s 失败\n", dev_path);
        return -1;
    }
    if (serial_rs485_kernel_enable(g_fd) != 0) {
        g_rs485_kernel = 0;              /* 内核不支持 → 手动 RTS */
        serial_rs485_dir(g_fd, 0);       /* 默认置接收态 */
    } else {
        g_rs485_kernel = 1;
    }

    /* 2. 节点注册表 + 共享快照初始化 */
    uint8_t addrs[MAX_NODES];
    for (int i = 0; i < g_node_cnt; i++) {
        g_nodes[i].addr = (argc > 3) ? (uint8_t)atoi(argv[3 + i])
                                     : DEFAULT_NODES[i];
        g_nodes[i].online = 1;
        g_nodes[i].fail_count = 0;
        g_nodes[i].skip_rounds_left = 0;
        addrs[i] = g_nodes[i].addr;
    }
    if (node_snapshot_init(addrs, g_node_cnt) != g_node_cnt) {
        fprintf(stderr, "快照初始化失败\n");
        return -1;
    }

    /* 3. 创建线程邮箱并注册各功能线程 */
    g_mbox = mbox_create();
    if (g_mbox == NULL) {
        fprintf(stderr, "创建线程邮箱失败\n");
        return -1;
    }
    if (mbox_register(g_mbox, TH_PROC_NAME, proc_thread, g_mbox) != 0) {
        fprintf(stderr, "注册处理线程失败\n");
        return -1;
    }
    /* 存储线程（SQLite）——内容后续填充 */
    if (mbox_register(g_mbox, TH_STORE_NAME, storage_thread, g_mbox) != 0) {
        fprintf(stderr, "注册存储线程失败\n");
        return -1;
    }
    /* 展示线程（LVGL）——内容后续填充 */
    if (mbox_register(g_mbox, TH_UI_NAME, lvgl_ui_thread, g_mbox) != 0) {
        fprintf(stderr, "注册展示线程失败\n");
        return -1;
    }

    /* 4. 创建采集线程 + 注册信号处理 */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    pthread_t ctid;
    if (pthread_create(&ctid, NULL, poll_thread, NULL) != 0) {
        fprintf(stderr, "创建采集线程失败\n");
        return -1;
    }

    printf("启动：%s @ %d, 节点数 %d（Ctrl+C 退出）\n", dev_path, baud, g_node_cnt);

    /* 5. 主线程作为上层：定时读取共享快照并展示 */
    while (g_running) {
        for (int i = 0; i < g_node_cnt; i++) {
            NodeSnap s;
            if (node_snapshot_get(i, &s) == 0 && s.last_update_ms > 0) {
                print_node(&s);
            }
        }
        sleep(POLL_CYCLE_S);
    }

    /* 6. 退出清理：回收采集线程 → 销毁邮箱(含所有功能线程) → 关串口 */
    pthread_join(ctid, NULL);
    mbox_destroy(g_mbox);
    serial_close(g_fd);
    printf("\n已退出。\n");
    return 0;
}