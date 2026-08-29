# AST-056 — 修复 Coroutine 帧生命周期竞争（yield/sleep/await handoff 后的二次销毁）

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-073, R-074, R-076)
Milestone: hotfix（blocking AST-050 benchmark corpus）
Blocked by: None
Status: done
Claimed by: agent

## Rules and decisions

- R-073 [primary owner: AST-032]、R-074 [primary owner: AST-033]、R-076 [primary owner: AST-035]
  —— 本 Ticket 为上述规则的缺陷修复（supporting），恢复其既定生命周期语义：
  协程帧恰好销毁一次；source: D-114, D-119, D-122, D-147

## What to build

修复既有缺陷：`YieldAwaiter/SleepAwaiter/TaskHandleAwaiter/Graph/跨Runtime await` 的
`await_suspend` 在将 resume invoker 重新入队后返回 true，快速 worker 可在挂起方
`coro.resume()` 返回并执行 `coro.done()/destroy()` 之前完成整个协程并销毁帧，
导致 use-after-free/double-free（协程帧被销毁两次或销毁后访问）。

## Invariants

- 每个协程帧恰好被销毁一次；suspend 方在 requeue 后不再触碰帧（单一所有权移交）。
- 修复不得改变 yield/sleep/await 的调度语义、取消语义或 Metrics/Trace 口径。

## Test-first seam

- Public seam: spawn + `co_await yield()/sleep_for/TaskHandle` + 主线程 `get()` 高频循环。
- RED evidence: 512 次 spawn+yield+get 循环在 ASan 下 heap-use-after-free（pre-AST-048
  源码同样复现，15 次运行崩溃 11 次，证明为既有缺陷而非观测改动引入）。

## Acceptance criteria

- [x] `[R-073]`（supporting 修复，primary 归 AST-032/033/035）spawn+yield/sleep/await+get 循环在 Debug 与 ASan 下稳定无 UAF/double-free。

## Out of scope

- 不改变 awaiter 语义、不引入新 public API；await_suspend 抛出路径的帧泄漏（既有）另行处理。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-073, R-074, R-076
- Decisions: `.scratch/astra-scheduler-runtime/decision-log.md` — D-114, D-119, D-122, D-147
- Verification: `tests/test_coroutine_frame_lifetime.cpp`（4 用例：yield×512、sleep_for×256、co_await TaskHandle×256、三重 yield 嵌套 handoff×256）RED：ASan heap-use-after-free（修复前，pre-AST-048 源码同样复现 15 次崩溃 11 次，证明为既有缺陷）；GREEN：Debug/ASan 各 10+ 次循环稳定、全量 Debug 47/47 与 ASan 47/47 通过、astra_coroutine_resume_handshake_test 连续 5 次稳定（历史间歇崩溃同源）；gates 15/15；traceability 通过（tickets=56）。修复内容：TaskSharedStateBase 增加 resume_handoff_seq_ 代际计数；Yield/Sleep/TaskHandle/GraphRun awaiter 在 requeue 前标记 handoff；CoroutineTaskInvokerModel/CoroutineResumeInvokerModel 在 resume() 返回后按代际裁决所有权——已移交则不再读取 done()/销毁帧。
