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
