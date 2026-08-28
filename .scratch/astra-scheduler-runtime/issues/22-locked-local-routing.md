# AST-022 — 加入 Locked Local Deque 与 Ready Routing Precedence

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-063, R-101)
Milestone: v0.2.0
Blocked by: AST-008, AST-010, AST-012
Status: done
Claimed by: agent

## Rules and decisions

- R-063 [primary] — Ready Routing Precedence 与 source公平性固定；source: D-090, D-091, D-092, D-147
- R-101 [supporting] — SchedulerCapabilities 报告实际Local Deque backend；source: D-101, D-167, D-162

## What to build

为每个 Worker 增加 Locked Local Deque，按专用 destination→owner Local/off-worker Global 的 precedence 路由，并保持 Global source 不饥饿。

## Invariants

- `[R-063]` v0.1全部Ready进Global；后续External/cross-runtime进Global、same-runtime Internal进owner Local，无专用规则的完成publication在owner Worker走Local否则Global；yield/timer强制ordinary Global，never-started Deadline进Global EDF；Worker连续Local claim最多默认64次后必须探测Global，Global FIFO、owner Local LIFO、thief取oldest。 例外边界：deadline Task首次start后按触发它的awaiter回归非EDF规则。
- `[R-101]` 有效Scheduler的 `capabilities()`必须返回不可由用户aggregate-initialize的trivially-copyable immutable `SchedulerCapabilities`，其 `local_deque_backend()`为 `LocalDequeBackend::{None,Locked,ChaseLevLockFree}`之一且 `lock_free_local_deque()`仅最后一种为true；v0.1为None、v0.2/fallback为Locked，Stopped后保留且不得运行时切换或按版本推断，空Scheduler抛logic_error。 例外边界：该能力不声称整个Runtime lock-free。

## Test-first seam

- Public seam: Ready Task首次publication和Coroutine resume routing。；Runtime、Trace metadata与Benchmark artifact。
- RED evidence: 先写 publisher 身份、awaiter/deadline 占位 destination、local/global source 顺序和 capability=`Locked` 测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-063]` routing source可由Trace验证，Local洪水不能永久饿死Global。
- [x] `[R-101]` v0.2 启用 Locked Local Deque 后 capability 必须报告 `Locked` 且 `lock_free_local_deque()==false`，不得按版本名称虚报 Chase-Lev lock-free。

## Out of scope

- 不实现 Chase-Lev lock-free backend 及 v0.3 之后的 public 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-063, R-101
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-090, D-091, D-092, D-147, D-101, D-167, D-162
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:
  - 单元测试：`tests/test_locked_local_routing.cpp` 覆盖 R-063（External vs Internal 路由优先级、Local Deque LIFO、防 Global 饥饿探测阈值 64）与 R-101（v0.2 capabilities 返回 Locked 且 lock_free 为 false、空 handle 抛 logic_error）。
  - In-tree CTest：`wsl bash -lc "ctest --test-dir build/wsl-gcc-debug --output-on-failure"` 20/20 tests 全部 PASS。
  - Package consumer 与安装门禁：`python3 -X utf8 tools/check_cmake_package.py` 37/37 tests 全部 OK。
  - 发布门禁：`python3 -X utf8 tools/check_release_gates.py` 15/15 tests 全部 OK。
