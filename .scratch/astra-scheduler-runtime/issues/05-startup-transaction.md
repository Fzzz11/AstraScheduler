# AST-005 — 实现 Scheduler startup transaction 与 Finalization gate 排序

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-023, R-024, R-097)
Milestone: v0.1.0
Blocked by: AST-004
Status: done
Claimed by: Antigravity agent (2026-08-27)

## Rules and decisions

- R-023 [primary] — Reaper handoff 能力先于 Worker 启动；source: D-019
- R-024 [primary] — 运行期 handoff 不获取可失败资源；source: D-019
- R-097 [primary] — Scheduler startup transaction 与 Finalization close 唯一排序；source: D-023, D-024, D-155, D-156

## What to build

将配置验证、Reaper registration/handoff 预留、Worker 创建和 Running 发布组织成同步强事务；与永久 registration close 建立唯一线性化顺序。

## Invariants

- `[R-023]` Runtime 必须在任何 Worker 启动前建立并预留 Reaper handoff 能力；准备失败时启动必须失败、不得发布 `Running`，且不得留下活动 Worker。
- `[R-024]` 进入 `Running` 后，Worker 最后 Handle 的 handoff 必须为 `noexcept`、不分配内存、不创建线程，并且不得等待 Drain Work Closure、Worker 退出或 Shutdown Completion。 例外边界：可以使用内部同步完成线性化。
- `[R-097]` `Scheduler(options)`必须同步验证、预留/注册Reaper、创建Worker并经barrier一次发布Running后才返回，失败阻止用户工作并完整join/rollback；Running publication与Finalization close线性排序，close先则startup不开放admission并回滚后抛FinalizationStarted creation rejection，Running先则构造成功且随后可立即Graceful Stopping。 例外边界：无Runtime的空集合begin可直接Finalized。

## Test-first seam

- Public seam: Scheduler 启动事务。；R-021 的运行期所有权移交路径。；Scheduler创建、Finalization核算集合与startup race。
- RED evidence: 用可注入失败点先覆盖每个 startup 阶段回滚、无公开 Starting 状态、close/start 竞态仅有两个合法结果。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-023]` 注入注册、预留或 coordinator 建立失败时，用户任务从未获得执行窗口。
- [x] `[R-024]` 资源耗尽故障注入不会让已经 Running 的 handoff 丢失 Runtime State 所有权。
- [x] `[R-097]` 不存在public Created/Starting或半启动Handle，竞态Scheduler恰好成功纳入或零用户工作失败。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-023, R-024, R-097
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-019, D-023, D-024, D-155, D-156
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-27；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-023 | `tests/test_startup_transaction.cpp::test_R023_*` — 预留 Reaper handoff 能力与 Worker 启动顺序严格断言；注入预留失败抛出 `std::bad_alloc` 且保证 0 活跃 Worker、0 残留注册、未发布 Running；注入 Worker 线程中断失败时抛出 `std::system_error` 且完整回滚 join 已启动 Worker 并撤销 Reaper 注册。 | 编译期 RED：`fatal error: astra/error.hpp: No such file or directory`；运行期无事务回滚能力。 | `test_R023_*` 故障注入回滚用例 100% 通过，0 活跃 Worker 残留，0 注册泄漏。 |
| R-024 | `tests/test_startup_transaction.cpp::test_R024_*`、`tests/consumer/main.cpp` — `HandoffCapabilitySlot` 在启动期预分配并建立原子标志；运行期 handoff 操作均为 `noexcept`，无动态内存分配与无线程创建，资源耗尽故障不会导致 handoff 失败或丢失所有权。 | 编译期 RED：未定义预留能力槽位结构与 noexcept 原子保障。 | `test_R024_*` static_assert 与预分配槽位原子操作断言通过。 |
| R-097 | `tests/test_startup_transaction.cpp::test_R097_*`、`tests/consumer/main.cpp`、`tools/check_cmake_package.py::AST005StartupTransactionGates` — 异常类型 `scheduler_creation_rejected` 继承自 `std::runtime_error` 且携带 `SchedulerCreationError::FinalizationStarted`；Finalization 已开启时构造直接拒绝；并发启动与 `close_registration()` 竞态严格仅产生成功 Running 纳入核算或拒绝回滚两类合法结果；不存在 Created/Starting 中间态。 | 编译期 RED：`fatal error: astra/error.hpp: No such file or directory`。 | `test_R097_*`、consumer smoke 与 CMake package 门禁 20 项全部通过。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 20 tests in 14.729s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.160s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake -S . -B build/wsl-gcc-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 3`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `21f55d4dedabbf97a0b396e076bc2a996d93d8ba`，改动为工作区未提交文件）：
  - Standards 轴：新增 `include/astra/error.hpp` 符合 include guards 与 64-bit Linux 规范；`scheduler_creation_rejected` 经 `ASTRA_EXPORT` 正确导出类型信息以支持跨 DSO 异常捕获；`src/reaper_registry.hpp` 与 `src/reaper_registry.cpp` 为隐藏实现（`ASTRA_NO_EXPORT`），动态库导出符号严格受控；多线程 Worker 启动与析构 join 遵循 RAII 强异常安全；通过 CMake package 规范正确安装头文件。
  - Spec 轴：R-023（Reaper 能力优先于 Worker 建立、启动失败 0 活跃 Worker 保证）、R-024（运行期 handoff noexcept 无分配）、R-097（同步强事务、原子 Running 发布、Finalization close 线性竞态排序及 FinalizationStarted 异常类型）100% 对齐 Approved Spec 与决策台账（D-019, D-023, D-024, D-155, D-156）。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

