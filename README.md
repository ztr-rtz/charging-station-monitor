# 充电站环境检测系统

分布式充电站环境检测系统：**STM32F103 多节点采集 + i.MX6ULL 边缘网关 + OneNET 云平台**。

- 感知层：STM32F103 采集节点（SHT30 温湿度、MQ-2 烟雾、电压电流），Modbus RTU 从站
- 汇聚层：i.MX6ULL 网关（正点原子 MINI）数据汇聚、边缘滤波/告警、LVGL 800×480 本地展示
- 应用层：MQTT 接入 OneNET，远程监控与告警

## 文档

| 文档 | 说明 |
|---|---|
| [docs/week-1-sprint.md](docs/week-1-sprint.md) | **★ 一周冲刺计划**（当前执行：7 天全链路闭环 + 砍单清单） |
| [docs/requirements-spec.md](docs/requirements-spec.md) | **软件需求规格说明书（SRS）**：功能/非功能需求、约束、接口需求、验收标准 |
| [docs/high-level-design.md](docs/high-level-design.md) | **概要设计说明书（HLD）**：系统架构、模块设计、接口设计、数据设计、运行设计 |
| [docs/project-summary.md](docs/project-summary.md) | **项目总结**（架构/模块/协议/计划/简历/面试问答，面试前看这一份） |
| [docs/project-plan.md](docs/project-plan.md) | 项目框架与实施计划（架构/选型/协议/分阶段计划/面试素材） |
| [docs/overview.md](docs/overview.md) | 总体框架总览 + 待补细节状态索引 |

> 当前执行 **1 周冲刺版计划**（`docs/week-1-sprint.md`）：P0 最小闭环优先，P2 内容（nRF24L01 内核驱动/断网补传/设备树自编译）已按计划降级或砍除。

## 代码

网关侧固件位于 `code/`（Linux / C99，单进程多线程），实现"采集 → 处理 → 存储/展示"全链路。

### 目录结构

```
code/
├── main.c            # 主程序：线程编排 + Modbus 轮询采集（生产者）
├── uart.c/h          # 串口原始模式 + RS485 方向控制（内核 TIOCSRS485 / 手动 RTS 双通道）
├── modbus_env.c/h    # Modbus RTU 主站：0x03 读 8 寄存器、CRC16、异常码/长度校验
├── mbox.c/h          # 线程邮箱：按名寻址、定长环形队列（64 槽），send 非阻塞 / recv 阻塞
├── node_snapshot.c/h # 共享快照：每节点独立锁，锁内仅 memcpy（决策 D9）
├── filter.c/h        # 环形缓冲（30 点）+ 差异化滤波（滑动均值 / 中值）
├── alert.c/h         # 三态告警状态机（正常→预警→告警）＋迟滞防抖动
├── data_process.c/h  # 处理流水线：滤波→量程换算→告警→写快照→转发存储
├── storage.c/h       # SQLite 存储线程（框架，落库逻辑待填充）
├── lvgl_ui.c/h       # LVGL 展示线程（框架，刷屏逻辑待填充）
└── Makefile          # arm 交叉编译脚本
```

### 数据流

```
采集线程 ──邮箱(MSG_ENV_DATA)──► 处理线程 ──滤波/量程/告警──► 共享快照 ──► UI/存储/MQTT
                                   └──邮箱(MSG_STORE_NODE)──► 存储线程(SQLite)
```

### 编译与运行

交叉编译（目标板 i.MX6ULL / Cortex-A7，需 `arm-linux-gnueabihf-gcc`）：

```bash
make                      # 产物 gateway_modbus_master
make clean
```

运行（默认串口 `/dev/ttymxc2`、波特率 9600、节点地址 1/2/3，均可由命令行覆盖）：

```bash
./gateway_modbus_master [串口] [波特率] [节点地址...]
./gateway_modbus_master /dev/ttymxc2 9600 1 2 3
```

### 实现状态

| 模块 | 状态 |
|---|---|
| 采集域：串口 / RS485 / Modbus 主站 / 轮询调度 / 离线判定 | ✅ 完整 |
| 通信域：线程邮箱 | ✅ 完整 |
| 快照域：共享快照（锁内 memcpy） | ✅ 完整 |
| 处理域：环形缓冲 / 滤波 / 量程换算 / 三态迟滞告警 / 状态字 | ✅ 完整 |
| 存储域：SQLite 三表（node_data / cache_queue / alarm_log） | 🔲 框架（落库 TODO） |
| 展示域：LVGL 800×480 四页面 | 🔲 框架（刷屏 TODO） |
| 上报域：MQTT 上云 + 断网补传 | 🔲 未开始 |
