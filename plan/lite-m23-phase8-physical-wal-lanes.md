# Lite M23 Phase 8：物理 WAL lane 拆分

## 目标

把 Phase 7 的队列分片推进到真实的物理 durable WAL 路径：8 个 durable-writer
shard 各自打开独立的 RocksDB WAL 数据库，事务提交先以同步 intent 写入对应
`transactiondb-wal/shard-N`，再写统一 TransactionDB，并在成功后清理 intent。

每个 intent 携带统一数据批次和 `m23/commit/<id>` marker。进程异常退出后，启动
阶段扫描各 shard 的 pending intent：若统一库已有 marker，只清理 intent；否则
重放完整 WriteBatch，再清理 intent。这样不会把一个事务拆成多个数据提交，当前
统一 TransactionDB 仍是数据的原子提交点。

## 实现边界

- `LiteUnifiedWriteBatchCommit` 为每个 batch 分配唯一 WAL id；group commit
  leader 将带 marker 的序列化 WriteBatch 写入目标 shard WAL；
- synchronous commit 使用同步 WAL intent、同步统一库写入和同步 intent 清理；
- `TRAF_LITE_PHYSICAL_WAL_FAULT=after-intent|after-canonical` 用于验证
  两个崩溃窗口的启动恢复；
- 仍未把数据 DB 拆成多个 TransactionDB。跨 shard 的真正独立数据提交和 2PC
  需要下一阶段，否则会破坏当前 OCC 快照与跨表事务原子性；
- metrics 增加 `physical_wal_shards`，同时保留 `durable_writer_shards`。

## 验收

- release 构建与 M22A telemetry 合约通过；
- 新建 store 后出现 8 个 `transactiondb-wal/shard-N` 目录；
- 正常 workload 后 pending intent 为空；
- `after-intent` 和 `after-canonical` 故障后重启，consistency、clean/unclean
  restart、checkpoint restore 全部通过；
- release TPCC 的吞吐、方差、OCC hotspot 和 publication failure 记录在本文。

## Release 验证记录

使用 10 warehouse/32 terminal、64 workers、同步提交、`GROUP_COMMIT_WINDOW_US=0`
运行 release TPCC，源码 revision 为 `fd89cf30d11924fa62422e265f265036a8074264`。

- 吞吐 `57.703 TPS`；三次 repetition 为 `57.473/58.503/57.151 TPS`，方差
  `2.3657%`；50 TPS 和 10% 方差 gate 通过；相对 Phase 7 的 `59.467 TPS`
  下降约 `3.0%`，相对 `62.274 TPS` 基线下降约 `7.3%`；
- workload 进程内报告 `durable_writer_shards=8`、`physical_wal_shards=8`、
  `physical_commits=20480`、`publication_failures=0`，最大物理提交重叠为 `11`；
- `publication_latency_us=540428815`，较 Phase 7 明显增加，确认同步 intent 写入
  和清理的额外 fsync 是当前成本；
- server OCC conflicts `1501`，client observed/retried `1420`；Top hotspot 最高
  计数 `432/357/101`，集中在同一 `object_uid=7676155049663573095`；
- New-Order/Payment/Order-Status/Delivery/Stock-Level p95 为
  `743910/488007/351976/1248124/487999 us`，均通过阶段 gate；
- consistency、online checkpoint、clean restart、unclean restart、checkpoint
  restore 和 disk watermark 全部通过。

loader 的独立进程在退出前会释放 unified handles，因此其装载阶段 metrics 可能
显示 `physical_wal_shards=0`；最终 workload 进程内指标确认 8 个物理 WAL lane
均已打开并参与提交。
