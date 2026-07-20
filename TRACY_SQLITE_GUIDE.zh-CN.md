# Tracy 0.13.1 SQLite 数据阅读与分析指南

本文档面向 `tracy-sqliteexport` 生成的 SQLite 数据库，用于：

- 判断数据库是否完整、是否对应预期的 Tracy 捕获；
- 快速了解捕获中有哪些线程、CPU/GPU 区间、内存、上下文切换和采样数据；
- 为后续人工分析或交给其他 Codex/AI 会话分析提供统一入口；
- 避免把“捕获中本来没有数据”误判为“导出时丢失数据”。

当前导出器和本文档针对：

- Tracy：`0.13.1`
- 导出 schema：`1`
- 数据库文件可能达到数 GB，请避免使用会一次性载入整表的 GUI 操作。

---

## 1. 先理解完整性保证

数据库的完整性分为两层。

### 1.1 原始捕获层

`capture_chunks` 保存原始 Tracy 捕获文件的分块内容、顺序和 SHA-256。

因此，即使将来发现某个分析表需要重新解释，只要 `capture_chunks` 校验通过，原始捕获仍然可以恢复，不会因为 SQLite 导出而不可逆地丢失。

### 1.2 结构化分析层

`export_audit` 记录每个导出分区的：

- 源对象数量；
- 实际导出数量；
- 数据库表中的数量；
- 对应分区的检查结果。

当前验证器还会检查：

- 原始捕获整体 SHA-256；
- 每个 `capture_chunks` 分块的 SHA-256；
- 导出数量与数据库实际行数；
- 整数、浮点数和压缩数据的原始位模式；
- Tracy 字符串、调用栈、内存和 GPU 等对象之间的引用关系；
- SQLite `foreign_key_check`；
- SQLite `integrity_check`。

重要结论：

> 验证通过表示“原始捕获中存在的数据没有被导出过程丢失，并且结构化表通过一致性检查”。它不表示 Tracy 客户端一定采集到了程序运行时发生的所有事件。

例如以下情况可能是捕获本身的事实，而不是导出错误：

- `gpu_notes` 为 0；
- `context_switches` 为 0；
- 某个 D3D12 queue 没有出现；
- 某些线程没有命名；
- 某些 zone 没有调用栈；
- 某个 plot 没有采样；
- `end` 为未完成状态，因为捕获在 zone 结束前停止。

---

## 2. 推荐的工作方式

不要直接修改唯一一份已经验证过的数据库。

推荐流程：

1. 保留原始 `.tracy` 文件；
2. 保留第一次验证通过的 `.sqlite` 文件；
3. 将分析 SQL、索引或临时表应用到数据库副本；
4. 每次分析先读取 `meta` 和 `export_audit`；
5. 大表先使用 `COUNT(*)`、时间范围和 `LIMIT`，不要直接在 GUI 中执行“打开整表”。

如果只做读取，可以使用 Python 标准库或任意支持 SQLite 的客户端。

---

## 3. 第一次打开数据库

### 3.1 SQLite 命令行

```powershell
sqlite3 "C:\path\capture.sqlite"
```

进入后建议先执行：

```sql
.headers on
.mode column
.timer on

SELECT * FROM meta ORDER BY 1;
SELECT * FROM export_audit ORDER BY 1;
PRAGMA foreign_key_check;
PRAGMA integrity_check;
```

预期：

- `PRAGMA foreign_key_check` 返回 0 行；
- `PRAGMA integrity_check` 返回 `ok`。

### 3.2 Python 只读连接

```python
from pathlib import Path
import sqlite3

db = Path(r"C:\path\capture.sqlite").resolve()
uri = db.as_uri() + "?mode=ro"

con = sqlite3.connect(uri, uri=True)
con.execute("PRAGMA query_only = ON")

print(con.execute("PRAGMA integrity_check").fetchone())
```

如果数据库仍可能被另一个进程写入，不要使用 SQLite 的 `immutable=1`。

---

## 4. 不依赖文档版本地查看实际 schema

数据库中的 schema 是最终权威来源。分析前应先查看实际表和列，避免将其他 Tracy 版本的字段名套用到当前数据库。

### 4.1 查看所有表

```sql
SELECT name
FROM sqlite_schema
WHERE type = 'table'
ORDER BY name;
```

