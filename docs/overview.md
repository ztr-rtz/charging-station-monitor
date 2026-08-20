# 充电站检测系统 · 总体框架总览

> v1.0 ｜ 2026-08-17 ｜ 一页纸看全项目：架构 / 模块 / 文档 / 计划 / 完成状态。
> 状态标记：**[已定]** = 方案已确认 ｜ **[待细化]** = 框架定了、细节待补 ｜ **[待办]** = 尚未开始

---

## 1. 项目定位 [已定]

分布式充电站环境检测系统：STM32F103 多节点采集环境变量（温湿度/烟雾/电压电流），经 RS485 + nRF24L01 双链路汇聚至 i.MX6ULL 网关；网关完成数据汇聚、边缘处理、LVGL 本地展示，经 MQTT 上报 OneNET 云平台，实现远程监控与告警。用途：秋招简历项目。

## 2. 系统架构 [已定]

```
应用层 · OneNET 云平台（物模型 · 曲线 · 告警）
        ▲  MQTT over TCP:1883 · QoS1 · 断线重连
汇聚层 · i.MX6ULL 网关（正点原子 MINI · Linux）
        ├─ 采集通道抽象层（RS485 / nRF24L01 统一调度）
        ├─ 边缘处理（滤波/阈值/告警状态机）
        ├─ SQLite 断网缓存
        └─ LVGL 800×480 @60Hz 本地展示
        ▲  RS485/Modbus RTU（有线主线） + nRF24L01 2.4GHz（无线扩展）
感知层 · STM32F103 节点 × N（Modbus 从站）
        DHT11 · MQ-2 · 电压采样 · 电流采样(ACS712)
```

## 3. 技术栈 [已定]

| 维度 | 选型 |
|---|---|
| 网关平台 | i.MX6ULL（正点原子 MINI，Cortex-A7，Linux） |
| 节点平台 | STM32F103（HAL 库） |
| 语言 | C / C++ |
| 有线链路 | RS485 半双工 + Modbus RTU（9600 8N1） |
| 无线链路 | nRF24L01（SPI，增强型 ShockBurst，32B 单包） |
| 本地展示 | LVGL + Linux framebuffer，800×480 @60Hz |
| 上云 | MQTT（paho-mqtt-c）→ OneNET Studio 物模型 |
| 存储 | SQLite |
| 工程化 | CMake 交叉编译（arm-linux-gnueabihf）· systemd · 看门狗 |

## 4. 采集节点框架（STM32F103） [已细化 → docs/node-design.md]

- 外设清单 [已定]：SHT30 温湿度（I2C）· MQ-2（ADC）· 电压采样（ADC 分压）· 电流采样（ACS712/ADC）· 可选 DHT11（单总线低成本方案）· 蜂鸣器/LED 告警（GPIO）
- 节点软件结构 [已定]：传感器驱动层 → 数据寄存器区 → Modbus 从站；有线（USART1+MAX485）/ 无线（SPI1+nRF24L01）两种变体，传感器电路共用、固件仅通信层差异
- 已细化内容：
  - 引脚分配表（公共 PA0/PA1/PA2/PB6/PB7；有线 PA8/PA9/PA10；无线 PA3~PA7/PB0）
  - 传感器驱动要点（SHT30 I2C 命令/CRC8/换算公式/软件 I2C 决策、MQ-2 预热 60s、电压分压换算、ACS712 电平匹配）
  - Modbus 从站寄存器区 + 40008 状态字 bit 定义
  - 电气与接线要点（5V/3.3V 供电、终端电阻、I2C 上拉）
- 遗留：SHT30 软件 I2C 时序细节、分压量程标定、MQ-2 浓度曲线、从站状态机（node-design.md §6）

## 5. 网关软件框架（i.MX6ULL） [已细化 → 六大域全部闭环]

- 线程模型 [已定]：单进程多线程——采集 / 处理 / UI / MQTT，队列+互斥锁解耦
- 采集域 [已细化 → docs/channel-abstraction.md]：通道抽象层接口（open/send/recv/close）→ RS485 通道 + nRF24L01 通道 → 节点注册表 → Modbus 轮询调度
  - 已细化内容：channel_ops 函数指针表接口定义与语义约定、两种通道实现差异、节点注册表（通道≠节点，多对多绑定）、错误语义表、调度骨架
- 采集域轮询 [已细化 → docs/modbus-polling.md]：2s 周期 / 200ms 超时 / 3 次离线阈值的参数推导（时间预算表）、调度状态机、CRC/异常码校验、离线降频与恢复、单调时钟
- 处理域 [已细化 → docs/data-processing.md]：环形缓冲（30 点/指标）→ 差异化滤波（缓变指标滑动平均 / 尖峰指标中值，窗口 5 点=10s）→ 量程换算表 → 三态告警状态机（正常/预警/告警 + 迟滞防抖）
  - 已细化内容：滤波选型表、SHT30/分压/ACS712 换算公式、告警阈值参考表（温度 45/55℃、湿度 80/90%、烟雾 ADC、电压 ±5%/±10%、电流 25/32A，全部配置化）、状态机动作语义（WARN 不打扰 / ALARM 才上云）
- 存储域 [已细化 → docs/storage-design.md]：SQLite 三表分离（node_data 历史 / cache_queue 断网补传 / alarm_log 告警日志），WAL 读写并发、批量事务提交、30 天保留策略
  - 已细化内容：三表字段契约与索引、原始值+滤波值双保留（可溯源）、补传按 ts 升序 + (node_id,ts) 幂等去重、重试上限兜底、容量预算（≈4.3MB/天/2节点）、NTP 与单调时钟互不干扰
