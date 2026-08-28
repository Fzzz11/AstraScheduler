# AST-006 — 解耦 Runtime State 并实现最后 Worker Handle handoff

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-020, R-021, R-022)
Milestone: v0.1.0
Blocked by: AST-005
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-020 [primary] — Handle 生命周期与共享 Runtime State 解耦；source: D-017
- R-021 [primary] — 目标 Worker 最后 Handle 通过 Reaper handoff 返回；source: D-017, D-018
- R-022 [primary] — Worker orphan handoff 保留 Graceful 默认；source: D-018

## What to build

用共享 Runtime State 承载执行身份；最后 Handle 在目标 Worker 上释放时只做预留好的 orphan handoff，并保留 Graceful 默认策略后立即返回。

## Invariants

- `[R-020]` Scheduler Handle 的生命周期必须与共享 Runtime State 解耦，Runtime State 必须存活到所有 Worker 停止访问、线程完成 join 且最终回收结束。
- `[R-021]` 目标 Scheduler Worker 销毁最后一个 Handle 时，析构必须在线性化地移交 Runtime State 强所有权给非目标 Worker Reaper 后立即返回，不得 self-wait、self-join、detach、伪造 Shutdown Completion 或仅因该场景 terminate。 例外边界：已 Stopped 时仍由安全非 Worker 路径完成必要 join/reclamation。
- `[R-022]` R-021 发生时，`Running` Runtime 必须请求 Graceful Shutdown，`Stopping` Runtime 必须保留当前 Shutdown Mode，`Stopped` Runtime 只进入安全 join/final reclamation。 例外边界：并发合法 `shutdown_now()` 可按 R-014 的顺序升级模式。

## Test-first seam

- Public seam: Scheduler Handle、Worker 执行引用与 Reaper 所有权。；同 Scheduler Worker 上的最后 Handle 析构。；Worker 最后 Handle 析构时的 Runtime 状态分支。
- RED evidence: 先写 Worker 内释放最后 Handle 的确定性测试，验证无 self-join、无悬空访问、handoff 后任务仍可完成。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-020]` 最后 Handle 消失不会导致仍在访问的 Worker 发生 use-after-free。
- [x] `[R-021]` Worker 任务可释放最后 Handle 并继续返回，Runtime State 保持有效且后续真实完成。
- [x] `[R-022]` Worker handoff 与非 Worker 析构表达相同的默认 Graceful 意图，但前者异步返回。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-020, R-021, R-022
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-017, D-018
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-020 | `tests/test_runtime_state_handoff.cpp::test_R020_runtime_state_decoupled_from_handle_lifetime` — 证明多个 `Scheduler` Handle 共享底层 `RuntimeState` 引用，销毁部分 Handle 或移动句柄不影响正在运行的兄弟 Handle 与 Worker 执行路径；所有外部 Handle 消失后 Worker 仍可安全访问 State 直至回收。 | 编译期 RED：`find_slot` 未定义；运行期未实现底层生命周期解耦。 | `test_R020_*` 测试通过，多 Handle 与 Worker 访问无 UAF。 |
| R-021 | `tests/test_runtime_state_handoff.cpp::test_R021_R022_worker_last_handle_destruction_handoff`、`test_R021_multi_worker_handoff` — 证明在目标 Worker 线程上销毁最后一个 `Scheduler` Handle 时，析构函数立即返回（耗时 < 500ms），绝不发生 self-join 死锁或异常；Worker 任务在 handoff 发生后继续执行并正常返回；底层 `RuntimeState` 所有权线性移交给非 Worker Reaper 线程并在 Worker 全部退出后安全 join 与回收。 | 运行期 RED：Worker 析构同步 self-join 死锁或异常。 | `test_R021_*` 立即返回且异步 Reaper 回收 100% 成功。 |
| R-022 | `tests/test_runtime_state_handoff.cpp::test_R021_R022_worker_last_handle_destruction_handoff`、`tools/check_cmake_package.py::AST006RuntimeStateHandoffGates` — 证明 Worker 析构 handoff 与非 Worker 析构具有相同的默认 Graceful 意图；`Running` 时触发 Graceful Stopping，`Stopping` 时保持现有模式。 | 编译期 RED：`find_slot` 未就绪。 | 状态平滑流转为 Graceful Stopping 并安全终结，package 门禁 21 项全部通过。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 21 tests in 14.778s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.201s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 4`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `21f55d4dedabbf97a0b396e076bc2a996d93d8ba`）：
  - Standards 轴：`Scheduler::Impl` 与 `ReaperRegistry` 保持 hidden 实现（`ASTRA_NO_EXPORT`），未向 `libAstraScheduler.so` 泄漏私有符号；Worker 线程识别通过 `thread_local RuntimeId` 实现无锁查询；强引用所有权移交通过 `std::shared_ptr<void>` 与 preallocated `HandoffCapabilitySlot` 保证 `noexcept` 和零堆分配；所有 Worker 线程均经非 Worker 线程显式 `join()`，杜绝 detach 与僵尸线程。
  - Spec 轴：R-020（Handle 与 State 解耦）、R-021（Worker 析构立即返回，无 self-join、无 UAF、异步 Reaper 接管）、R-022（Worker 与非 Worker 析构保持一致的默认 Graceful 意图）100% 满足 Approved Spec 及 D-017, D-018 决策。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

