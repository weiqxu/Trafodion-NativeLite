# Lite M23 性能提升计划

## 目标

M22H 的固定 TPCC 基线为 10 warehouses、32 terminals、45/40/5/5/5
事务比例、40 warmup + 200 measured/terminal、3 次 repetition。当前
Lite 为 55.853 TPS，方差 0.81%；同环境 MySQL 为 202.802 TPS，但
MySQL 方差 17.7%，因此不能把 MySQL 结果直接当作正式资格线。

M23 的第一目标是 Lite 达到 **100 TPS、方差不超过 10%**，同时保持
durable write、事务一致性和恢复门禁。130 TPS 是延伸目标。所有结果必须
保留事务 mix、连接数、warmup、数据规模和 source revision。

## 瓶颈排序

1. OCC 冲突和客户端重试：当前 20,480 次提交对应 1,063 次服务端冲突和
   997 次客户端可见重试，首先影响 Payment 等共享键事务的尾延迟。
2. publication/durable write：提交需要构造 WriteBatch、执行同步 RocksDB
   写入并发布可见性；同步写不能为了 TPS 门禁改成异步确认。
3. T4、编译和执行路径：当前部分编译、计划缓存和 literal specialization
   指标不可见，New-Order 的拆分执行可能放大端到端延迟。
4. Stock Level：当前已有 range-to-point 优化，不能以牺牲其收益换取其它
   事务的平均 TPS。

## 阶段和提交边界

### 阶段 1：观测能力

增加 T4 decode/dispatch、compile mutex wait、计划缓存命中率、literal
specialization、Executor、OCC 冲突分类、publication 子阶段、WAL fsync、
write amplification、compaction/write stall 等指标。先用固定 workload
重跑，产出可对比的延迟分解报告。

提交：`perf: add tpcc latency breakdown telemetry`

验收：指标可在 workload JSON 中读取；原有 M22 事务、一致性和恢复测试不
回归；缺失指标必须显式标记 unavailable，不能填 0。

### 阶段 2：OCC 冲突治理

缩小事务读集，减少共享键的无效验证；按 warehouse/shard 组织冲突历史和
commit intent；优化有界退避和重试统计。不能绕过 validation，也不能把
冲突改报成成功。

提交：`perf: reduce tpcc occ conflicts and retries`

验收：冲突下降至少 60%，客户端重试少于 300，TPS 至少 75，一致性、恢复、
checkpoint、watermark 全通过。

### 阶段 3：publication 和 durable write

按 shard 引入 group commit，合并同一提交窗口的 WriteBatch，减少重复索引
和历史写入；只有同步 WAL/storage 完成后才确认事务提交。异步写只允许做
诊断对照，不得用于正式资格结果。

提交：`perf: optimize durable publication with group commit`

验收：publication 累计耗时下降至少 40%，提交等待 p95 下降至少 50%，TPS
至少 90，并保持 crash/recovery 语义。

### 阶段 4：计划和执行路径

使用参数化、规范化 SQL 提高 plan-cache 命中率；减少编译锁竞争；让
New-Order 走完整 mutation batch；在不破坏 session-affine 所有权的前提下，
并行独立的 Payment/OrderStatus 点查。

提交：`perf: optimize tpcc plan cache and execution path`

验收：plan-cache 命中率至少 95%，compile mutex wait 小于总耗时 5%，
New-Order/Payment p95 下降至少 40%，TPS 至少 110。

### 阶段 5：综合验收

在同一机器和同一数据规模下重跑 Lite 与 MySQL，固定 warmup、重复次数、
事务 mix 和报告格式；执行完整正确性、恢复和资源门禁。

提交：`test: qualify m23 tpcc performance`

验收：Lite TPS 至少 100、方差不超过 10%、重试率不超过 1%，核心事务
p95 不高于 M22 基线的 60%，所有 durability/consistency/recovery 门禁通过。

## 共同约束

- 每个阶段只提交本阶段相关文件；提交前执行 `git diff --check`。
- 每个阶段单独重跑固定 TPCC workload，保留 JSON、日志和 source revision。
- 不使用 `sync=false` 作为正式性能结果，不绕过 session-affine 执行上下文。
- 任一阶段若正确性或恢复门禁失败，停止性能优化并回退该阶段提交。
