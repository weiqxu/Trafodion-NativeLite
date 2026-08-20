# LocalLite M23 后续阶段：分片 durable writer 与 OCC 热点统计

## 目标

publication 当前由单一提交队列承载。本阶段按物理 WriteBatch 的稳定 hash
选择 durable-writer shard；一个事务的完整 batch 只进入一个 shard，不能拆分
跨 shard，从而保持事务原子性。每个 shard 使用独立的 pending/processing/cond
状态，底层仍通过同一个 RocksDB TransactionDB/WAL 完成同步 durable write。

同时记录 OCC 实际相交的 `(object_uid, write_key)`，保留有界热点集合并在
`occ-metrics` JSON 中输出按冲突次数排序的 Top-N，避免只知道 point conflict
而不知道具体共享行。

## 实现边界

- 默认 8 个 durable-writer shard，可由 `TRAF_LOCAL_LITE_DURABLE_SHARDS` 调低
  做对照；不会把一个事务的 WriteBatch 拆成多个物理写入；
- 每个物理写仍使用同步 WriteOptions，commit 成功只在 `rocksdb_write` 成功后
  返回；
- OCC hotspot 只记录真实 intersect 的写 key，最多保留 256 个候选，报告 Top 16；
- 不改变 OCC validation、可见性发布、checkpoint 或恢复协议。

## 验收

- release 构建通过；
- 固定 10 warehouse/32 terminal TPCC 运行后报告
  `durable_writer_shards` 和 `occ_conflict_hotspots`；
- consistency、publication failure、checkpoint、clean/unclean restart 和
  checkpoint restore 全部通过。

## Release 验证记录

验证命令使用 `LOCAL_LITE_BUILD_TYPE=release`、10 warehouse/32 terminal、64
workers、同步提交和 `TRAF_LOCAL_LITE_GROUP_COMMIT_WINDOW_US=0`，源码 revision
为 `ca39c4435887c43517d02cec4cb786eee8d607fe`。

- workload 吞吐 `59.467 TPS`；三次 repetition 为 `59.069/60.869/58.513 TPS`，
  方差 `4.0251%`；50 TPS 和 10% 方差 gate 通过，但相对前一版 `62.274 TPS`
  下降约 `4.5%`，说明队列分片尚未消除共享 TransactionDB/WAL 的物理写入瓶颈；
- server 报告 `durable_writer_shards=8`、`physical_commits=20480`、
  `publication_failures=0`、`maximum_physical_commit_overlap=7`，累计 OCC
  冲突 `1341`（client observed/retried `1275`）；Top hotspot 为同一
  `object_uid=7676147237129277799` 下的 key，最高计数 `341/340`，完整列表在
  workload report 的 `occ_conflict_hotspots` 字段；
- New-Order/Payment/Order-Status/Delivery/Stock-Level p95 分别为
  `724143/466930/347974/1247910/472029 us`，均低于阶段 gate；
- consistency、online checkpoint、clean restart、unclean restart、
  checkpoint restore 和 disk watermark gate 全部通过。

该实现是“分片 durable-writer 队列 + 共享 TransactionDB/WAL”：事务 batch 不跨
队列拆分，保留原子性；若要进一步获得真正的多 WAL 并行度，需要下一阶段把物理
存储拆成每 shard 独立 TransactionDB，并设计跨 shard 事务提交协议，本阶段未越过
该边界。
