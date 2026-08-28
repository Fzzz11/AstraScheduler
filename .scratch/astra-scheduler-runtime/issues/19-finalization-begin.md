# AST-019 — 实现 begin_finalization、核算集合与 startup 竞态

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-031, R-037, R-038, R-104)
Milestone: v0.1.0
Blocked by: AST-005, AST-018
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-031 [primary] — begin_finalization 只发布开始请求；source: D-026
- R-037 [primary] — begin_finalization 幂等共享唯一世代；source: D-032
- R-038 [primary] — begin 与 request_immediate 可由任意应用线程请求；source: D-033
- R-104 [primary] — Finalization 对已核算与启动中 Runtime 使用 Graceful；source: D-024, D-156

## What to build

`begin_finalization()` 幂等返回唯一世代，永久关闭注册，立即返回；对已注册和赢得 startup 核算竞态的 Runtime 请求 Graceful。

## Invariants

- `[R-031]` `begin_finalization()` 必须完成永久注册关闭、初始 Graceful/sticky 请求可靠记录和 coordinator 通知后立即返回，不得等待 Runtime drain、Join Ready、Worker/coordinator join、`Stopped` 或 `Finalized`。 例外边界：空核算集合可以在同一调用内真实完成，见 R-037。
- `[R-037]` 首次 `begin_finalization()` 必须建立唯一 Finalization Completion；并发、Finalizing 或 Finalized 后的重复调用必须返回关联同一世代的控制对象且不重复副作用；从未建立 Reaper 且核算集合为空时必须永久关闭注册并直接完成，不得创建 coordinator。
- `[R-038]` `begin_finalization()` 与 `request_immediate()` 必须允许任意应用线程调用，包括任意 Scheduler Worker；两者只完成线性化、可靠记录和通知，不得等待 Runtime drain、Worker/coordinator 退出或 join。 例外边界：允许短暂内部同步，不形成 lock-free、wait-free 或固定时延承诺。
- `[R-104]` Finalization必须对close前已核算Runtime请求Graceful且不降级既有Immediate；close先于Running publication的startup必须在开放用户工作前观察sticky请求并rollback，Running先发布则构造成功并作为核算成员可立即进入Graceful Stopping。 例外边界：显式shutdown_now/request_immediate可单向升级。

## Test-first seam

- Public seam: 首次和幂等重复 begin；重复语义见 R-037。；所有进程级 begin 调用。；两个 Finalization 请求式命令。；Finalization accounted set与Scheduler startup race的shutdown mode。
- RED evidence: 先写多线程 begin、begin/startup 线性化、已核算 Runtime、调用线程身份及“begin 不等待完成”测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-031]` begin 返回只证明 Finalization 已不可逆开始；活动 Runtime 可继续在后台推进。
- [x] `[R-037]` 多个 begin 获得的控制对象观察同一 Completion，空进程 begin 后 Completed 且无 Reaper thread。
- [x] `[R-038]` Worker 可发起全局终结或升级后继续完成当前任务，不产生 self-wait。
- [x] `[R-104]` Finalization不因进程收尾默认取消已接受工作，半启动Runtime不获得用户执行窗口。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-031, R-037, R-038, R-104
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-026, D-032, D-033, D-024, D-156
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-031 | `tests/test_finalization_begin.cpp::test_R031_begin_does_not_wait_for_runtime_completion` — 验证 `begin_finalization()` 立即返回（< 40ms），不等待后台长任务与 drain 完成。 | 运行期 RED：begin 同步阻塞直至任务执行完成。 | begin 仅关闭注册并发布 Graceful 请求后非阻塞返回。 |
| R-037 | `tests/test_finalization_begin.cpp::test_R037_empty_system_and_idempotent_generation` — 验证空系统调用 `begin_finalization()` 直接完成且 coordinator 线程数为 0；并发重复调用幂等安全。 | 运行期 RED：空系统误创建 coordinator 线程或并发调用出现竞态副作用。 | 空系统无 coordinator 线程，多次 begin 共享同一世代。 |
| R-038 | `tests/test_finalization_begin.cpp::test_R038_worker_can_call_begin_and_request_immediate` — 验证 Worker 线程内调用 `begin_finalization()` 与 `request_immediate()` 不产生 self-wait，可顺利完成任务。 | 运行期 RED：Worker 调用发生死锁或抛出异常。 | Worker 线程安全调用非阻塞命令并正常退出。 |
| R-104 | `tests/test_finalization_begin.cpp::test_R104_finalization_graceful_and_startup_race` — 验证已核算 Runtime 收到 Graceful 请求（状态转为 Stopping），close 之后创建的新 Runtime 强事务回滚并抛出 `scheduler_creation_rejected(FinalizationStarted)`。 | 运行期 RED：Runtime 模式未转为 Graceful，或 close 后仍允许创建新 Scheduler。 | 严格保持 Graceful 模式并拒绝 close 后的新实例。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 34 tests in 38.225s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.238s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 17`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `edb6dc3`）：
  - Standards 轴：`begin_finalization()` 遵循无界非阻塞规范；`ReaperRegistry` 在空集合时不拉起无谓线程；锁内安全提取回调并在锁外广播。
  - Spec 轴：R-031（begin 非阻塞）、R-037（幂等唯一世代与空系统无线程）、R-038（任意线程含 Worker 可调用）、R-104（Graceful 广播与 startup 竞态全序）100% 满足 Approved Spec。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