### 4.2 查看某张表的列

```sql
PRAGMA table_info('threads');
PRAGMA table_info('cpu_zones');
PRAGMA table_info('gpu_contexts');
PRAGMA table_info('gpu_zones');
PRAGMA table_info('memory_events');
PRAGMA table_info('plot_samples');
```

### 4.3 查看建表 SQL 和外键

```sql
SELECT sql
FROM sqlite_schema
WHERE type = 'table' AND name = 'cpu_zones';

PRAGMA foreign_key_list('cpu_zones');
```

### 4.4 用 Python 列出每张表的列和行数

注意：对数千万行的大表执行 `COUNT(*)` 需要时间。

```python
from pathlib import Path
import sqlite3

db = Path(r"C:\path\capture.sqlite").resolve()
con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
con.execute("PRAGMA query_only = ON")

tables = [
    row[0]
    for row in con.execute("""
        SELECT name
        FROM sqlite_schema
        WHERE type = 'table'
        ORDER BY name
    """)
]

for table in tables:
    quoted = '"' + table.replace('"', '""') + '"'
    columns = [
        row[1]
        for row in con.execute(f"PRAGMA table_info({quoted})")
    ]
    count = con.execute(f"SELECT COUNT(*) FROM {quoted}").fetchone()[0]
    print(f"{table}: rows={count:,}")
    print("  " + ", ".join(columns))
```

---

## 5. 主要数据域

以下是日常分析最重要的数据域。完整表名以第 4 节的 schema 查询结果为准。

### 5.1 导出元数据和审计

关键表：

- `meta`
- `export_audit`
- `capture_chunks`

用途：

- 确认 Tracy 版本和 schema 版本；
- 确认原始捕获路径、大小和 SHA-256；
- 判断每一类对象是否完整导出；
- 必要时恢复原始捕获。

这是所有分析的第一站。

### 5.2 帧和时间轴

关键表：

- `frame_sets`
- `frames`

用途：

- 查找主帧、命名 frame set 和连续帧区间；
- 计算帧时间分布；
- 将 CPU、GPU、plot 和内存变化投影到同一个时间范围。

不要默认所有 `frame_sets` 都代表屏幕 Present。应先查看 frame set 名称和样本。

### 5.3 线程和 CPU zone

关键表：

- `threads`
- `cpu_zones`
- `thread_samples`
- `thread_names` 以及其他字符串/源码位置相关表

含义：

- `threads` 是 Tracy 捕获中认识的线程；
- `cpu_zones` 是 `ZoneScoped`、动态 zone 或程序生成的嵌套区间；
- `thread_samples` 是统计采样记录，不等同于 instrumented zone；
- 线程名可能来自程序命名、操作系统信息或 Tracy 内部映射。

分析线程时不要只看 `cpu_zones`：

- zone 表示程序主动插桩的工作；
- context switch 表示线程实际上何时被操作系统调度；
- sample 表示采样时观察到的调用位置。

三者回答的是不同问题。

### 5.4 CPU 调度和核心活动

关键表：

- `context_switches`
- `cpu_context_switches`
- `cpu_topology`
- `tid_pid`
- 与线程/进程映射相关的表

用途：

- 判断线程处于 running、waiting 或被抢占；
- 判断线程在哪个逻辑 CPU 上运行；
- 查找线程迁移、调度空洞和过度抢占；
- 将 Tracy zone 的墙钟时间拆分为“真正运行”和“等待”。

注意：

> `cpu_zones.end - cpu_zones.start` 是墙钟持续时间，不一定都是 CPU 执行时间。线程可能在 zone 中途睡眠、等待锁或被系统抢占。

### 5.5 D3D12/GPU

关键表：

- `gpu_contexts`
- `gpu_zones`
- `gpu_notes`
- GPU context、queue、名称和线程映射相关表

用途：

- 区分不同 Tracy GPU context；
- 区分不同 D3D12 command queue；
- 读取 GPU query 时间戳生成的 zone；
- 对照 CPU 提交区间和 GPU 执行区间；
- 查找 queue 间的重叠和空闲时间。

分析原则：

1. 先按 `gpu_context` 或 queue 分组；
2. 不要把不同 queue 上重叠执行的 zone 时长直接相加；
3. CPU 提交时间不是 GPU 执行时间；
4. 一个 queue 没出现在捕获中，可能表示没有工作、没有创建 Tracy context，或捕获区间内没有成功采集；
5. `gpu_notes = 0` 不代表 `gpu_zones` 丢失。

