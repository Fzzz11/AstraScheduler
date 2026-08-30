# AST-060 — 公开 awaitable 改为薄包装并移出 handshake

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-120)
Milestone: v1.2.0
Blocked by: AST-059
Status: done
Claimed by: agent

## Rules and decisions

- R-120 [primary] — 公开 awaitable 以薄包装留在安装头且不暴露协议；source: D-172, D-170
- R-118 [supporting] — 安装头不得完成 AwaitHandshake；source: D-170
- R-073 [supporting] — cold Task 与 spawn 语义不变；source: D-114
- R-076 [supporting] — await 路由与自等待拒绝不变；source: D-120
- R-079 [supporting] — Worker timer 的 sleep 语义不变；source: D-126

## What to build

`co_await` TaskHandle（仅左值）、GraphRun（仅左值）、`yield()`、`sleep_for`/`sleep_until`、`cancellation_point` 保持既有挂起/恢复/取消语义。awaiter 作为薄包装可留在安装头：`await_suspend` 只调用 compiled 协议；类型及其成员不得暴露 `AwaitHandshake`、TaskControlBlock/`TaskSharedState`、mutex/cv、rescheduler 或 timer registrar。documented surface 是这些操作，不承诺 awaiter 类型名。

## Invariants

- 左值-only co_await 与 rvalue delete 保持不变。
- 不把第三方自定义 awaiter 列为 documented 扩展面。
- handshake 状态机不得在安装头展开。

## Test-first seam

- Public seam: spawn/yield/sleep/await 行为测试；独立 consumer 对 `AwaitHandshake` 的完成型 compile probe。
- RED evidence: 安装头仍能完成 `AwaitHandshake` 时 probe 失败；现有 coroutine 行为测试必须保持绿。

## Acceptance criteria

- [x] `[R-120]` yield/sleep/cancellation_point 与 TaskHandle/GraphRun co_await 行为测试通过。
- [x] `[R-120]` 安装头中的 awaiter 不暴露 handshake、TCB、mutex/cv、rescheduler 或 timer registrar。
- [x] `[R-118]` 针对 `AwaitHandshake` 的完成型 compile probe 失败。
- [x] `[R-076]` 左值 await 可用；自等待拒绝语义不变。
- [x] `[R-079]` sleep 取消与到期语义不变。

## Out of scope

- 不整条 await 路径类型擦除。
- 不改 F 信封（AST-061）。
- 不配置 version script（AST-063）。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-120
- Decisions: `.scratch/astra-scheduler-runtime/decision-log.md` — D-172, D-170
- ADR: `docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md`
- Verification: WSL GCC Debug；`AwaitHandshake` 完整定义移至 `src/await_handshake.hpp`；awaiter 只调用 `tcb_arm_*` compiled 协议；`R120_await_handshake` probe 拒绝完成型；worker timers / spawn / graph coroutine / handshake tests 通过；`ctest` 52/52。
