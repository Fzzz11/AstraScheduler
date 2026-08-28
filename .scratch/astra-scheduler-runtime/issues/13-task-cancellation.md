# AST-013 — 实现显式 Task cancellation 的首次 start 分类

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-053, R-054)
Milestone: v0.1.0
Blocked by: AST-010, AST-011
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-053 [primary] — request_cancel 按首次 start 竞态分类；source: D-052, D-053, D-054, D-055
- R-054 [primary] — Cooperative stop 的真实退出决定 Outcome；source: D-056, D-058, D-059, D-060

## What to build

`request_cancel()` 与首次 start 线性化竞争；未开始任务发布 Cancelled，已开始任务只收到 cooperative stop，最终 Outcome 由真实退出决定。

## Invariants

- `[R-053]` `void TaskHandle::request_cancel() const noexcept` 必须幂等、可并发且在请求可靠发布后立即返回；请求在线性化上先于首次 start 时 Task 直接发布 Cancelled且不执行用户代码，start 先胜出时只发布 cooperative stop request。 例外边界：empty Handle 的 request_cancel 是 R-057 的 no-op；已Terminal不改写。
- `[R-054]` Running Callable 在 stop request 后正常返回仍必须发布 Value，抛出 `task_cancelled` 才发布 Cancelled，其他异常发布 Exception；`submit` 必须优先普通invocation，仅普通形式不可调用时在首参数注入该Task的 `std::stop_token`，并提供不挂起的 `throw_if_stop_requested(token)` cancellation point。 例外边界：Coroutine内建awaiter取消见 R-075。

## Test-first seam

- Public seam: 任意线程对有效 TaskHandle 的单 Task cancellation。；stop-aware Callable invocation 与 execution boundary。
- RED evidence: 用 barrier 穷举 cancel-before-start、start-wins、忽略 stop、`task_cancelled` 退出和普通异常路径。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-053]` cancellation/start竞态只有一个分类，重复调用不重复完成或执行。
- [x] `[R-054]` stop request本身不覆盖用户真实结果，generic callable不会意外收到token。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-053, R-054
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-052, D-053, D-054, D-055, D-056, D-058, D-059, D-060
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-053 | `tests/test_task_cancellation.cpp::test_R053_cancel_before_start_wins`、`test_R053_idempotent_and_empty_handle` — 证明在任务 start 之前请求取消在线性化上胜出，任务直接发布 `Cancelled` 终态且 Callable 0 次执行；`request_cancel()` 幂等、线程安全且空句柄为 no-op。 | 运行期 RED：pre-start cancel 未阻断 worker 调度执行。 | pre-start 状态直接发布 Cancelled，worker 检查到已取消立即跳过用户 Callable。 |
| R-054 | `tests/test_task_cancellation.cpp::test_R053_R054_running_cooperative_stop_outcomes`、`test_R054_submit_prefers_ordinary_invocation` — 证明 Running 状态收到 stop request 后，最终 Outcome 完全由真实退出行为决定（正常返回为 Succeeded、抛出 `task_cancelled` 为 Cancelled、其他异常为 Failed）；`submit()` 优先匹配普通签名，避免 generic callable 意外注入 `stop_token`。 | 运行期 RED：stop request 强行篡改用户正常返回值或 generic 重载解析歧义。 | 正常返回发布 Value，throw_if_stop_requested 发布 Cancelled，普通调用优先。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 28 tests in 37.882s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.253s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 11`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `70db246`）：
  - Standards 轴：`try_start()` 与 `request_cancel()` 在锁保护下形成严格二值竞态，无锁竞争漏洞；`std::stop_source`/`std::stop_token` 协作停机封装优雅；`throw_if_stop_requested` 符合 C++20 stop token 规范。
  - Spec 轴：R-053（首次 start 竞态分类与 0 次执行保证）、R-054（真实退出行为决定终态与普通调用优先）100% 满足 Approved Spec。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

