# AST-062 — 锁定协议类型完成型边界

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-118)
Milestone: v1.2.0
Blocked by: AST-059, AST-060, AST-061
Status: done
Claimed by: agent

## Rules and decisions

- R-118 [primary] — 安装头不得提供可完成的运行协议类型；source: D-170
- R-114 [supporting] — consumer 不可访问实现状态；source: D-169, D-170
- R-117 [supporting] — public tests 与 internal seams 物理隔离；source: D-169

## What to build

独立 package consumer 对 `TaskSharedStateBase`、`AwaitHandshake` 及等价 TaskControlBlock/handshake 协议类型的完成型 compile probe 全部失败。documented `public_contract.cpp` 继续编译运行。需要 handshake/TCB 的测试必须是 internal tests，只通过非安装头访问。mutex、完成回调、rescheduler、timer 注册、handshake 状态机与 invoker 执行协议不得以完整类型出现在 installed headers。

## Invariants

- 结果格、F 信封与薄 awaiter 不属于本规则禁止的协议类型（R-119 至 R-121）。
- 编译失败型 boundary probe 可以有意引用被禁止的名称。
- public tests 不得带 `src/` include path。

## Test-first seam

- Public seam: encapsulation / package consumer 完成型 negative compile probes 与 `public_contract.cpp`。
- RED evidence: 任一协议类型仍可被独立 consumer 完成时 probe 失败。

## Acceptance criteria

- [x] `[R-118]` 独立 consumer 不能完成 `TaskSharedStateBase`、`AwaitHandshake` 或等价协议类型。
- [x] `[R-118]` documented public_contract 继续编译运行。
- [x] `[R-114]` public consumer 不能修改运行时不变量或调用测试控制入口。
- [x] `[R-117]` handshake/TCB 白盒测试为 internal tests，public tests 无 `src/` include。

## Out of scope

- 不配置 version script（AST-063 / R-123）。
- 不深化 GraphExecution（R-116）。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-118
- Decisions: `.scratch/astra-scheduler-runtime/decision-log.md` — D-170
- ADR: `docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md`
- Verification: WSL GCC Debug；`tools/check_encapsulation.py` 12 个 negative probes + installed-header 完成型审计通过；独立 `check_cmake_package.py` public_contract 50/50；handshake/TCB 白盒测试走 `src/` internal tests。
