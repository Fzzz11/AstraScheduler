# AST-012 — 实现 Unbounded/Helping wait 与 timeout 边界

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-052, R-055, R-056, R-059)
Milestone: v0.1.0
Blocked by: AST-008, AST-011
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-052 [primary] — get 使用 caller-relative Unbounded/Helping Wait；source: D-047, D-048, D-049, D-050, D-051
- R-055 [primary] — wait 只同步完成且复用 Helping；source: D-061, D-062, D-073
- R-056 [primary] — wait_for 超时不伪造 Task 完成；source: D-063, D-064, D-065, D-066
- R-059 [primary] — Helping depth 与 Shutdown eligibility 受配置约束；source: D-078, D-079, D-080

## What to build

非 Worker 使用无界同步等待；同 Runtime Worker 通过正常调度路径 Helping，受 depth/Shutdown eligibility 限制；`wait_for` 超时不伪造完成。

## Invariants

- `[R-052]` 未完成 Task 的 `get()` 在非 Worker 上必须执行 Unbounded Wait；同 Runtime Worker 必须 Helping source Runtime，跨 Runtime Worker 只帮助 source Runtime；Direct Self-Wait 必须在副作用前抛 `std::logic_error`，Runtime 不保证检测 Indirect Wait Cycle。 例外边界：source Runtime Immediate 时只可恢复已开始Coroutine segment，见 R-059/R-075。
- `[R-055]` `TaskHandle::wait() const` 必须只在真实 Terminal Outcome 发布后返回，不传播或消费 Value/Exception/Cancelled，并按调用方身份复用 R-052 的 Unbounded/Helping/self-wait规则；多个等待者共享一次 completion publication且不得丢失完成。
- `[R-056]` `TaskHandle::wait_for(duration)` 必须以 steady_clock 返回 `Completed/TimedOut`且不传播Outcome或取消Task；非正duration即时观察，完成与期限形成唯一顺序，Worker在等待期间按R-052帮助，但 helped Callable 不可抢占使实际返回可越过timeout。 例外边界：Direct Self-Wait在读取期限或Helping前抛logic_error。
- `[R-059]` 每个Worker的Helping嵌套深度必须受正数 `max_helping_depth` 限制且默认64，超限在启动下一层帮助前抛 `helping_depth_exceeded`；Helping始终使用source Runtime正常eligibility，Graceful仅推进Drain Closure，Immediate不得first-start新Task。 例外边界：Immediate可运行R-075规定的already-started Coroutine resume segment。

## Test-first seam

- Public seam: 同步 Task result 获取。；无期限同步完成观察。；TaskHandle 有界同步观察。；get/wait/wait_for/GraphRun同步Helping。
- RED evidence: 构造 single-worker nested wait、direct self-wait、depth overflow、shutdown 中 helping 和 timeout/complete 边界竞态。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-052]` 等待不创建补偿线程或执行foreign Runtime工作；动态环允许永久阻塞。
- [x] `[R-055]` wait后仍可完整get，多等待者最终观察同一完成。
- [x] `[R-056]` TimedOut 后Task继续，稍后wait/get仍可观察真实Outcome。
- [x] `[R-059]` 深层同步组合确定性失败而不篡改目标Task，Immediate不借Helping启动新工作。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-052, R-055, R-056, R-059
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-047, D-048, D-049, D-050, D-051, D-061, D-062, D-073, D-063, D-064, D-065, D-066, D-078, D-079, D-080
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-052 | `tests/test_helping_wait.cpp::test_R052_single_worker_nested_helping_wait`、`test_R052_direct_self_wait_rejected` — 证明单 Worker 下嵌套 `get()` 通过 Helping Wait 自动协作执行队列中子任务，无死锁；Direct Self-Wait 在副作用前严格抛出 `std::logic_error`。 | 运行期 RED：单 worker 下嵌套 get 自锁，self-wait 未检测。 | 单 worker fork-join 正常执行返回，self-wait 正确抛出 logic_error。 |
| R-055 | `tests/test_helping_wait.cpp::test_R055_wait_reuses_helping` — 证明 `wait()` 复用 Helping Wait 调度路径，并在真实 Terminal Outcome 发布后正常返回，不篡改或消费结果。 | 编译期 RED：wait() 未接入 caller-relative helping 路径。 | wait() 触发 helping 并等待目标完成，多次调用安全。 |
| R-056 | `tests/test_helping_wait.cpp::test_R056_wait_for_timeout_and_helping` — 证明 `wait_for` 超时仅返回 `TimedOut`，不取消任务或伪造终态，任务继续执行并最终成功产出 Outcome。 | 运行期 RED：wait_for 未在 Worker 路径上正确有界推进。 | TimedOut 后任务完好，后续 get 正确获取计算结果。 |
| R-059 | `tests/test_helping_wait.cpp::test_R059_helping_depth_limit` — 证明嵌套 Helping Wait 深度受 `max_helping_depth` 严格限制，超限在进入下一层前同步抛出 `helping_depth_exceeded`，被帮助任务未受破坏；Immediate 停机模式下不启动新任务。 | 运行期 RED：无深度限制引发无界递归。 | 达到指定阈值时确定性抛出 `helping_depth_exceeded`。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 27 tests in 34.696s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.263s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 10`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `e90f008`）：
  - Standards 轴：`TaskExecutionContextGuard` 与 RAII depth guard 实现异常安全的状态跟踪与深度回滚；`perform_caller_wait` 统一调度 Unbounded/Helping wait；`helping_depth_exceeded` 异常设计标准；符号导出与隐藏严格一致。
  - Spec 轴：R-052（Helping Wait 与 Direct Self-Wait 拦截）、R-055（wait 复用 Helping）、R-056（wait_for 超时保护与非抢占）、R-059（max_helping_depth 与 Immediate 停机合规）100% 满足 Approved Spec 及相关 ADR/决策。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

