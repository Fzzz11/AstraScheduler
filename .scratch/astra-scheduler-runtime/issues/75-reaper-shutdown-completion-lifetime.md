# AST-075 — 修复 Reaper Shutdown Completion Waiter 与 Runtime State 析构竞争

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)（hotfix；不扩张 public API）
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-020, R-026, R-103, R-105)
Milestone: v1.2.0
Blocked by: AST-006, AST-007, AST-017
Status: done
Claimed by: agent

## Rules and decisions

- R-020 [supporting owner: AST-006] — Runtime State 必须存活到 Worker 停止访问、join 完成且最终回收结束；source: D-017
- R-026 [supporting owner: AST-007] — Reaper 与同步关停路径只能有一个 join owner；Stopped 只在 join 之后发布；source: D-020
- R-103 / R-105 [supporting owner: AST-017] — 最后非 Worker Handle 的 noexcept 同步 RAII 仍等待真实 Shutdown Completion；source: D-014, D-017

本 Ticket 是上述规则的缺陷修复，不改变 Handle/Reaper 公共语义。

## What to build

Finalization 与最后 Handle RAII 并发时，Reaper 协调线程可能作为 Shutdown Completion Waiter 在 `condition_variable` 谓词里读取 Runtime State（`packed_status` / `shutdown_mutex`），而主线程 `~Scheduler` 已作为 Leader 发布 `Stopped` 并 `delete Impl`。

TSan 复现：`astra_finalization_begin_test`（`test_R031`）、`astra_finalization_wait_test`（`test_R039`）。注册期 cleanup 只抓裸 `this`；`retained_state` 仅 orphan handoff 才持有强引用。

修复：Waiter 等待用的 mutex/cv/完成标志必须能在 `Impl` 析构后仍存活（独立的共享 Shutdown Completion），直到 Waiter 从 `wait` 返回。不得让控制线程代 owner 写 Local bottom，不改 Global FIFO/EDF，不把 TSan 注解当成弱内存证明。

## Invariants

- Runtime State 的可观察 status 仍由 `packed_status` 提供；Waiter 不得在 `Impl` 释放后访问其成员。
- 仍恰好一个 join owner；不得 double-join worker 线程。
- 最后非 Worker Handle 析构仍 noexcept、仍等待真实 Stopped。

## Test-first seam

- Public seam: `begin_finalization()` + 测试线程上最后 `Scheduler` 析构；Worker 调用 `wait()` 抛 `logic_error` 后的 Handle RAII。
- RED evidence: WSL TSan `astra_finalization_begin_test` / `astra_finalization_wait_test` 报告 `~shared_ptr<Impl>` 与 `reaper_cleanup_and_join` 谓词 `packed_status.load` 的 data race。

## Acceptance criteria

- [x] `[R-020]` 上述两测试在 TSan（`halt_on_error=1`）下不再报告 Impl 析构与 Reaper waiter 的 data race。
- [x] `[R-026]` Debug 下 finalization begin/wait 与 last-handle RAII 仍通过；不得出现双重 join 或过早 Stopped。
- [x] `[R-103]` 非 Worker 最后 Handle 析构后 `status()` 为 Stopped。

## Out of scope

- TSan 全量 54 顺序挂起。
- Global FIFO/EDF lock-free。
- 删除 Chase-Lev TSan 注解或宣称弱内存证明。
- 控制线程写 owner-only Local bottom。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-020, R-026, R-103, R-105
- Decisions: D-017, D-020, D-014
- ADR: 0007, 0008, 0011
- Diagnosis: `docs/AST-074闭合后剩余四项分流.md` 第 3.1 节
- Verification (WSL2, g++ 13.1.0, cmake 3.28.6, 2026-08-31):
  - RED: `TSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/wsl-gcc-tsan/tests/astra_finalization_begin_test` 与 `astra_finalization_wait_test` 报告 `~shared_ptr<Impl>` vs `reaper_cleanup_and_join` 谓词 `packed_status.load`。
  - GREEN TSan（各 2 次）: 同上命令，无 data race；另 `astra_last_handle_raii_test`、`astra_reaper_coordinator_test`、`astra_immediate_escalation_test` 通过。
  - GREEN Debug 全量: `ctest --test-dir build/wsl-gcc-debug --output-on-failure` — 54/54。
  - GREEN ASan/UBSan: `astra_finalization_begin_test` 与 `astra_finalization_wait_test`（`halt_on_error=1`）。
  - 修复: `ShutdownCompletion` 与 Runtime State 分寿命；`replace_cleanup_fn` 用 `weak_ptr` 锁住 Impl 直到 Waiter 返回。
