# AST-061 — submit/emplace 安装头只保留 F 信封

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-121)
Milestone: v1.2.0
Blocked by: AST-059
Status: done
Claimed by: agent

## Rules and decisions

- R-121 [primary] — submit/emplace 安装头只保留 F 信封；source: D-174, D-170
- R-118 [supporting] — invoker 执行协议不得以完整类型留在安装头；source: D-170
- R-058 [supporting] — 结果类型约束不变；source: D-074
- R-102 [supporting] — move-only callable 仍可提交；source: D-165

## What to build

`submit`/`try_submit`/`spawn`/`try_spawn` 与 `TaskGraph::emplace`/`emplace_coroutine` 的安装头 invoker 只存储可调用对象 `F` 及是否接受 `stop_token` 的形态；`execute()` 只调用 `F` 并把返回值交给结果格或 compiled TCB。admission、try_start、metrics、helping、timer、handshake 不得出现在该信封的安装头实现中。不得把 `F` 擦成 `std::function`。move-only `F` 仍可提交。Graph 节点仍必须返回 void。

## Invariants

- 可调用对象约束：`f(args...)` 或 `f(stop_token, args...)`；不得返回裸引用；结果可移动。
- Graph 节点可调用对象返回 void。
- 不因改用 `std::function` 拒绝只移动的 `F`。

## Test-first seam

- Public seam: submit/spawn/emplace 行为测试与 move-only callable 测试；对安装头 invoker 协议内联的负向审计。
- RED evidence: 安装头 invoker 仍内联 try_start/metrics/handshake 时审计失败；现有提交测试必须保持绿。

## Acceptance criteria

- [x] `[R-121]` submit/try_submit/spawn/try_spawn 与 Graph emplace/emplace_coroutine 行为测试通过。
- [x] `[R-121]` 安装头信封实现不含 admission、try_start、metrics、helping、timer 或 handshake。
- [x] `[R-121]` 未使用 `std::function` 作为 header 缝上的 F 擦除。
- [x] `[R-102]` move-only callable 仍可提交。
- [x] `[R-058]` 拒绝裸引用结果与不可移动结果的编译期约束仍在。

## Out of scope

- 不把 GraphExecution 收成深模块（R-116）。
- 不配置 version script（AST-063）。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-121
- Decisions: `.scratch/astra-scheduler-runtime/decision-log.md` — D-174, D-170
- ADR: `docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md`
- Verification: WSL GCC Debug；`TaskInvokerModel::execute` 只调用 F 并把结果写入 ResultCell；`wrap_submitted_invoker` 在库内做 try_start/metrics/context；Graph emplace 仍直接存储 F；encapsulation 审计拒绝 `std::function` F 擦除与 execute 内联 try_start；move-only submit 测试通过；`ctest` 52/52。
