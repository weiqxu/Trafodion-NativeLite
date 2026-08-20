# M23 阶段 5 综合验收记录

## 执行信息

- source revision：`e879af403`
- workload：10 warehouses、32 terminals、45/40/5/5/5 mix
- warmup：40/terminal
- measured：200/terminal
- repetitions：3
- durable commit：同步写
- 原始临时产物：`/tmp/traf-local-lite-tpcc-performance.GfymVy`

## 已通过的部分

- Java T4 workload 编译通过。
- NativeLite debug 全量构建通过。
- M14C 五类事务、回滚、断连、重复键和一致性测试通过。
- 10 warehouse native loader 完成，8,990,118 keys 的加载报告显示
  517 个同步提交、0 个 publication failure；关系校验通过。

## 未通过的资格结果

本次显式启用 `TRAF_LOCAL_LITE_GROUP_COMMIT_WINDOW_US=500`，完整测量输出：

```text
measured repetition TPS: [31.7702365, 26.5370316, 33.5299251]
throughput variance: 0.2635145 (26.35%)
gate: max variance 0.10 -> FAIL
```

结果表明当前自研 group commit 在完整 TPCC 负载下把物理写入路径过度串行化，
不能作为正式性能默认值。阶段 5 将默认窗口改为 `0`，并将
`performance.retry.backoff.jitter.millis` 恢复为 `0`；后续需要先重做按 shard
的无全局锁 group commit，再重新申请资格测试。

本记录不是 M23 生产准入证明。M23 仍需在默认配置下重新完成三次稳定测量，
达到 TPS、方差、p95、重试率和恢复门禁后才能标记为通过。
