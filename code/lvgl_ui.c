/* =====================================================================
 * lvgl_ui.c - 展示域线程入口（框架占位）
 * ---------------------------------------------------------------------
 * 职责：初始化 LVGL、周期读取共享快照(node_snapshot_get)并渲染到屏幕。
 *       当前只搭出“周期轮询”的线程框架，LVGL 初始化/控件/刷屏留空。
 *
 * 说明：UI 数据来自共享快照（决策 D9）；用 usleep 周期轮询占位，
 *       避免空 while(1) 忙转烧 CPU。
 */
#include "lvgl_ui.h"
#include <stdint.h>
#include <unistd.h>

void *lvgl_ui_thread(void *arg)
{
    (void)arg;   /* arg 为 mbox_t*；UI 暂以共享快照为主入口，先不使用 */

    while (1) {
        /* TODO: 后续填充
         *   1) LVGL 初始化 + framebuffer(800×480) 接入
         *   2) 定时 node_snapshot_get 读各节点 EnvProc 物理量
         *   3) 刷新控件：实时值 / 告警标红 / 节点状态
         */
        usleep(100 * 1000);   /* 框架：周期轮询占位（100ms），防空转 */
    }
    return NULL;
}