对于当前项目，应特别确认：

- Direct queue 是否出现；
- Compute queue 是否出现；
- Copy queue 是否出现；
- 每个 queue 的 zone 数量、首尾时间和最长空闲区间；
- Tracy reconnect 前后 context 是否发生切换。

### 5.6 Plot 和 DXGI 统计

关键表：

- `plots`
- `plot_samples`

`plots` 通常很小，可以先完整查看：

```sql
SELECT * FROM plots ORDER BY 1;
```

然后查看列定义：

```sql
PRAGMA table_info('plot_samples');
```

本项目预期可能包含 DXGI 显存/内存预算相关 plot，例如：

- Local budget；
- Local current usage；
- Local available for reservation；
- Local current reservation；
- Non-local budget；
- Non-local current usage；
- Non-local available for reservation；
- Non-local current reservation。

实际名称以 `plots` 表为准，不要依赖上面的描述进行硬编码。

分析时先取得目标 plot 的 ID，再从 `plot_samples` 按时间范围读取。不要一次性加载所有 plot 样本。

### 5.7 内存

关键表：

- `memory_pools`
- `memory_events`
- 内存 active/free 索引和线程映射相关表

内存事件通常包含：

- 分配地址；
- 分配大小；
- 分配时间；
- 释放时间；
- 分配线程；
- 释放线程；
- 分配/释放调用栈；
- 所属内存池。

可回答的问题：

- 捕获结束时仍存活的分配；
- 分配/释放速率；
- 峰值存活字节数；
- 最大单次分配；
- 分配热点调用栈；
- 跨线程释放；
- 生命周期很短但数量巨大的分配。

重要：

- Tracy 内存事件表示程序通过 Tracy 内存接口上报的分配；
- 它不是 Windows 全进程内存的自动完整快照；
- DXGI budget/usage plot 和 Tracy 内存分配事件不是同一个数据源。

### 5.8 调用栈和符号

关键表：

- `callstacks`
- `callstack_items`
- 调用栈 frame 定义/条目相关表
- `symbols`
- symbol map、源码位置和统计相关表

`callstacks` 和 `callstack_items` 可能非常大。通常先从：

- 慢 zone；
- 大内存分配；
- 高频 sample；
- 锁等待；

取得调用栈 ID，再按 ID 查询调用栈条目，而不是从调用栈表开始全库扫描。

### 5.9 锁

关键表：

- `locks`
- `lock_threads`
- `lock_events`

用途：

- 查找竞争最严重的锁；
- 查找持锁时间；
- 区分等待者和持有者；
- 将锁等待投影到 CPU zone 和 context switch 时间轴。

---

## 6. 时间、持续时间和数值类型

### 6.1 时间单位

带 `_ns` 的字段使用纳秒。

常用换算：

```text
微秒 = ns / 1,000
毫秒 = ns / 1,000,000
秒   = ns / 1,000,000,000
```

常规区间持续时间：

```text
duration_ns = end_ns - start_ns
```

需要先排除：

- 未结束区间；
- `end < start` 的无效/特殊状态；
- NULL；
- 捕获边界截断的区间。

### 6.2 整数原始位

Tracy 中有些标识符本质上是无符号 64 位整数，例如：

- 指针；
- thread ID；
- 某些对象引用；
- 源数据中的字符串地址。

SQLite 的 `INTEGER` 是有符号 64 位，因此导出器可能同时保存：

- 便于查询的数值；
- 十六进制文本；
- 原始 8 字节 BLOB。

当精确标识很重要时，以 `*_hex` 或原始字节字段为准，不要将超过 `2^63-1` 的值经过 JavaScript Number 或有符号整数后再比较。

### 6.3 浮点原始位

对于 plot 等浮点数据，导出器可能同时保留：

- SQLite `REAL`；
- 原始 IEEE-754 位模式。

普通性能分析使用 `REAL` 即可；验证 NaN、Infinity、负零或完全逐位一致时使用原始位字段。

### 6.4 NULL、0 和缺失

不要将以下状态混为一谈：

