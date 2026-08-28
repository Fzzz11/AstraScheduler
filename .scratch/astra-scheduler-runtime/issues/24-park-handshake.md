# AST-024 — 实现无丢唤醒 Park Handshake

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-065)
Milestone: v0.2.0
Blocked by: AST-015, AST-022, AST-023
Status: done
Claimed by: agent

## Rules and decisions

- R-065 [primary] — Park Handshake 防止 Ready/控制面丢唤醒；source: D-094, D-095, D-096

## What to build

在休眠前登记意图并二次检查 publication generation、Ready sources 与退出条件；work/control publication 正确唤醒。

## Invariants

- `[R-065]` Worker空闲必须先有限active backoff再可通知park；所有work publisher按 publish→advance epoch→notify，Worker登记park intent前后双检work、Shutdown与epoch；单个/批量work按可并行性唤醒，控制面变化notify-all，epoch饱和必须进入无ABA slow path或禁用park。 例外边界：pause/yield具体次数为内部benchmark参数。

## Test-first seam

- Public seam: Global/Local/DAG/Coroutine/timer publication与Worker退出。
- RED evidence: 用 park 前/后 barrier 穷举 publish、shutdown、spurious wake 和多 Worker 唤醒竞争。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-065]` producer与park竞态不产生永久睡眠，空闲不busy-spin。

## Out of scope

- 不实现 Chase-Lev lock-free backend 及 v0.3 之后的 public 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-065
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-094, D-095, D-096
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:
  - 单元测试：`tests/test_park_handshake.cpp` 覆盖 R-065（空闲 Worker 正确进入 Park 状态且不忙等、有新任务即时唤醒、高并发 Producer-Park 竞态下无丢唤醒、控制面 Shutdown 唤醒全部 Parked Worker）。
  - In-tree CTest：`wsl bash -lc "ctest --test-dir build/wsl-gcc-debug --output-on-failure"` 22/22 tests 全部 PASS。
  - ASan / UBSan / LSan 内存安全与泄漏门禁：`build/wsl-gcc-asan` 22/22 tests 全部 PASS（0 leaks / 0 errors / 0 deadlocks）。
  - Package consumer 与安装门禁：`python3 -X utf8 tools/check_cmake_package.py` 39/39 tests 全部 OK。
  - 发布门禁：`python3 -X utf8 tools/check_release_gates.py` 15/15 tests 全部 OK。
 
