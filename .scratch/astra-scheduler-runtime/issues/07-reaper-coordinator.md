# AST-007 — 实现唯一 Reaper coordinator 的 pending/join/idle 循环

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-025, R-026, R-028, R-107)
Milestone: v0.1.0
Blocked by: AST-006
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-025 [primary] — Pending Runtime 不阻塞 Reaper；source: D-020
- R-026 [primary] — Join Ready 后唯一 join 并发布 Stopped；source: D-020
- R-028 [primary] — Reaper 空闲时保持同一服务；source: D-022
- R-107 [primary] — Supported Configuration 只有一个实现实例与Reaper coordinator；source: D-021, D-159

## What to build

建立单 implementation instance 下的唯一 coordinator；Pending Runtime 不阻塞其他回收，只在 Join Ready 后唯一 join、发布 Stopped，空闲不重启服务。

## Invariants

- `[R-025]` Reaper 可以持有 Pending Runtime State，但不得阻塞等待其任务或 Drain Work Closure；永久 Pending Runtime 不得阻塞其他 Join Ready Runtime 的回收。
- `[R-026]` 仅当 Runtime 单调进入 Join Ready 后，Reaper 才能认领 join；Reaper 与同步关停路径之间只能有一个 join owner，且 `Stopped`/Shutdown Completion 只能在全部 Worker 实际 join 后发布。
- `[R-028]` Reaper Service 首次成功建立后必须在空闲时阻塞等待并保持同一 coordinator，不得因最后一个 Runtime 消失、队列为空或空闲超时而自动停止或重建。例外边界：显式 Reaper Finalization 由 approved Spec 中对应的 active Finalization rules 覆盖。
- `[R-107]` R-111定义的Linux-only Supported Configuration中，一个进程必须只加载一个Astra Implementation Instance，并由其中一个逻辑Reaper Service和恰好一条不属于任何Scheduler的专用coordinator服务全部Runtime；不得按Scheduler/handoff增加Reaper线程，coordinator不得执行用户任务或参与steal；多个DSO各自静态嵌入实现不享有这些process-wide保证。 例外边界：从未建立服务且空集合Finalization不创建coordinator。

## Test-first seam

- Public seam: Reaper 同时核算一个或多个 orphan Runtime State。；Runtime 回收和 join ownership 竞态。；RegistrationOpen 阶段的进程级 Reaper Service。；process-wide Reaper、Finalization gate、ID allocator与Process Metrics拓扑。
- RED evidence: 用两个受控 Runtime 证明一个长期 Pending 不阻塞另一个 Join Ready，且并发 handoff 只有一次 join/coordinator。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-025]` 一个永久任务所在 Runtime 长期 Pending 时，其他 Runtime 仍能 join 并发布 Stopped。
- [x] `[R-026]` Join Ready 本身不会提前满足完成，Worker 也不会等待 Reaper 先 join 而形成循环等待。
- [x] `[R-028]` 多轮 Scheduler 创建/销毁复用同一休眠 coordinator，空闲时不 busy-spin 或强持有已完成 Runtime。
- [x] `[R-107]` 支持配置中Scheduler数量不增加coordinator数，unsupported duplicate instance被部署文档/测试明确拒绝。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-025, R-026, R-028, R-107
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-020, D-022, D-021, D-159
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-025 | `tests/test_reaper_coordinator.cpp::test_R025_pending_runtime_does_not_block_reaper` — 构造长期处于 Pending 状态的 Runtime A 与快速退出的 Runtime B，验证 Reaper coordinator 不对 Pending Runtime 执行阻塞等待，Runtime B 顺利完成 join、发布 Stopped 并注销；待 A 解除阻塞后 A 也顺利回收。 | 运行期 RED：若对 Pending Runtime 执行阻塞等待，Runtime B 回收将被阻塞超时失败。 | `test_R025_*` 通过，Head-of-Line 隔离完全成立。 |
| R-026 | `tests/test_reaper_coordinator.cpp::test_R026_join_ready_unique_join_and_stopped_publication` — 证明仅当全部 Worker 退出工作循环后单调进入 Join Ready，Reaper coordinator 才认领唯一 join 并发布 Stopped。 | 编译期 RED：未建立单调 Join Ready 与唯一 join 仲裁。 | `test_R026_*` 通过，Worker 无等待循环，Stopped 发布时序正确。 |
| R-028 | `tests/test_reaper_coordinator.cpp::test_R028_reaper_idle_service_persistence` — 证明多轮连续创建和销毁 Scheduler 时，Reaper coordinator 线程在空闲时阻塞等待并保持同一服务，不发生线程停启颠簸（thread count 恒为 1）。 | 编译期 RED：`coordinator_thread_count` 未提供。 | 3 轮生命周期后 coordinator 保持同一休眠线程，`registered_count == 0`。 |
| R-107 | `tests/test_reaper_coordinator.cpp::test_R107_single_coordinator_thread_topology`、`tools/check_cmake_package.py::AST007ReaperCoordinatorGates` — 证明单 Implementation Instance 拓扑下无论创建多少个并发 Scheduler 实例，全局 Dedicated Reaper coordinator 线程数恰好为 1，不为单个 Scheduler 或 handoff 增加 Reaper 线程。 | 编译期 RED：多 Scheduler 并发时 coordinator 拓扑未收拢。 | 并发 4 个 Scheduler 下 `coordinator_thread_count() == 1`，package 门禁 22 项全过。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 22 tests in 15.047s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.190s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 5`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `1a1aa7d0441f4a7e2db4bab9b9283253cb75d6bc`）：
  - Standards 轴：`ReaperRegistry` 的专用协调线程使用 `std::condition_variable` 空闲休眠等待，绝无 busy-spin 或轮询；所有锁保护均遵循最小临界区，在释放 `mutex_` 后执行 join 与资源释放，防止 head-of-line 锁竞争；无对外泄漏私有符号（`ASTRA_NO_EXPORT`）。
  - Spec 轴：R-025（Pending Runtime 不阻塞 Reaper）、R-026（Join Ready 后认领唯一 join 并发布 Stopped）、R-028（空闲保持同一 coordinator 服务）、R-107（Supported Configuration 恰好一条 Dedicated Reaper coordinator 线程）100% 满足 Approved Spec 及 D-020, D-021, D-022, D-159 决策。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。
