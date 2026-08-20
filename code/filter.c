/* =====================================================================
 * filter.c - 滤波模块实现（环形缓冲 + 滑动平均 / 中值滤波）
 * ---------------------------------------------------------------------
 * 实现细节：
 *   - ring_push 写 head 位置后环形后移；count 到上限后覆盖最旧值
 *   - filt_sma 只对最近 min(win,count) 个点求平均（用 long 避免溢出）
 *   - filt_median 把最近几个点拷贝排序后取中位（窗口小，插入排序足够）
 */

#include "filter.h"

/* 把新值压入环形缓冲：写入当前 head，head 后移（模 HIST_LEN），
 * count 未满则加 1（满后保持 count=HIST_LEN，新值持续覆盖最旧）。 */
void ring_push(RingBuf *rb, int32_t val)
{
    rb->hist[rb->head] = val;
    rb->head = (rb->head + 1) % HIST_LEN;
    if (rb->count < HIST_LEN) {
        rb->count++;
    }
}

/* 取出最近最多 win 个点，按“最新优先”放入 out，返回实际个数。
 * 最新点是 head-1（模环形），依次向前取。 */
static int ring_last_n(const RingBuf *rb, int win, int32_t *out)
{
    int n = win;
    if (n > rb->count) {
        n = rb->count;
    }
    if (n > HIST_LEN) {
        n = HIST_LEN;
    }
    for (int k = 0; k < n; k++) {
        int idx = (rb->head - 1 - k + HIST_LEN) % HIST_LEN;
        out[k] = rb->hist[idx];
    }
    return n;
}

/* 中值滤波用的小数组排序（插入排序，n 很小，代价可忽略） */
static void sort_asc(int32_t *arr, int n)
{
    for (int i = 1; i < n; i++) {
        int32_t key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

/* 滑动平均：最近 win 点的算术平均。n 个点的和用 long 累计，避免 int32 溢出。 */
int32_t filt_sma(RingBuf *rb, int win)
{
    int32_t buf[HIST_LEN];
    int n = ring_last_n(rb, win, buf);
    if (n == 0) {
        return 0;
    }
    long sum = 0;
    for (int i = 0; i < n; i++) {
        sum += buf[i];
    }
    return (int32_t)(sum / n);
}

/* 中值滤波：对最近 n 个点升序排序，取第 n/2 个作为中位。
 * 偶数时取上中位，奇数取正中位——对尖峰脉冲有强抑制能力。 */
int32_t filt_median(RingBuf *rb, int win)
{
    int32_t buf[HIST_LEN];
    int n = ring_last_n(rb, win, buf);
    if (n == 0) {
        return 0;
    }
    sort_asc(buf, n);
    return buf[n / 2];
}