- SQL `NULL`：没有对应值；
- 数值 0：可能是合法 ID、sentinel 或时间起点；
- 0 行：该类对象在捕获中不存在；
- 外键找不到：可能是特殊 sentinel，也可能需要进一步核查。

判断是否丢失应结合：

- `export_audit`；
- 外键检查；
- 该 ID 是否真的被业务表引用；
- 原始捕获校验。

---

## 7. 建议的分析顺序

### 7.1 第一步：确认身份

读取：

```sql
SELECT * FROM meta ORDER BY 1;
```

至少确认：

- Tracy 版本；
- schema 版本；
- 原始捕获大小；
- SHA-256；
- 捕获时间范围。

### 7.2 第二步：确认完整性

读取：

```sql
SELECT * FROM export_audit ORDER BY 1;
PRAGMA foreign_key_check;
PRAGMA integrity_check;
```

也可以查看验证器参数：

```powershell
python .\verify_export.py --help
```

优先使用仓库内与当前 schema 同版本的 `verify_export.py`。

### 7.3 第三步：建立数据清单

首先只统计这些核心表：

```sql
SELECT 'threads' AS table_name, COUNT(*) AS row_count FROM threads
UNION ALL SELECT 'cpu_zones', COUNT(*) FROM cpu_zones
UNION ALL SELECT 'thread_samples', COUNT(*) FROM thread_samples
UNION ALL SELECT 'gpu_contexts', COUNT(*) FROM gpu_contexts
UNION ALL SELECT 'gpu_zones', COUNT(*) FROM gpu_zones
UNION ALL SELECT 'gpu_notes', COUNT(*) FROM gpu_notes
UNION ALL SELECT 'plots', COUNT(*) FROM plots
UNION ALL SELECT 'plot_samples', COUNT(*) FROM plot_samples
UNION ALL SELECT 'memory_pools', COUNT(*) FROM memory_pools
UNION ALL SELECT 'memory_events', COUNT(*) FROM memory_events
UNION ALL SELECT 'context_switches', COUNT(*) FROM context_switches
UNION ALL SELECT 'cpu_context_switches', COUNT(*) FROM cpu_context_switches
UNION ALL SELECT 'callstacks', COUNT(*) FROM callstacks
UNION ALL SELECT 'callstack_items', COUNT(*) FROM callstack_items
UNION ALL SELECT 'symbols', COUNT(*) FROM symbols;
```

### 7.4 第四步：选择时间窗口

不要从全捕获直接做所有关联。

先选择：

- 某一帧；
- 某一个卡顿区间；
- 某个 reconnect 前后；
- 某次显存峰值；
- 某个 GPU queue 空洞。

然后让 CPU、GPU、plot、内存和 context switch 查询都限制在相同时间窗口。

### 7.5 第五步：最后才展开调用栈

调用栈数据量通常最大，应先找到目标对象 ID，再展开调用栈。

---

## 8. 常见性能问题的查询思路

具体字段名以 `PRAGMA table_info(...)` 为准。

### 8.1 最慢 CPU zone

1. 从 `cpu_zones` 计算 `end_ns - start_ns`；
2. 排除未结束/截断项；
3. 关联线程、zone 名称和源码位置；
4. 分别统计：
   - 最大值；
   - P50/P95/P99；
   - 总墙钟时间；
   - 出现次数；
5. 再用 context switch 判断慢是执行还是等待。

### 8.2 帧卡顿

1. 从 `frames` 找最长帧；
2. 建立该帧时间窗口；
3. 查同窗口的 CPU zone；
4. 查每个 GPU queue 的 GPU zone；
5. 查 CPU context switch；
6. 查 DXGI usage/budget plot；
7. 查内存分配突发；
8. 最后展开最可疑对象的调用栈。

### 8.3 GPU queue 空洞

1. 按 `gpu_context`/queue 分组；
2. 按 GPU 开始时间排序；
3. 计算当前 zone 开始与前一个 zone 结束之间的间隔；
4. 将空洞与 CPU 提交、Present、同步和其他 queue 对齐；
5. 区分真正 GPU idle 与 Tracy query 未覆盖的区间。

### 8.4 显存压力

1. 从 `plots` 找 local/non-local usage 和 budget；
2. 计算 `usage / budget`；
3. 找最大值和快速增长区间；
4. 对齐帧时间、资源创建 zone 和内存事件；
5. 注意 DXGI usage 不是单个资源的精确占用清单。

