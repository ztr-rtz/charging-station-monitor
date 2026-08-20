#ifndef MODBUS_ENV_H
#define MODBUS_ENV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* =====================================================================
 * Modbus RTU 环境检测寄存器契约（对齐 requirements-spec.md §5.2）
 * 偏移  地址    含义          类型    缩放
 * 0     40001   温度          int16   ×10  (0.1℃)
 * 1     40002   湿度          uint16  ×10  (0.1%RH)
 * 2     40003   烟雾          uint16  ADC 原始值
 * 3     40004   电压高16位    uint16  ×100 (0.01V)
 * 4     40005   电压低16位    uint16  ×100
 * 5     40006   电流高16位    uint16  ×1000(0.001A)
 * 6     40007   电流低16位    uint16  ×1000
 * 7     40008   状态字        uint16  bit0故障 / bit1~5越限
 * 说明：多寄存器按 Modbus 大端，数值高位在地址较小的寄存器。
 * ===================================================================== */

#define MB_ENV_REG_COUNT   8

#define MB_ENV_REG_TEMP    0
#define MB_ENV_REG_HUMID   1
#define MB_ENV_REG_SMOKE   2
#define MB_ENV_REG_VOLT_HI 3
#define MB_ENV_REG_VOLT_LO 4
#define MB_ENV_REG_CURR_HI 5
#define MB_ENV_REG_CURR_LO 6
#define MB_ENV_REG_STATUS  7

/* 40008 状态字位定义 */
#define MB_ENV_ST_FAULT      0x0001  /* bit0 传感器故障       */
#define MB_ENV_ST_TEMP_ALM   0x0002  /* bit1 温度越限          */
#define MB_ENV_ST_HUMID_ALM  0x0004  /* bit2 湿度越限          */
#define MB_ENV_ST_SMOKE_ALM  0x0008  /* bit3 烟雾越限          */
#define MB_ENV_ST_VOLT_ALM   0x0010  /* bit4 电压越限          */
#define MB_ENV_ST_CURR_ALM   0x0020  /* bit5 电流越限          */

/* 8 寄存器解析后的结构化结果 */
typedef struct {
    int16_t  temperature;   /* ×10（0.1℃）   */
    uint16_t humidity;      /* ×10（0.1%RH）  */
    uint16_t smoke;         /* ADC 原始值      */
    uint32_t voltage;       /* ×100（0.01V）   */
    uint32_t current;       /* ×1000（0.001A） */
    uint16_t status;        /* 状态字          */
} EnvRegisters;

/* Modbus RTU CRC16（多项式 0xA001） */
uint16_t calculateCRC16(const uint8_t *data, uint16_t length);

/* 构建“读保持寄存器（功能码 0x03）”请求帧，从 40001 起读 8 个寄存器。
 * buf 容量需 >= 8，返回帧长（8）；出错返回 -1。 */
int modbus_env_build_read(uint8_t slave, uint8_t *buf, size_t cap);

/* 解析响应帧。
 * slave : 期望从站地址
 * frame : 接收到的整帧（含 CRC）
 * len   : 帧长度
 * out   : 输出解析结果
 * 返回 true 表示解析成功；返回 false 表示校验失败/功能码异常/长度错误。 */
bool modbus_env_parse_read(uint8_t slave, const uint8_t *frame, size_t len,
                           EnvRegisters *out);

#endif /* MODBUS_ENV_H */
