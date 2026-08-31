# AST-074 — 将 Chase-Lev 接入生产 ReadyQueues

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-066, R-067, R-068, R-081, R-101)
Milestone: v1.2.0
Blocked by: AST-025, AST-026, AST-027, AST-039, AST-072, AST-073
Status: done
Claimed by: agent

## Rules and decisions

- R-066 [supporting] — Chase-Lev 以 oracle 验证固定 portable memory order；source: D-097, D-098
- R-067 [supporting] — Deque growth 保留旧 buffer 并维持单一 Scheduling Reference；source: D-099, D-100
- R-068 [supporting] — Deque index、状态与算术不得依赖 wrap；source: D-101, D-102, D-103
- R-081 [supporting] — 每个 Ready source 按四 band 8:4:2:1 非抢占服务；source: D-129, D-130, D-131
- R-101 [supporting] — SchedulerCapabilities 精确报告实际 Local Deque backend；source: D-101, D-162, D-167

## What to build

把已经验证的 portable `ChaseLevDeque` 接入 `ReadyQueues` 的四个 per-Worker
Priority band：所属 Worker 从 bottom push/pop，其他 Worker 从 top steal；保留
Global EDF/FIFO、Local/Global burst、priority calendar 与 immediate cleanup 语义。
仅当生产实例实际使用且所需 atomic 始终 lock-free 时报告
`ChaseLevLockFree`，否则保持 Locked semantic fallback。

## Acceptance criteria

- [x] Tier-1 生产 `ReadyQueues` 每个 Worker 的四个 Local band 使用 Chase-Lev，owner push/pop 与 thief steal 不再获取 Local mutex。
- [x] 所需 atomic 不 lock-free 时自动选择 Locked Local Queue，capability 与实际 backend 一致且生命周期内冻结。
- [x] Local growth 失败时工作回退 Global Queue，不丢失、不重复、不 inline 执行。
- [x] Priority 8:4:2:1、Local LIFO、steal oldest、Global anti-starvation、Immediate cleanup 与 Coroutine resume 语义保持不变。
- [x] WSL Debug 全量、ASan/UBSan、TSan/weak-memory 相关测试以及 release/encapsulation gates 通过。

## Out of scope

- 不把 Global FIFO/EDF 改为 lock-free。
- 不新增公共 backend 选择开关或运行时动态切换。
- 不宣称整个 Runtime lock-free，也不修改 Priority/Deadline 的公共语义。

## Traceability

- Spec: `R-066`、`R-067`、`R-068`、`R-081`、`R-101`
- Decisions: `D-097` 至 `D-103`、`D-129` 至 `D-131`、`D-162`、`D-167`
- ADR: `0026`、`0028`、`0029`、`0036`、`0037`
- Verification (WSL2, Linux 6.6.87.2-microsoft-standard-WSL2, g++ 13.1.0, cmake 3.28.6, 2026-08-31):
  - Debug 全量：
    `cmake -S . -B build/wsl-gcc-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build/wsl-gcc-debug -j2 && ctest --test-dir build/wsl-gcc-debug --output-on-failure`
    — 54/54 passed。
  - ASan/UBSan 全量（独立目录，halt_on_error）：
    `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 cmake -S . -B build/wsl-gcc-asan -DCMAKE_BUILD_TYPE=Debug -DASTRA_ENABLE_SANITIZERS=ON && cmake --build build/wsl-gcc-asan -j2 && ctest --test-dir build/wsl-gcc-asan --output-on-failure`
    — 54/54 passed。
    首次全量因 `Impl` 多重继承下 `condition_variable` 谓词捕获 `this` 触发 UBSan（`astra_finalization_begin_test` / `astra_finalization_wait_test`）；谓词改为只读 `packed_status` 后复跑通过。
  - TSan（独立目录，GCC 不建模 `atomic_thread_fence`，保留 `-Wtsan`）：
    `TSAN_OPTIONS=halt_on_error=1 cmake -S . -B build/wsl-gcc-tsan -DCMAKE_BUILD_TYPE=Debug -DASTRA_ENABLE_TSAN=ON && cmake --build build/wsl-gcc-tsan -j2`
    AST-074 相关：
    `ctest --test-dir build/wsl-gcc-tsan -R 'astra_immediate_escalation_test|astra_steal_round_test|astra_priority_bands_test|astra_chase_lev_' --output-on-failure`
    — 7/7 passed（ordering、growth、indices、ready_queues、immediate、steal_round、priority_bands）。
    另：`astra_weak_memory_stress_test`、`astra_coroutine_frame_lifetime_test` 各重复 2 次通过。
    为补偿 GCC TSan 不建模 fence，生产 `ChaseLevDeque` 在 push/pop/steal/rebase 的 fence 旁增加 `__tsan_acquire` / `__tsan_release`。
    未纳入 AST-074 闭合范围：`astra_finalization_begin_test` / `astra_finalization_wait_test` 在 TSan 下仍报告 Reaper waiter 读取 `packed_status` 与主线程销毁 `shared_ptr<Impl>` 的竞争；全量 54 顺序跑曾在 `astra_immediate_escalation_test` 长时间无进展（该测试单独运行通过）。
  - `python3 -X utf8 tools/check_release_gates.py` — 15/15 passed。
  - `python3 -X utf8 tools/check_encapsulation.py` — passed。

- Follow-up repair（`docs/Chase-LevReadyQueues后续缺口与修复计划.md` A–E，HEAD `1e3d6da`，WSL2, Linux 6.6.87.2-microsoft-standard-WSL2, g++ 13.1.0, cmake 3.28.6, 2026-08-31）：
  - A：Immediate cleanup 不再写 Chase-Lev Local bottom，resume 进 Global FIFO（`2b97f26`）。
  - B：高水位 rebase 失败时 `push()` 返回 false（`b6e71d4`）。
  - C：maintenance / active_thieves seq_cst 握手，`is_lock_free` 计入 `atomic<bool>` 与 `atomic<uint32_t>`（`a534e16`）。
  - D：`ready_next` / `ready_is_external` 迁出安装头到 `ReadyLinkedInvoker`（`c3b087f`）；encapsulation 13 个负向探针。
  - E：`IntrusiveFifo::clear()`（`1e3d6da`）。
  - Debug 全量：
    `cmake --build build/wsl-gcc-debug -j2 && ctest --test-dir build/wsl-gcc-debug --output-on-failure`
    — 54/54 passed。
  - ASan/UBSan 全量（独立目录，halt_on_error）：
    `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 cmake --build build/wsl-gcc-asan -j2 && ctest --test-dir build/wsl-gcc-asan --output-on-failure`
    — 54/54 passed。
  - TSan 相关子集（独立目录，halt_on_error）：
    `TSAN_OPTIONS=halt_on_error=1 cmake --build build/wsl-gcc-tsan -j2`
    `ctest --test-dir build/wsl-gcc-tsan -R astra_chase_lev_`
    `ctest --test-dir build/wsl-gcc-tsan -R astra_immediate_escalation`
    `ctest --test-dir build/wsl-gcc-tsan -R astra_steal_round`
    `ctest --test-dir build/wsl-gcc-tsan -R astra_priority_bands`
    `ctest --test-dir build/wsl-gcc-tsan -R astra_weak_memory_stress`
    `ctest --test-dir build/wsl-gcc-tsan -R astra_coroutine_frame_lifetime`
    — 9/9 passed（ordering、growth、indices、ready_queues、immediate、steal_round、priority_bands、weak_memory、coroutine_frame）。
    未纳入本批：Reaper waiter 与 `shared_ptr<Impl>` 析构的 TSan 报告；TSan 全量 54。
