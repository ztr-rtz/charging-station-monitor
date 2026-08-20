/* =====================================================================
 * uart.h - 串口(RS485)底层封装，供 Modbus 主站使用
 * ---------------------------------------------------------------------
 * 职责：
 *   - 打开/配置串口为“原始模式”（8N1，无流控，非阻塞+select 超时）
 *   - 二进制安全读写（绝不追加字符串终止符，避免破坏帧内 0x00）
 *   - RS485 半双工方向控制：优先内核 TIOCSRS485，回退手动 RTS
 *
 * 对应需求：FR-G01 通道抽象，FR-G04 串口参数(9600 8N1)/200ms 超时。
 */

#ifndef UART_H
#define UART_H

#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

/* 打开串口设备并按指定波特率配置为原始模式。
 * path      : 设备路径，如 "/dev/ttymxc2"
 * baud      : 波特率数值（9600/19200/38400/57600/115200）
 * timeout_ms: 单次读超时基准（毫秒），实际由 serial_read 的 select 控制
 * 返回 fd；失败返回 -1 */
int serial_open(const char *path, int baud, int timeout_ms);

/* 把 buf 的 len 字节写入串口（二进制安全），返回写入字节数；失败返回 -1 */
ssize_t serial_write(int fd, const void *buf, size_t len);

/* 从串口读取数据到 buf（二进制安全，不追加字符串终止符）。
 * 内部用 select 实现最长 timeout_ms 超时：超时返回 0，出错返回 -1。
 * 单次最多读 len 字节。 */
ssize_t serial_read(int fd, void *buf, size_t len, int timeout_ms);

/* 关闭串口并做清理 */
int serial_close(int fd);

/* 使用内核 RS485 驱动接管收发方向（devicetree 启用 RS485 时推荐）。
 * 成功返回 0；内核不支持返回 -1（上层可回退到 serial_rs485_dir）。 */
int serial_rs485_kernel_enable(int fd);

/* 手动控制 RS485 半双工方向（约定 MAX485 RE/DE 接 UART RTS）。
 * tx=1 置发送态（DE 高），tx=0 置接收态（RE 有效）。成功返回 0，失败返回 -1。 */
int serial_rs485_dir(int fd, int tx);

/* 波特率整数到 termios speed_t 的映射；不支持返回 (speed_t)-1 */
speed_t baud_to_speed(int baud);

#endif /* UART_H */