### 8.5 分配抖动

1. 从 `memory_events` 按固定时间桶统计分配次数和字节；
2. 区分峰值存活字节和累计分配字节；
3. 找生命周期很短的分配；
4. 按调用栈聚合；
5. 检查是否跨线程释放。

### 8.6 调度问题

1. 从 `context_switches` 找目标线程；
2. 计算 runnable、running、waiting 区间；
3. 检查 CPU 迁移；
4. 对齐长 CPU zone；
5. 检查是否被更高优先级线程抢占或等待同步。

---

## 9. Tracy UI 颜色不能直接当作数据库语义

Tracy UI 中看到的颜色可能来自不同来源：

- zone 自定义颜色；
-线程颜色；
- context switch 状态；
- CPU core 活动；
-采样/调用栈标记；
- UI 为提高可读性自动生成的颜色。

数据库分析应使用明确字段：

- 时间；
- 线程；
- CPU；
-状态；
- zone/plot/context ID；
-源码位置；
-父子关系。

不要仅根据截图中的颜色推断线程状态。尤其是 CPU 核心轨道上的颜色和 instrumented zone 颜色不是同一层语义。

---

## 10. 大数据库的性能建议

### 10.1 避免

- GUI 中直接“打开”数百万行表；
- 无时间条件地关联 `callstack_items`、`plot_samples` 和 `memory_events`；
- 为了预览而执行全表 `ORDER BY`；
- 在唯一验证副本中创建索引或临时表；
- 使用 pandas 一次性读入整表；
- 把 64 位无符号 ID 转为 JavaScript Number。

### 10.2 推荐

- `LIMIT`；
- 先查 ID，再查详情；
- 按时间范围；
- 使用 SQLite 聚合后只返回小结果集；
- 需要索引时复制数据库；
- Python 使用游标分批读取；
- 对长查询先运行 `EXPLAIN QUERY PLAN`。

分析副本可根据具体查询创建索引，例如：

```sql
-- 仅为示意；实际列名先通过 PRAGMA table_info 确认。
-- CREATE INDEX ... ON cpu_zones(thread_id, start_ns);
-- CREATE INDEX ... ON gpu_zones(context_id, gpu_start_ns);
-- CREATE INDEX ... ON plot_samples(plot_id, time_ns);
```

不要在未确认列名前直接执行上述示意语句。

---

## 11. 交给其他 Codex/AI 会话时的推荐提示词

可以将下面内容与数据库路径一起提供给新的分析会话：

```text
请分析这个由 tracy-sqliteexport 生成的 Tracy 0.13.1 SQLite 数据库。

开始前必须先阅读：
C:\CodeProjects\GodotProjects\tracy-sqlite-export\TRACY_SQLITE_GUIDE.zh-CN.md

分析规则：
1. 首先读取 meta、export_audit 和实际 sqlite_schema。
2. 先执行 foreign_key_check 和 integrity_check。
3. 不要假设字段名；先用 PRAGMA table_info。
4. 不要直接加载整张大表。
5. 所有结论都注明使用的时间范围、线程、GPU context/queue 和行数。
6. 区分墙钟 zone 时长、CPU 实际调度时间和 GPU query 时间。
7. 区分 Tracy 内存事件和 DXGI budget/usage plot。
8. 0 行表示捕获中没有该类对象，除非 export_audit 或引用检查证明导出异常。
9. 不修改唯一数据库；如需索引，先复制。

请先给出：
- 数据库身份和完整性结论；
- 所有核心表行数；
- 线程清单；
- GPU context/queue 清单；
- plot 清单；
- 捕获时间范围；
然后再进行具体性能分析。
```

请将上述示例中的文档路径替换为当前仓库的实际路径。

---

## 12. 当前仓库中的相关文件

- `CMakeLists.txt`：构建 `tracy-sqliteexport`；
- `src/TracySqliteExport.cpp` 或实际 exporter 源文件：schema 和导出实现；
- `verify_export.py`：完整性验证；
- `analyze_export.py` 或实际分析脚本：辅助分析。

文件名以当前目录实际内容为准。如果文档和代码对字段存在冲突，以：

1. 当前数据库中的 `sqlite_schema`；
2. 与该数据库同版本的 exporter；
3. 与该数据库同版本的 verifier；

为最终依据。

