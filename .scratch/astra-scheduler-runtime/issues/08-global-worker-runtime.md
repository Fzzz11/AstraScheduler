# AST-008 — 交付 Global-only Worker Runtime 基线

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-001, R-002)
Milestone: v0.1.0
Blocked by: AST-004, AST-005
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-001 [primary] — v0.1.0 使用全局注入队列基线；source: D-001
- R-002 [primary] — v0.1.0 排除本地队列与任务窃取；source: D-001

## What to build

实现 mutex 保护的 Global Injection Queue、固定 Worker 集合和基本执行循环；所有 Ready Task 只走 Global 路径。

## Invariants

- `[R-001]` v0.1.0 的全部 Ready Task 必须通过互斥保护的 Global Injection Queue 调度。
- `[R-002]` v0.1.0 不得包含 Per-Worker Local Queue、Work Stealing 或 Chase-Lev Deque。 例外边界：文档、接口 seam 或后续版本预留不构成 v0.1.0 功能完成。

## Test-first seam

- Public seam: v0.1.0 Basic Scheduler。；v0.1.0 release scope。
- RED evidence: 先用可观察 queue seam 证明 external/internal/worker-published Ready 都进入 Global，且构建中没有 local push/pop/steal 路径。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-001]` v0.1.0 的调度路径中不存在 Ready Task 绕过 Global Injection Queue 的本地队列路径。
- [x] `[R-002]` v0.1.0 构建和测试不执行本地 push/pop/steal 或 Chase-Lev 算法。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-001, R-002
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-001
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-001 | `tests/test_global_worker_runtime.cpp::test_R001_global_injection_queue_baseline`、`test_R001_concurrent_external_submissions`、`test_R001_nested_task_submission` — 证明外部多线程提交与 Worker 内部嵌套提交的所有 Ready 任务均统一进入互斥保护的 Global Injection Queue 并按严格 FIFO 顺序调度执行。 | 运行期 RED：LIFO 执行顺序导致 `executed_order[i] == i` 断言失败。 | `test_R001_*` 测试全过，FIFO 调度与并发提交正确无误。 |
| R-002 | `tests/test_global_worker_runtime.cpp::test_R002_exclude_local_queues_and_work_stealing`、`tools/check_cmake_package.py::AST008GlobalWorkerRuntimeGates` — 证明 v0.1.0 架构严格排除 Per-Worker Local Queue 与 Work Stealing，Capabilities 报告 `local_deque_backend == None` 且 `lock_free_local_deque == false`，无 local push/pop/steal 执行路径。 | 运行期 RED：未建立全局队列基线隔离。 | 能力冻结快照正确报告 None，package 门禁 23 项全部通过。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 23 tests in 17.600s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.182s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 6`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `3019d3d8d31e10ceb97e311d3f4817bf4e9df57b`）：
  - Standards 轴：`Global Injection Queue` 采用 `std::deque<std::function<void()>>` 实现严格 FIFO 调度；多线程入队与 Worker 出队均受 `lifecycle_mutex` 互斥保护；调度器通过 `std::condition_variable` 唤醒空闲 Worker；符号控制保持私有隐藏（`ASTRA_NO_EXPORT`）。
  - Spec 轴：R-001（全部 Ready Task 走互斥 Global Injection Queue）、R-002（严格排除 Local Queue / Work Stealing / Chase-Lev）100% 满足 Approved Spec 及 D-001 决策。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。
 
