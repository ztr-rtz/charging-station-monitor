# 网关存储域 · SQLite 数据库设计

> v1.0 ｜ 2026-08-17 ｜ 对应总体框架 §5 存储域 ｜ 待补索引 #6
> 输入：docs/data-processing.md 的归一化数据 + 告警事件 ｜ 下游：UI 历史曲线（#7）、MQTT 断网补传（#8）
> 本文只定义表结构与策略，不含代码实现。

---

## 1. 定位与职责

数据库落在 **i.MX6ULL 的 eMMC/SD 卡**（`/var/lib/charging-monitor/monitor.db`），掉电不丢。三张表，三个职责：

| 表 | 职责 | 写入方 | 读取方 |
|---|---|---|---|
| `node_data` | 历史数据（含原始值+滤波值） | 处理线程 | UI 曲线 · 云端历史 · 调试 |
| `cache_queue` | 断网补传队列 | 处理线程 | MQTT 线程（补传） |
| `alarm_log` | 告警事件日志 | 处理线程 | UI 告警页 · 云端事件 |

> 设计原则：**写多读少用队列，写少读多用日志**——数据表按批写，队列表按需清，日志表只追加。

---

## 2. 表结构定义（建表契约）

### 2.1 `node_data` —— 历史数据表

```sql
CREATE TABLE node_data (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id   INTEGER NOT NULL,          -- 节点地址 1..N
    ts        INTEGER NOT NULL,          -- 采集时间戳（Unix 秒，本地）
    raw_temp  INTEGER,                   -- 温度原始寄存器值（SHT30 16bit）
    temp      REAL,                      -- 滤波后温度 ℃
    raw_humi  INTEGER,
    humi      REAL,                      -- 滤波后湿度 %RH
    raw_smoke INTEGER,
    smoke     REAL,                      -- 滤波后烟雾（V / 待标定 ppm）
    raw_volt  INTEGER,
    volt      REAL,                      -- 滤波后电压 V
    raw_curr  INTEGER,
    curr      REAL,                      -- 滤波后电流 A
    status    INTEGER DEFAULT 0          -- 40008 状态字快照（传感器故障位）
);
CREATE INDEX idx_node_data_ts ON node_data(node_id, ts);
```

**关键决策：原始值 + 滤波值双保留**

- `raw_*` 列存采集原始寄存器值，`滤波列` 存处理后物理量
- 目的：**可溯源**——联调时发现数据异常，能回查"是传感器问题还是滤波问题"；也支持将来换滤波算法后离线重算
- 代价：每行多 5 个整数列 ≈ 20B，容量预算内可忽略

> 面试点：很多人只存滤波结果，你讲"原始值双保留用于溯源与算法迭代"，是数据意识上的加分项。

### 2.2 `cache_queue` —— 断网补传队列

```sql
CREATE TABLE cache_queue (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id INTEGER NOT NULL,            -- 节点地址
    ts      INTEGER NOT NULL,            -- 采集时间戳（补传顺序与去重依据）
    payload TEXT    NOT NULL,            -- 物模型 JSON（OneNET 报文）
    status  INTEGER DEFAULT 0,           -- 0 待传 / 1 已传
    retry   INTEGER DEFAULT 0,           -- 已重试次数
    sent_at INTEGER                     -- 实际发送时间（已传记录清理依据）
);
CREATE INDEX idx_cache_queue_status ON cache_queue(status, ts);
```

**关键决策：**

- **补传顺序**：`ORDER BY ts ASC`，先旧后新，保证云端时间线不乱
- **幂等去重**：云端按 `(node_id, ts)` 去重——重传不产生重复记录（配合 QoS1"至少一次"语义）
- **已传清理**：`status=1` 且 `sent_at` 超过保留期（如 7 天）的行定时删除
- **异常兜底**：重试超上限（如 5 次）仍失败的行保留，等待下个周期；不清空、不丢弃——断网数据是"必须补"的，不是"尽力而为"

> 面试点：**补传 = 至少一次投递 + 幂等去重**，这是分布式消息投递语义的标准解，一问 QoS 就能引到这。

### 2.3 `alarm_log` —— 告警事件日志

