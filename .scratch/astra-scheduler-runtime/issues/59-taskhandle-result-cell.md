# AST-059 — TaskHandle 结果格与 compiled TaskControlBlock

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-119)
Milestone: v1.2.0
Blocked by: AST-058
Status: done
Claimed by: agent

## Rules and decisions

- R-119 [primary] — 结果格为 TaskHandle private nested，安装头不出现 TaskSharedState 名；source: D-173, D-170
- R-118 [supporting] — 安装头不得完成 TaskSharedStateBase 等协议类型；source: D-170
- R-048 [supporting] — TaskHandle 共享 identity 与终态观察；source: D-041
- R-051 [supporting] — 仅左值 get 返回稳定 const T&；source: D-076
- R-057 [supporting] — 空句柄与生命周期投影；source: D-067

## What to build

安装头用 `TaskHandle<T>` / `TaskHandle<void>` 的 private nested 结果格承载值（或 void）、异常与终态。mutex、回调、rescheduler、timer、handshake 编进 compiled TaskControlBlock。package consumer 不能完成 `TaskSharedState` 或 `TaskSharedStateBase`。`get`/`wait`/`request_cancel` 可观察语义不变。

## Invariants

- `get()` 仍返回 `const T&`（void 不返回值）；失败重抛；取消抛 `task_cancelled`；仅左值可调用。
- 空/moved-from 句柄行为不变。
- 结果格不得包含 mutex、回调列表、rescheduler、timer hook 或 handshake。
- 不改变 documented public 调度与取消语义。

## Test-first seam

- Public seam: TaskHandle get/wait/cancel；独立 consumer 对 `TaskSharedState*` 的完成型 compile probe。
- RED evidence: 安装头仍能完成 `TaskSharedState` / `TaskSharedStateBase` 时 probe 失败（此时应判定未完成）；现有 get/wait 测试必须保持绿。

## Acceptance criteria

- [x] `[R-119]` 结果格为 TaskHandle 的 private nested；consumer 不能将其作为独立入口命名或构造。
- [x] `[R-119]` 安装头不能完成 `TaskSharedState` 或 `TaskSharedStateBase`。
- [x] `[R-119]` 现有 TaskHandle get/wait/cancel 行为测试通过。
- [x] `[R-118]` 针对 TaskSharedStateBase 的完成型 compile probe 失败。
- [x] `[R-051]` 左值 get 仍可用；rvalue get 仍编译失败。
- [x] `[R-057]` 空句柄不伪装有效 Task 状态。

## Out of scope

- 不把 handshake 移出 coroutine 安装头（AST-060 / R-120）。
- 不改 submit/emplace invoker 为 F 信封（AST-061 / R-121）。
- 不配置 version script（AST-063 / R-123）。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-119
- Decisions: `.scratch/astra-scheduler-runtime/decision-log.md` — D-173, D-170
- ADR: `docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md`
- Verification: WSL GCC Debug；compiled `TaskControlBlock` 在 `src/task_control_block.hpp`；`TaskHandle::{T,void}::ResultCell` 为 private nested；encapsulation probes `R119_task_shared_state*` / `R119_result_cell_independent` / `R119_task_control_block` 拒绝完成型；`ctest` Debug 52/52；独立 consumer package 50/50。
