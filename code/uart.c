/* =====================================================================
 * uart.c - 串口(RS485)底层封装实现
 * ---------------------------------------------------------------------
 * 要点：
 *   1) 原始模式配置：8位 无校验 1停止，关闭软/硬件流控、回显、规范处理
 *   2) 二进制安全读写：不追加 '\0'，不把二进制帧当字符串
 *   3) 读超时用 select 实现（Vmin/Vtime 全 0，非阻塞读）
 *   4) RS485 方向：优先内核 TIOCSRS485（硬件自动控制），失败回退手动 RTS
 */

#include "uart.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <linux/serial.h>

/* ============ 波特率映射：整数 → termios 常量 ============ */
speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return -1;      /* 不支持则返回 -1，由上层报错 */
    }
}

/* ============ 打开串口并配置为原始模式 ============ */
int serial_open(const char *path, int baud, int timeout_ms)
{
    speed_t speed = baud_to_speed(baud);
    if (speed == (speed_t)-1) {
        fprintf(stderr, "不支持的波特率：%d\n", baud);
        return -1;
    }
    (void)timeout_ms;   /* 超时实际由 serial_read 的 select 控制，这里仅作记录 */

    /* 打开：O_NONBLOCK 避免 open 阻塞在载波检测；读写 + 不成为控制终端 */
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open serial");
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    /* ---- 配置 termios：8N1 原始模式 ---- */
    cfsetispeed(&tio, speed);   /* 输入波特率 */
    cfsetospeed(&tio, speed);   /* 输出波特率 */

    /* 控制标志：数据位 8、无校验、1 停止位、关闭 RTS/CTS 硬件流控 */
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    tio.c_cflag |= (CS8 | CLOCAL | CREAD);   /* 忽略调制解调线 + 使能接收 */

    /* 本地标志：关闭规范模式、回显、擦除、信号生成 */
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    /* 输出标志：关闭输出后处理（原样输出） */
    tio.c_oflag &= ~OPOST;

    /* 输入标志：关闭软流控/校验/换行转换等一切处理 */
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | INPCK | ISTRIP | BRKINT |
                     ICRNL | IGNCR | INLCR);

    /* 非阻塞读：Vmin=0 立即返回，超时交给上层 select */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);   /* 清空收发缓冲，避免陈旧数据 */
    return fd;
}

/* ============ 写：二进制安全（原样写 len 字节） ============ */
ssize_t serial_write(int fd, const void *buf, size_t len)
{
    ssize_t n = write(fd, buf, len);
    if (n < 0) {
        perror("serial write");
    }
    return n;
}

/* ============ 读：select 超时 + 二进制安全 ============
 * 返回读取字节数；超时(select 超时)返回 0；出错返回 -1。 */
ssize_t serial_read(int fd, void *buf, size_t len, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv, *ptv = NULL;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    if (timeout_ms > 0) {                       /* 构造 select 超时时间 */
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }                                           /* timeout_ms<=0 表示无限等待 */

    int r = select(fd + 1, &rfds, NULL, NULL, ptv);
    if (r < 0) {
        if (errno == EINTR) {                   /* 被信号打断：视为超时 */
            return 0;
        }
        perror("select");
        return -1;
    }
    if (r == 0) {
        return 0;                               /* 超时，没有数据 */
    }

    ssize_t n = read(fd, buf, len);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("serial read");                 /* 非阻塞读返回 EAGAIN 属正常 */
        return -1;
    }
    return n;
}

/* ============ 关闭串口 ============ */
int serial_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
    return 0;
}

/* ============ 内核 RS485 方向接管 ============
 * 若内核/设备树已使能 RS485，则让驱动在发送时自动拉高 DE、结束自动拉回 RE，
 * 完全由内核接管半双工方向，无需应用手动切 RTS。 */
int serial_rs485_kernel_enable(int fd)
{
#ifdef TIOCSRS485
    struct serial_rs485 rs485conf;

    memset(&rs485conf, 0, sizeof(rs485conf));
    rs485conf.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;

    if (ioctl(fd, TIOCSRS485, &rs485conf) == 0) {
        return 0;                               /* 内核接管成功 */
    }
#endif
    return -1;                                  /* 不支持则返回 -1，上层回退手动 */
}

/* ============ 手动 RS485 方向控制 ============
 * 约定：RTS 置位 = 发送（MAX485 DE 拉高），RTS 复位 = 接收（RE 有效）。
 * 若你的硬件极性相反，翻转下方两行即可。 */
int serial_rs485_dir(int fd, int tx)
{
    unsigned int status;

    if (ioctl(fd, TIOCMGET, &status) != 0) {   /* 读当前 modem 状态位 */
        perror("TIOCMGET");
        return -1;
    }
    if (tx) {
        status |= TIOCM_RTS;                   /* 置位 RTS → 发送态 */
    } else {
        status &= ~TIOCM_RTS;                  /* 复位 RTS → 接收态 */
    }
    if (ioctl(fd, TIOCMSET, &status) != 0) {   /* 写回 modem 状态位 */
        perror("TIOCMSET");
        return -1;
    }
    return 0;
}