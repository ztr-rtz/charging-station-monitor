# CHANGELOG

本仓库变更日志，格式参照 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)。
版本号遵循 [SemVer](https://semver.org/lang/zh-CN/)。

## [Unreleased]

- 存储域（SQLite 三表 + WAL）、展示域（LVGL 四页面）—— 框架代码已入，功能待实现

## [v0.1.2] - 2026-08-21

### Added

- `code/mqtt_report.c/h`：上报域 MQTT 客户端（paho-mqtt-c）
  - OneNET token 鉴权（HMAC-SHA256，产品 `5gScvSra72` / 设备 `imx6ull`）
  - Broker：`tcp://mqtts.heclouds.com:1883`
  - 属性上报按节点号动态拼 key：`node{1,2,3}_{temp,humi,smoke,volt,current,alarm}`
  - alarm 状态字收敛为 0/1 上报（物模型 int32 限定）
  - QoS1 至少一次、断线指数退避重连、LWT 遗嘱（online=0 retained）、KeepAlive 60s
  - 5s 聚合上报（mbox_recv_nowait 非阻塞轮询 + 定时触发）
  - 断网缓存/补传占位函数（`mqtt_cache_enqueue` / `mqtt_replay_cache`）
- `docs/onenet-model-3nodes.json`：OneNET 三节点物模型导入文件
  - 18 属性（node1~3 × 6），identifier 统一 `node{1,2,3}_*`
  - 替换原命名混乱的 6 属性物模型（temo 拼写错误、node1_humi 标识/名称错位）
- `mbox.c/h`：`mbox_recv_nowait()` 非阻塞邮箱接收（定时轮询场景）
- `data_process.h`：`MSG_MQTT_REPORT` 消息类型、`TH_MQTT_NAME` 线程名
- `main.c`：注册 MQTT 上报线程

### Changed

- `Makefile`：SOURCES 增加 `mqtt_report.c`，LDFLAGS 增加 `-lpaho-mqtt3c`
- `data_process.c`：处理结果同时转发 MQTT 线程

## [v0.1.1] - 2026-08-20

### Added

- `code/` 网关侧源码全部入库（20 个文件）
- 采集域：`uart.c/h`（串口原始模式 + RS485 双通道：内核 TIOCSRS485 / 手动 RTS）、
  `modbus_env.c/h`（Modbus RTU 主站，0x03 读 8 寄存器，CRC16，异常码/长度校验）、
  `main.c` 轮询（2s 周期 / 200ms 超时 / 3 次判离线 / 降频探测）
- 通信域：`mbox.c/h` 按名寻址定长环形邮箱（64 槽）
- 快照域：`node_snapshot.c/h` 每节点独立锁，锁内仅 memcpy
- 处理域：`filter.c/h`（30 点环形缓冲 + 5 点滑动均值/中值）、
  `alert.c/h`（三态迟滞告警状态机 + 状态字合并 bit1~5）、
  `data_process.c/h`（流水线胶水：收邮箱 → 滤波/量程/告警 → 写快照）
- 框架线程：`storage.c/h`（SQLite 三表占位）、`lvgl_ui.c/h`（LVGL 占位）

### Changed

- `README.md`：补充代码章节（目录结构 / 数据流 / 编译运行 / 实现状态）
- `.gitignore`：`!code/Makefile` 例外、忽略 `gateway_modbus_master` 编译产物

## [v0.1.0] - 2026-08-20

### Added

- 项目设计文档 15 份（`docs/`）：需求规格、高层设计、节点设计、Modbus 轮询、
  数据处理、存储设计、MQTT 上报、LVGL UI、设备树、通道抽象、冲刺计划等
- `README.md`：项目简介与文档索引
