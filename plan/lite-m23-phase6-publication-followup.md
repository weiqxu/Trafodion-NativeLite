# Trafodion Lite M23 后续阶段：publication group commit 解串行化

## 范围

release TPCC 基线显示同步 publication 是当前主要耗时来源。现有 group
commit 在 `GroupCommitState` mutex 内执行物理 RocksDB 写入，导致其它提交
不能在写入期间排队。本阶段只调整 group commit 的锁范围，不关闭同步 WAL，
不改变 OCC 校验、可见性发布或恢复语义。

## 实现方案

- leader 负责从 pending 队列取出一个批次并串行调用 `rocksdb_write`；
- 物理写入前释放 group mutex，其他事务可以并发入队；
- 物理写入完成后只在 mutex 内标记 request 完成并唤醒等待者；
- 如果写入期间有新请求入队，leader 继续处理下一批；
- group commit 窗口仍由 `TRAF_LITE_GROUP_COMMIT_WINDOW_US` 控制，默认
  0，不使用异步提交作为正式结果。

## 验收

- release TPCC：10 warehouse、32 terminals、45/40/5/5/5、40 warmup、
  200 measured、3 repetitions；
- throughput、variance、p95、OCC、consistency、checkpoint、clean/unclean
  recovery 全部记录；
- 任一 durable/recovery 门禁失败则不接受本阶段。

## release 验证结果（2026-08-21）

source revision：`9a6c726a7f3284a239500a8c83ade1b8a2839c6c`。

- 默认窗口 `0us`：repetition TPS `60.345 / 60.698 / 61.796`，平均
  `60.940 TPS`，方差 `2.404%`；
- 对照窗口 `500us`：repetition TPS `60.497 / 60.288 / 62.331`，平均
  `61.025 TPS`，方差 `3.389%`；
- 两轮均通过一致性、checkpoint、clean restart、unclean restart 和
  checkpoint restore；publication failure 为 0；
- 两轮吞吐均低于上一版 release 基线 `62.274 TPS`，本阶段不能宣称 TPS
  提升。解串行化只保留为后续分片 durable writer 的安全基础，默认窗口仍为
  `0us`。

原始报告：

- `/tmp/traf-lite-m23-phase6-report/workload-report.json`
- `/tmp/traf-lite-m23-phase6-500-report/workload-report.json`
