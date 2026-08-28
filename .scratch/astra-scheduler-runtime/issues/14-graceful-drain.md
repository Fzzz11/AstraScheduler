# AST-014 — 实现 Graceful admission closure 与 Drain Work Closure

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-006, R-007, R-012, R-019)
Milestone: v0.1.0
Blocked by: AST-010, AST-011
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-006 [primary] — Graceful Stopping 接受授权的 Internal Submission；source: D-002
- R-007 [primary] — Graceful 转换线性化关闭 External Submission；source: D-003
- R-012 [primary] — 非 Worker graceful shutdown 排空传递闭包；source: D-010
- R-019 [primary] — Stopped 是关停吸收状态；source: D-016

## What to build

线性化关闭 External Submission，继续接受获授权 Internal Submission，排空传递闭包并把 Stopped 作为吸收状态。

## Invariants

- `[R-006]` Graceful Stopping 期间，Runtime 必须继续接受由同一 Scheduler 当前正在执行的已接受任务发起的 Internal Submission，并把这些任务纳入同一次 Drain Work Closure。 例外边界：其他线程和其他 Scheduler Worker 的提交属于 External Submission；Immediate Stopping 不适用。
- `[R-007]` External Submission 与 `Running → Stopping` 必须形成单一线性化顺序；转换前线性化的提交被接受并计入 Drain Work Closure，转换后线性化的提交被拒绝且不得入队或增加 outstanding work。 例外边界：R-006 授权的 Internal Submission。
- `[R-012]` 非目标 Scheduler Worker 调用 `shutdown()` 时，方法必须等待整个 Drain Work Closure 终结、全部 Worker 退出并 join、`Stopped` 发布后返回；闭包无法终结时可无限阻塞。 例外边界：目标 Scheduler Worker 由 R-013 覆盖。
- `[R-019]` `Stopped` 发布后，任何线程调用 `shutdown()` 或 `shutdown_now()` 必须成功且立即无副作用返回，不得创建新 Shutdown Completion、改变历史 Shutdown Mode、重复 join、重启 Runtime 或改写任务终态。

## Test-first seam

- Public seam: 同 Scheduler Worker 在 Graceful Stopping 中的任务派生。；External Submission 与 Graceful Shutdown 的竞态。；普通应用线程以及其他 Scheduler 的 Worker对目标 Scheduler 的调用。；已 Stopped 的 Scheduler Runtime。
- RED evidence: 先写关停边界前后 external/internal 提交、递归 internal fan-out、重复 shutdown 和 Stopped 后操作测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-006]` 已接受父任务在 Graceful Stopping 中提交的子任务得到正常终态，Shutdown Completion 晚于该子任务终结。
- [x] `[R-007]` 每个竞态提交恰好落在 accepted 或 rejected 一侧，不产生孤儿任务或提前关停。
- [x] `[R-012]` 关停不会以队列瞬时为空提前返回，返回后所有 Worker 已 join。
- [x] `[R-019]` 两种关停 API 在 Graceful/Immediate 完成后均稳定立即返回且状态不变。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-006, R-007, R-012, R-019
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-002, D-003, D-010, D-016
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-006 | `tests/test_graceful_drain.cpp::test_R006_R012_graceful_drain_internal_closure` — 证明在 Graceful Stopping 期间，由已执行 Worker 派生的 Internal 子任务与递归孙子任务均被正确接纳并执行至终结。 | 运行期 RED：Stopping 状态下一律拒绝所有提交（包含 internal）。 | 获授权的 Internal 提交成功入队并产出预期结果。 |
| R-007 | `tests/test_graceful_drain.cpp::test_R007_external_submission_rejected_after_stopping` — 证明 `Running → Stopping` 转换后外部提交严格被拒绝（抛出 `submission_rejected(Stopping)`），无遗漏。 | 编译期/运行期 RED：状态转换后外部提交仍被接收。 | 外部提交线性化抛出 `submission_rejected(Stopping)`。 |
| R-012 | `tests/test_graceful_drain.cpp::test_R006_R012_graceful_drain_internal_closure` — 证明非 Worker 调用 `shutdown()` 等待整个 Drain Work Closure 结束且所有 Worker 退出 join 后返回，最终发布 `Stopped(Graceful)`。 | 运行期 RED：shutdown() 未等待传递闭包或瞬时空队列提前返回。 | 闭包排空后同步返回，所有 worker 线程已 join。 |
| R-019 | `tests/test_graceful_drain.cpp::test_R019_stopped_is_absorbing_state` — 证明 `Stopped` 状态作为吸收状态，多次调用 `shutdown()` / `shutdown_now()` 立即无副作用返回，且历史模式不被改写。 | 运行期 RED：Stopped 状态下调用 shutdown 重复执行清理或破坏已有状态。 | 幂等无副作用直接返回，历史模式稳定保持。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 29 tests in 40.308s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.259s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 12`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `a79e57b`）：
  - Standards 轴：`active_task_count` 与 `global_injection_queue` 组合形成精确的 Drain Work Closure 判定；`shutdown()` / `shutdown_now()` API 规范简洁；公共符号导出完备且无内部泄漏。
  - Spec 轴：R-006（Internal 授权与闭包纳入）、R-007（External 线性化关闭）、R-012（非 Worker 同步等待排空与 Worker join）、R-019（Stopped 吸收状态与幂等无副作用）100% 满足 Approved Spec。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

