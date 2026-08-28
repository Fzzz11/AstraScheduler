# AST-010 — 实现 External Pending Capacity 与强 admission transaction

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-061, R-062)
Milestone: v0.1.0
Blocked by: AST-009
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-061 [primary] — External Pending Capacity 与 backpressure 固定；source: D-083, D-084, D-085, D-086
- R-062 [primary] — submit/try_submit 共享强 admission transaction；source: D-087, D-088, D-089, D-155

## What to build

为 External Submission 实现 pending 配额、`submit` backpressure 和 `try_submit` 非阻塞失败；二者共享一次性强事务并正确回滚资源与配额。

## Invariants

- `[R-061]` 每Runtime必须以正数 `external_pending_capacity` 限制已接受但未首次Running的External工作，默认65536；slot在admission占用并在首次start或start前Terminal释放，Internal不占用；容量策略仅为默认Reject或Block，Block只允许普通非Worker且必须在slot/gate竞态下无丢唤醒，CallerRuns不得提供。 例外边界：try_submit永不等待；started后Coroutine suspension不重新占slot；Block等待者不保证FIFO或公平顺序。
- `[R-062]` `submit`与`try_submit`必须在同一强异常安全事务中完成gate、slot、capture/TCB、ID、outstanding与不可丢失publication；成功返回真实TaskHandle，失败完全回滚且不执行Callable；最终 `SubmissionError` 仅为 Stopping/Stopped/CapacityExhausted，submit抛`submission_rejected`，try_submit即时返回variant alternative，其他构造/分配异常保持原类型。 例外边界：空Scheduler为logic_error，Finalization启动拒绝由R-097的creation error表达。

## Test-first seam

- Public seam: 普通Task与R-070的External Graph admission。；Callable与Coroutine spawn的Runtime admission。
- RED evidence: 注入容量耗尽、分配失败、close 竞态，先验证失败不留下 Task identity、调度引用或容量泄漏。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-061]` External未启动工作有界，Worker不会因Block自锁，关闭gate能唤醒并拒绝等待者。
- [x] `[R-062]` 不存在orphan Handle、泄漏slot/outstanding或已拒绝却执行的Callable。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-061, R-062
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-083, D-084, D-085, D-086, D-087, D-088, D-089, D-155
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-061 | `tests/test_admission_backpressure.cpp::test_R061_*` — 证明 `external_pending_capacity` 有效限制外部 pending 工作量，Reject 策略下容量耗尽立即抛出 `submission_rejected(CapacityExhausted)`（`try_submit` 返回错误变体）；Block 策略下普通线程安全阻塞并在 slot 释放或 Shutdown 唤醒时以 lifecycle rejection 结束；跨 Runtime Worker 提交满容量 Runtime 绝不阻塞自锁；同 Runtime Worker 的 Internal Submission 豁免 slot 限制。 | 编译期 RED：未定义背压与配额逻辑。 | Reject/Block/跨 Runtime/Internal 豁免全部按规则通过。 |
| R-062 | `tests/test_admission_backpressure.cpp::test_R062_*`、`tools/check_cmake_package.py::AST010AdmissionBackpressureGates` — 证明 `submit` 与 `try_submit` 共享强异常安全 admission transaction，用户构造/移动异常完整回滚 slot 与预留计数，绝不泄漏 slot 或产生孤儿任务；`try_submit` 稳定返回 `SubmissionResult<T>`。 | 编译期 RED：无统一事务与 try_submit 变体返回。 | 构造抛出异常回滚 slot 成功，try_submit 矩阵验证完全通过。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 25 tests in 27.684s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.267s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 8`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `395d8f4cf5e2bcc7381db73da14c6080efe04cc3`）：
  - Standards 轴：`SubmissionError`、`submission_rejected`、`SubmissionResult<T>` 接口设计小而稳定；`slot_cv` 与 `lifecycle_mutex` 协同保护配额与状态无竞态丢失唤醒；`submit`/`try_submit` 强事务异常回滚彻底，无 slot 泄漏；动态符号严格受限。
  - Spec 轴：R-061（External 配额、Reject/Block、非 Worker 限制、Internal 豁免）、R-062（强事务、回滚、try_submit variant 结果）100% 满足 Approved Spec 及 D-083/D-084/D-085/D-086/D-087/D-088/D-089/D-155 决策。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

