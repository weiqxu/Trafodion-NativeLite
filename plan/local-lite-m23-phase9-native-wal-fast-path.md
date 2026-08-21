# LocalLite M23 Phase 9：原生 WAL 快路径

## 目标

Phase 8 的物理 WAL lane 仍然把每个事务写成外部 RocksDB intent：

```text
intent Put(sync) -> unified TransactionDB Write(sync) -> intent Delete(sync)
```

这条路径增加了额外 WAL 写入和清理，且 8 个 intent lane 并不是 8 个数据分片。
Phase 9 先解决单一 unified TransactionDB 的常见事务路径：使用一个原生
`WriteBatch` 同时承载业务更新和紧凑的 commit envelope，通过一次 unified WAL
提交完成 durable write；Phase 8 intent 路径保留为显式 legacy/fault 回退路径。

## 实现方案

- 默认启用 `native` 模式；`TRAF_LOCAL_LITE_PHYSICAL_WAL_MODE=legacy` 可以回退
  到 Phase 8 intent Put/Delete 语义。
- native 模式把 `txid`、目标 writer shard 和 group 中的事务数量编码为
  `WriteBatch::PutLogData` 元数据。该元数据只进入 unified WAL，不创建永久 KV
  marker，也不需要正常路径删除 intent。
- group commit 仍然先合并业务 Put/Delete，再追加一个 group-level envelope，最后
  用一次同步或异步 unified DB 写入完成提交。
- 旧的 `after-intent`/`after-canonical` fault 注入默认强制 legacy 模式，保持
  Phase 8 恢复测试的含义；native fault 窗口由同样的边界单独验证。
- native 模式使用 `TRAF_LOCAL_LITE_NATIVE_WAL_FAULT=before-wal|after-canonical`
  注入 unified WAL 提交前后的崩溃边界；由于业务数据和 envelope 同批原子提交，
  `before-wal` 发生在真正写入前，`after-canonical` 只能看到完整事务或无事务。
- 当前阶段不改变数据分片、OCC validation、publication 顺序或跨分片 2PC；
  `PutLogData` 不是永久业务状态，后续阶段需要增加 WAL 保留和 `applied_lsn`
  checkpoint 才能用它做长期恢复/复制元数据。

## 验收

- release 构建、M22A/M22B、loader 和现有 Phase 8 fault recovery 测试通过；
- native workload 的 `physical_wal_shards` 仍可观测，但正常事务不产生 pending
  intent；
- legacy 模式的 `after-intent` 和 `after-canonical` 恢复语义不回归；
- native 与 legacy 使用同一同步提交配置进行 TPCC 对照，记录 TPS、publication
  latency、p95、OCC 冲突和 source revision；
- native 模式不降低 50 TPS/10% variance 资格门禁，且相对 Phase 8 不出现正确性、
  checkpoint、restart 或 watermark 回归。

## 提交边界

本阶段只修改 unified WAL commit API、metrics 暴露和本计划文档，提交信息说明
native envelope、legacy fallback 及其恢复边界。

## 当前实现验证

- `make local-lite-release -j2`：通过；
- `scripts/test-local-lite-m22a.sh`：通过；
- `scripts/test-local-lite-m22b.sh`：通过；
- `scripts/test-local-lite-tpcc-loader.sh`：在允许本机 TCP 后通过 smoke；
- 固定 M22 完整 TPCC 已启动并完成 Warehouse/Customer/History/Orders/New-Order
  装载，但在 Stock 大表装载阶段未继续产生进度，未将该次运行计入吞吐证据；
- 默认 reduced TPCC 在沙箱内无法启动 TCP，未把失败的 reduced 结果作为 native
  性能结论。正式性能结论需要使用 release + `m22-full.properties` 的完整报告。