```sql
CREATE TABLE alarm_log (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id   INTEGER NOT NULL,
    metric    TEXT    NOT NULL,          -- temp / humi / smoke / volt / curr
    level     INTEGER NOT NULL,          -- 1=WARN / 2=ALARM
    event     INTEGER NOT NULL,          -- 1=进入 / 0=恢复
    value     REAL,                      -- 触发时滤波值
    threshold REAL,                      -- 触发阈值（当时配置值）
    ts        INTEGER NOT NULL           -- 事件时间戳
);
CREATE INDEX idx_alarm_log_ts ON alarm_log(ts);
```

**关键决策：**

- **只记事件不记过程**：进入/恢复各一条，不每周期记状态——日志量小、查询快
- **记录阈值快照**：`threshold` 存触发时的配置值——后续改阈值，历史日志仍能还原"当时为什么告警"
- 状态机每次转移写一条，配合迟滞设计不会产生抖动风暴（55℃ 进入 / 52℃ 退出）

---

## 3. 运行时策略（比建表更值钱的工程细节）

### 3.1 WAL 模式（Write-Ahead Logging）

- `PRAGMA journal_mode=WAL`：写日志先落 WAL，读直接读主库，**读写不互斥**
- 为什么必要：处理线程写 `node_data` 的同时，UI 线程/MQTT 线程在读——多线程并发访问是本项目的常态
- 崩溃安全：事务原子，进程被杀不损坏已提交数据；定期 `PRAGMA wal_checkpoint` 合并

### 3.2 批量事务提交

- 处理线程**攒批提交**：缓存 N 条（如 10 条）或间隔 1s，一次 `BEGIN...COMMIT` 写入
- 为什么：单条事务有 fsync 开销（毫秒级），逐条写在高频采样下浪费 IO；批量可提升 10 倍级写吞吐
- 代价权衡：断电最多丢"未提交批次"（≤1s 数据）——环境监测业务可容忍，与失联策略一致

### 3.3 容量与保留策略

| 项 | 数值 | 说明 |
|---|---|---|
| 单条 `node_data` | ≈50B | 15 列，含索引膨胀 |
| 日增长（2 节点×2s） | ≈4.3MB/天 | 与早期估算一致 |
| 断网一周缓存 | ≤25MB | cache_queue 无压力 |
| 保留策略 | 30 天 | 定时清理 `ts < now-30d`，30 天 ≈130MB，SD 卡可承受 |

- 清理用**批量删除 + `VACUUM` 调度**（低频，如每周一次），避免碎片膨胀
- 曲线展示只取最近 1 小时（环形缓冲在内存），DB 历史是"按需查询"，UI 不背容量包袱

### 3.4 时钟与一致性

- `ts` 用**本地时间**记录（业务展示语义），补传对账用 `(node_id, ts)` 做幂等键
- 网关若有 NTP 校时，时间跳变只影响"显示时间轴"，不影响补传去重（幂等键不受单调性要求）——与采集域用 CLOCK_MONOTONIC 做调度计时互不干扰

---

## 4. 面试价值点

1. **选型逻辑**：嵌入式 Linux 为什么 SQLite（单文件、零配置、够用）而不是 MySQL/文件落盘
2. **WAL 模式**：读写并发场景的具体解法，能讲出"为什么默认 journal 不够"
3. **批量事务**：写吞吐优化与"断电最多丢 1s"的权衡——工程取舍思维
4. **补传幂等**：至少一次 + 幂等键，与 MQTT QoS 语义衔接（分布式消息投递标准解）
5. **数据保留与容量管理**：30 天预算算得清清楚楚，不是"先存着再说"
6. **双保留设计**：原始值 + 滤波值，数据可溯源、算法可迭代

**简历 bullet 参考：**
> 设计网关本地存储：SQLite 三表分离（历史数据/断网补传队列/告警日志），WAL 并发读写下批量事务提交，容量按 30 天预算管理；断网数据带时间戳缓存、恢复后按序补传并以 (node_id, ts) 幂等去重。

---

## 5. 遗留细化项（后续补）

- [ ] SQLite 编译/移植（交叉编译 sqlite3，或直接用系统包）——阶段 0 环境准备
- [ ] 批量提交的批大小/间隔实测调优（1s vs 2s 批量）
- [ ] 清理任务实现（谁触发、多久一次、VACUUM 时机）——可并入处理线程或独立维护线程
- [ ] 云端历史数据对账接口（若 OneNET 支持，校验补传是否齐全）
