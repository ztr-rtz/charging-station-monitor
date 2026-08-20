/* =====================================================================
 * modbus_env.c - Modbus RTU 环境检测从站协议层
 * ---------------------------------------------------------------------
 * 职责：
 *   - CRC16（多项式 0xA001，Modbus RTU 标准）
 *   - 构建“读保持寄存器 0x03”请求帧（40001 起 8 个寄存器）
 *   - 解析响应帧：校验 地址/功能码/字节数/CRC，并解出 EnvRegisters
 *
 * 对应需求：FR-G07 帧校验，接口契约 requirements-spec.md §5.2。
 */

#include "modbus_env.h"
#include <stdio.h>

/* ============ CRC16（Modbus RTU，多项式 0xA001） ============
 * 标准位算法：初值 0xFFFF，每字节异或后逐位右移，LSB=1 时异或 0xA001。 */
uint16_t calculateCRC16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFF;            /* 初始值（标准规定） */
    uint16_t i;
    for (i = 0; i < length; i++) {
        crc ^= data[i];               /* 字节异或进 CRC 寄存器 */
        for (int j = 0; j < 8; j++) { /* 逐位处理 8 个 bit */
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;        /* 多项式 0xA001（低字节表示） */
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ============ 构建“读保持寄存器 0x03”请求帧 ============
 * 请求帧格式（8 字节）：
 *   [从站地址][0x03][起始寄存器高][起始寄存器低][数量高][数量低][CRC低][CRC高]
 * 从 40001（偏移 0x0000）起读 MB_ENV_REG_COUNT(8) 个寄存器。
 * 返回帧长 8；buf 为空或容量不足返回 -1。 */
int modbus_env_build_read(uint8_t slave, uint8_t *buf, size_t cap)
{
    if (buf == NULL || cap < 8) {
        return -1;
    }

    buf[0] = slave;                         /* 从站地址 */
    buf[1] = 0x03;                          /* 功能码：读保持寄存器 */
    buf[2] = 0x00;                          /* 起始寄存器高字节（40001 -> 0x0000） */
    buf[3] = 0x00;                          /* 起始寄存器低字节 */
    buf[4] = 0x00;                          /* 寄存器数量高字节 */
    buf[5] = MB_ENV_REG_COUNT;              /* 寄存器数量 = 8 */

    uint16_t crc = calculateCRC16(buf, 6);  /* 对前 6 字节算 CRC */
    buf[6] = (uint8_t)(crc & 0xFF);         /* CRC 低字节在前（RTU 规定） */
    buf[7] = (uint8_t)(crc >> 8);           /* CRC 高字节在后 */
    return 8;
}

/* ============ 解析“读保持寄存器”响应帧 ============
 * 成功响应（21 字节）：
 *   [从站地址][0x03][字节数=16][8*2字节数据][CRC低][CRC高]
 * 异常响应（5 字节）：
 *   [从站地址][0x83][异常码][CRC低][CRC高]
 * 返回 true 表示成功并把各字段填入 out；否则 false。 */
bool modbus_env_parse_read(uint8_t slave, const uint8_t *frame, size_t len,
                           EnvRegisters *out)
{
    if (frame == NULL || out == NULL) {
        return false;
    }

    /* 期望的正常应答长度：1地址+1功能码+1字节数+16数据+2CRC = 21 */
    const size_t expect_len = 1 + 1 + 1 + MB_ENV_REG_COUNT * 2 + 2;

    if (len < 5) {
        return false;   /* 太短，连异常应答都构不成 */
    }

    if (frame[0] != slave) {
        return false;   /* 从站地址不匹配（收到的是别的节点回复） */
    }

    /* 异常响应：功能码高位置 1（0x03|0x80=0x83），后跟异常码 */
    if (frame[1] == (0x03 | 0x80)) {
        fprintf(stderr, "slave 0x%02X 返回异常码 0x%02X\n", slave, frame[2]);
        return false;
    }

    if (frame[1] != 0x03) {
        fprintf(stderr, "功能码错误：0x%02X\n", frame[1]);
        return false;
    }

    if (len != expect_len) {
        /* 长度不符先报错（最终由 CRC 把关） */
        fprintf(stderr, "响应长度错误：%u，期望 %u\n", (unsigned)len,
                (unsigned)expect_len);
        return false;
    }

    uint8_t byte_count = frame[2];
    if (byte_count != MB_ENV_REG_COUNT * 2) {   /* 字节数必须是 8*2=16 */
        fprintf(stderr, "数据字节数错误：%u\n", byte_count);
        return false;
    }

    /* CRC 校验：对地址+功能码+字节数+数据部分（expect_len-2）计算 */
    uint16_t crc_calc = calculateCRC16(frame, expect_len - 2);
    uint16_t crc_recv = (uint16_t)frame[expect_len - 1] << 8 |
                        (uint16_t)frame[expect_len - 2];   /* 接收的 CRC(高前低后) */
    if (crc_recv != crc_calc) {
        fprintf(stderr, "CRC 校验失败：接收 0x%04X，计算 0x%04X\n",
                crc_recv, crc_calc);
        return false;
    }

    /* 解析 8 个寄存器（Modbus 大端：每寄存器高字节在前） */
    uint16_t regs[MB_ENV_REG_COUNT];
    int i;
    for (i = 0; i < MB_ENV_REG_COUNT; i++) {
        regs[i] = (uint16_t)frame[3 + i * 2] << 8 |
                  (uint16_t)frame[3 + i * 2 + 1];
    }

    /* 按寄存器契约映射到 EnvRegisters：
     * 温度 int16×10 / 湿度 uint16×10 / 烟雾 uint16 原始
     * 电压 uint32×100（高16位在前）/ 电流 uint32×1000 / 状态字 */
    out->temperature = (int16_t)regs[MB_ENV_REG_TEMP];
    out->humidity    = regs[MB_ENV_REG_HUMID];
    out->smoke       = regs[MB_ENV_REG_SMOKE];
    out->voltage     = ((uint32_t)regs[MB_ENV_REG_VOLT_HI] << 16) |
                        regs[MB_ENV_REG_VOLT_LO];
    out->current     = ((uint32_t)regs[MB_ENV_REG_CURR_HI] << 16) |
                        regs[MB_ENV_REG_CURR_LO];
    out->status      = regs[MB_ENV_REG_STATUS];

    return true;
}