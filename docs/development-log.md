# 开发日志（Development Log）

> 按**日期**记录本项目每天的开发进展（提交了什么 / 新增了什么），配合
> [CHANGELOG.md](../CHANGELOG.md) 一起看：本文件看**开发过程**，CHANGELOG 看**版本里程碑**。
>
> 约定：每天一个 `## YYYY-MM-DD` 小节，按时间倒序排列；每条记录格式
> `时间 提交号（类型）：做了什么`。当天没有开发则不写。

## 2026-08-21

- 18:23 提交 `345c9f1`（docs）：新增 CHANGELOG.md 版本变更日志（v0.1.0 ~ v0.1.2）
- 18:21 提交 `ffb1223`（feat, v0.1.2）：上报域落地——MQTT 接入 OneNET + 三节点物模型
  - 新增 `code/mqtt_report.c/h`：paho-mqtt-c 客户端
    - OneNET token 鉴权（HMAC-SHA256，产品 `5gScvSra72` / 设备 `imx6ull`）
    - Broker：`tcp://mqtts.heclouds.com:1883`
    - 属性上报按节点号动态拼 key：`node{1,2,3}_{temp,humi,smoke,volt,current,alarm}`
    - alarm 状态字收敛为 0/1（物模型 int32 限定）、QoS1、指数退避重连、LWT、KeepAlive 60s
    - 5s 聚合上报（mbox_recv_nowait 非阻塞轮询 + 定时触发）
  - 新增 `docs/onenet-model-3nodes.json`：OneNET 三节点物模型导入文件（18 属性）
  - 新增 `mbox.c/h`：`mbox_recv_nowait()` 非阻塞邮箱接收
  - 更新 `main.c`（注册 MQTT 上报线程）、`Makefile`（`-lpaho-mqtt3c`）、
    `data_process.c/h`（`MSG_MQTT_REPORT` 消息类型 + 处理结果转发）

## 2026-08-20

- 15:31 提交 `6e7b54b`（feat, v0.1.1）：网关侧代码入库——采集/通信/快照/处理域落地
  - 新增 `code/` 全部 20 个文件（+1922 行）：
    - 采集域：`uart.c/h`（串口原始模式 + RS485 双通道：内核 TIOCSRS485 / 手动 RTS）、
      `modbus_env.c/h`（Modbus RTU 主站：0x03 读 8 寄存器、CRC16、异常码/长度校验）、
      `main.c`（轮询：2s 周期 / 200ms 超时 / 3 次判离线 / 降频探测）
    - 通信域：`mbox.c/h`（按名寻址定长环形邮箱，64 槽）
    - 快照域：`node_snapshot.c/h`（每节点独立锁，锁内仅 memcpy）
    - 处理域：`filter.c/h`（30 点环形缓冲 + 5 点滑动均值/中值）、
      `alert.c/h`（三态迟滞告警状态机 + 状态字 bit1~5）、
      `data_process.c/h`（流水线胶水：收邮箱 → 滤波/量程/告警 → 写快照）
    - 框架线程：`storage.c/h`（SQLite 三表占位）、`lvgl_ui.c/h`（LVGL 占位）
  - 更新 `README.md`（补充代码章节）、`.gitignore`（`!code/Makefile` 例外）
- 15:08 提交 `4ec0485`（init, v0.1.0）：项目启动——设计文档 15 份入库
  - 新增 `docs/`：需求规格（SRS）、高层设计（HLD）、节点设计、Modbus 轮询、
    数据处理、存储设计、MQTT 上报、LVGL UI、设备树、通道抽象、冲刺计划等
  - 新增 `README.md`（项目简介 + 文档索引）
