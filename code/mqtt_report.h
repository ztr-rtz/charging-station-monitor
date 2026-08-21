#ifndef MQTT_REPORT_H
#define MQTT_REPORT_H

#include <stdint.h>

/* 处理线程 -> MQTT 线程 的消息负载（与 StoreMsg 同结构，方便 proc 统一转发） */
typedef struct {
    uint8_t idx;
    int     online;
    /* 原始值由快照读取，此处仅传 proc 结果 */
} MqttMsg;

/* MQTT 线程入口：配合 mbox_register(m, TH_MQTT_NAME, mqtt_thread, m) */
void *mqtt_thread(void *arg);

#endif /* MQTT_REPORT_H */