- 展示域 [已细化 → docs/lvgl-ui.md]：四页面布局（实时仪表盘/历史曲线/告警记录/系统状态）+ 常驻状态栏；fb0 双缓冲直驱、脏矩形按需重绘、UI 线程共享快照解耦（锁内仅 memcpy）、三档刷新（LCD 60Hz / LVGL 脏区 / 数据 500ms~1s）
  - 已细化内容：每页 ASCII 布局与数据源（仪表盘快照 · 曲线=环形缓冲+SQLite 两级 · 告警页查 alarm_log）、页面切换（按键/触摸）、资源预算（双缓冲 1.5MB / lv_mem 1~2MB / UI 优先级低于采集）、移植要点（flush_cb/read_cb/lv_tick_inc）
- 上报域 [已细化 → docs/mqtt-report.md]：OneNET 三要素鉴权（ClientID/Username/Password）、物模型属性聚合上报（5s）+ 告警事件即时上报（QoS1 + 幂等去重）、KeepAlive 60s + LWT、指数退避重连（1s~30s 封顶 + 抖动）、cache_queue 补传（先旧后新 + 分批限流 + 先存量后实时）
  - 已细化内容：连接参数表、属性/事件 JSON 报文结构、重连状态机与退避参数、断网写入与恢复补传全流程、与 UI 状态栏/存储域衔接
- 待补细节：（§5 六域全部细化完成，无遗留）

## 6. Linux 驱动规划 [细化中 → docs/device-tree.md]

- 驱动工作点 [已定]：
  1. 设备树 bring-up（调试串口/SPI1/GPIO 复用）→ 已细化 → `device-tree.md`
  2. nRF24L01 内核 SPI 字符驱动（spi_driver · 中断底半部 · file_operations）—— 最高含金量
  3. LED/按键 miscdevice 驱动（状态灯 · input 子系统按键切页）
- 已细化内容（设备树）：
  - 外设→设备树节点映射表（uart1 调试 / uart3+方向脚 RS485 / ecspi1+nrf24l01 / lcdif / gpio-keys / led）
  - pinmux 配置示例：ecspi1 四线 + CSN 软控、uart3 方向脚、LED/按键预埋节点
  - nRF24L01 自定义节点：compatible 匹配键 / interrupts 下降沿 / ce-gpios 自定义属性 / spi-max-frequency
  - 修改流程（dtsi 分层只改板级 dts、&外设引用、make dtbs）+ 验证清单（/proc/device-tree、uevent、fbset、/proc/interrupts）+ 常见坑
- 待补细节：
  - [ ] nRF24L01 驱动接口设计（read/ioctl 语义、收发缓冲）
  - [ ] 驱动 Makefile 与 insmod 加载流程
  - [ ] 驱动与应用层（采集通道）的对接方式

## 7. 协议契约 [已定]

- Modbus RTU 寄存器映射：40001 温度 · 40002 湿度 · 40003 烟雾 · 40004~07 电压电流(双寄存器) · 40008 状态字；功能码 0x03
- MQTT 物模型：broker `mqtt.heclouds.com:1883`，鉴权 ClientID=设备名 / Username=产品ID / Password=设备密钥，上报 Topic `$sys/{产品ID}/{设备名}/thing/property/post`
- 无线帧封装 [待细化]：Modbus 帧压缩进 nRF24L01 32B 单包的适配细节

## 8. 云端（OneNET） [待办]

- 平台已选定，账号/产品/设备未注册（阶段 3 前完成即可）
- 待补：产品创建步骤、物模型定义（属性/告警）、设备鉴权信息管理

## 9. 实施计划 [已定]

> ⚡ **当前实际执行：1 周冲刺版**（详见 [docs/week-1-sprint.md](week-1-sprint.md)）——P0 最小闭环优先，P2 内容降级/砍除。下方为原 5~8 周完整版计划，作为长期参考。

| 阶段 | 内容 | 状态 |
|---|---|---|
| 0 环境准备（~1 周） | 工具链 · 板级 bring-up · 依赖库 · 接线 | [待办] |
| 1 节点与链路（1~2 周） | STM32 驱动 + Modbus 从站 · 网关主站轮询 · 无线节点接入 | [待办] |
| 2 处理与展示（1~2 周） | 滤波/告警 · SQLite · LVGL · LED/按键驱动 | [待办] |
| 3 上云联调（~1 周） | OneNET 接入 · MQTT 上报 · 重连补传 | [待办] |
| 4 工程收尾（~1 周） | systemd · 稳定性 · 演示视频 · 简历打磨 | [待办] |

节奏：每阶段结束录 1 分钟演示 + 写一页总结，最后拼成项目文档与面试讲稿。

## 10. 待补细节总索引

| # | 待补项 | 归属章节 | 状态 |
|---|---|---|---|
| 1 | 采集节点引脚分配与传感器驱动要点 | §4 | 已完成 → node-design.md |
| 2 | Modbus 从站实现方案 | §4 | 已完成（寄存器区/状态字/行为要点） |
| 3 | 通道抽象层接口定义 | §5 | 已完成 → channel-abstraction.md |
| 4 | Modbus 轮询时序与离线判定 | §5 | 已完成 → modbus-polling.md |
| 5 | 滤波参数与告警阈值表 | §5 | 已完成 → data-processing.md |
| 6 | SQLite 表结构 | §5 | 已完成 → storage-design.md |
| 7 | LVGL 页面布局与刷新策略 | §5 | 已完成 → lvgl-ui.md |
| 8 | MQTT 重连与补传策略 | §5 | 已完成 → mqtt-report.md |
| 9 | 设备树节点清单 | §6 | 已完成 → device-tree.md |
| 10 | nRF24L01 驱动接口设计 | §6 | 待补 |
| 11 | 无线帧 32B 封装适配 | §7 | 待补 |
| 12 | OneNET 产品/物模型配置步骤 | §8 | 待补 |
