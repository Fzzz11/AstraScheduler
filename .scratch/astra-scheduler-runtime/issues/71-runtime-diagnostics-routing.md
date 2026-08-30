# AST-071 — 抽出 Runtime wait/await diagnostics 路由

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-127, R-096)
Milestone: v1.2.0
Blocked by: AST-069, AST-070
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-127 [primary] — Runtime lookup 与 diagnostic routing 从 Scheduler 实现中分离；source: D-177
- R-096 [supporting] — Wait/Await 事件、计数和直方图语义保持不变；source: D-050, D-051, D-149

## Current gap

`src/runtime/scheduler.cpp` 已不再拥有 Worker loop 和 Runtime registry map，但仍直接定义
`WaitDiagnosticsGuard`、Trace wait event 路由以及多组 wait/await Metrics hook，并通过
`Scheduler::Impl` 字段读取 metrics/trace。该部分应继续留在 private runtime 组合层，不能成为
public Scheduler facade 与 observability 逻辑之间的隐式耦合。

## What to build

在 `src/observability/runtime_diagnostics.{hpp,cpp}` 建立非安装 diagnostics 模块：

- 通过窄的、non-owning Runtime diagnostics port 或 registry handle 读取 metrics/trace 能力；
- 迁移 WaitBegin/WaitEnd、AwaitArmed/Triggered/Resumed、unobserved failure 路由和 scope guard；
- 保留 `perform_caller_wait` / `perform_graph_caller_wait` 的等待与 helping 语义，不在本 Ticket 改调度算法；
- 保留 Metrics Off/Trace disabled fast path、source/target logical identity、timeout 与 histogram 口径；
- private diagnostics headers 不进入安装 manifest，Scheduler 只保留等待协议入口和委托。

## Invariants

- `[R-127]` diagnostics 模块不得包含或保存 `Scheduler::Impl*`，不得访问 Scheduler 字段布局或延长 Runtime 生命周期。
- `[R-096]` 不建立在线 wait-for graph，不改变 self/depth rejection、same/cross-runtime helping、timeout 或 Trace schema 语义。
- Diagnostics 失败或 Runtime 注销不能阻止任务/Graph 完成、Reaper join 或 shutdown。

## Acceptance criteria

- [ ] `WaitDiagnosticsGuard`、wait Trace event 路由和 diagnostics Metrics hooks 不再定义在 `src/runtime/scheduler.cpp`。
- [ ] 新模块通过窄 seam 获取 Runtime 能力，不持有 `Scheduler::Impl*`；新增静态审计覆盖该边界。
- [ ] `tests/test_wait_await_diagnostics.cpp`、GraphRun wait、Coroutine await、unobserved failure 行为不变。
- [ ] Debug、ASan/UBSan、TSan、package/encapsulation gates 通过，public API 与安装清单无变化。

## Out of scope

- 不拆 `ReadyQueues`、Worker helping claim 算法或 `RuntimeState`。
- 不新增 public diagnostics API，不修改 Metrics/Trace schema、事件编号或采样语义。
- 不把 diagnostics 变成拥有 Runtime 生命周期的服务对象。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-127, R-096
- Decision: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-177, D-050, D-051, D-149
- Verification: pending；实现后补充 WSL Debug/ASan/TSan、package 与 encapsulation evidence。
