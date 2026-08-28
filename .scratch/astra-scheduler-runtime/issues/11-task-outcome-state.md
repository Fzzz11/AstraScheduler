# AST-011 — 发布一致的 TaskState、Terminal Outcome 与重复 get

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-049, R-050, R-051, R-057, R-060)
Milestone: v0.1.0
Blocked by: AST-009
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-049 [primary] — Terminal Outcome 与终态一致发布；source: D-044, D-071
- R-050 [primary] — 异常和取消通过 get 重复传播；source: D-045, D-057
- R-051 [primary] — get 的结果引用由左值 Handle 持有；source: D-076
- R-057 [primary] — TaskHandle 空状态、TaskState 与并发边界固定；source: D-067, D-068, D-069, D-070, D-071, D-072, D-073, D-153
- R-060 [supporting] — 未观察失败仅按启用观测面诊断；source: D-081, D-082, D-120, D-151

## What to build

实现公开生命周期投影、Value/Exception/Cancelled 单次发布、左值 Handle 持有的结果引用和可重复异常/取消传播。

## Invariants

- `[R-049]` 每个 Task 必须恰好一次发布不可变 Value、Exception 或 Cancelled Terminal Outcome，并在同一 completion publication 中使对应 `Succeeded/Failed/Cancelled` TaskState 与全部等待者可见。 例外边界：尚未成功 admission 的工作没有 TaskState/Outcome。
- `[R-050]` Worker 边界必须捕获逃逸的任意 C++ 异常并保存 `std::exception_ptr`；有效左值 `get()` 必须按原动态类型重复重抛 Exception，Cancelled Outcome 必须重复抛 `astra::task_cancelled`，且异常不得逃出 Worker entry。 例外边界：以 `task_cancelled` 逃出用户执行按 R-054 转为 Cancelled，而非 Failed。
- `[R-051]` `TaskHandle<T>::get() const &` 必须在完成后返回共享 `const T&`，`TaskHandle<void>::get() const &` 返回 `void`，两者的 rvalue overload 必须删除且不得提供消费式 `take()`。 例外边界：Exception/Cancelled 按 R-050 抛出。
- `[R-057]` TaskHandle 必须支持 default/moved-from empty 与 `valid()`；空对象的 get/wait/wait_for/state/id 抛logic_error而 request_cancel为no-op；有效Task的公共State仅为 Waiting/Ready/Running/Suspended/Succeeded/Failed/Cancelled，`state()`非阻塞线性化，稳定对象操作可并发而同一对象reassociation/destruction需调用方同步。 例外边界：内部queue/claim/publication瞬态不公开。
- `[R-060]` Exception Outcome 在首次get/await传播前必须幂等标记observed；最终shared state释放时若仍未观察，仅在Metrics Basic/Detailed增加稳定 `unobserved_failures`，并仅在活动Trace可用时尽力发事件，不得terminate、默认日志、回调、级联取消或维持Metrics Off隐藏计数。 例外边界：wait/state/wait_for不标记observed。

## Test-first seam

- Public seam: Callable、Coroutine 与 Graph Node Task identity。；`TaskHandle<T>::get() const &` 与 `TaskHandle<void>`。；Value Outcome 的公共访问与 lifetime。；TaskHandle value semantics与公共TaskState。；普通Task与R-072的Graph真实Failed Node。
- RED evidence: 先写 value/void/reference、异常、取消、空 Handle、并发 observer 与重复 `get()` 的状态/结果一致性测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-049]` 不存在已见终态却读不到Outcome或同一Task副本看到不同Outcome的窗口。
- [x] `[R-050]` 多个Handle副本可重复观察相同异常或取消，不会终止Worker线程。
- [x] `[R-051]` 保留任一Handle即可稳定引用Value；临时Handle调用get在编译期失败。
- [x] `[R-057]` 空对象不会伪装Task状态，有效副本并发观察同一单调生命周期。
- [x] `[R-060]` v0.1 尚未启用 Metrics/Trace 时，未观察异常不会产生隐藏输出、调用终止处理或改变 Task/Worker 执行；启用观测面后的计数与 Trace 证据留给 AST-048。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-049, R-050, R-051, R-057, R-060
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-044, D-071, D-045, D-057, D-076, D-067, D-068, D-069, D-070, D-072, D-073, D-153, D-081, D-082, D-120, D-151
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-049 | `tests/test_task_outcome_state.cpp::test_R049_R051_value_outcome_and_state_consistency` — 证明 Value/Exception/Cancelled 与 `Succeeded/Failed/Cancelled` 状态在同一原子操作中发布，多个 Handle 副本观察结果严格一致。 | 编译期 RED：未定义不可变结果与状态发布一致性通道。 | Value/Exception/Cancelled 与 Succeeded/Failed/Cancelled 原子发布全部通过。 |
| R-050 | `tests/test_task_outcome_state.cpp::test_R050_exception_repeated_propagation`、`test_R050_R054_task_cancelled_propagation` — 证明用户异常与 `task_cancelled` 被 Worker 边界安全捕获不逃逸，多 Handle 副本调用 `get()` 可重复按原动态类型抛出相同异常或 `task_cancelled`。 | 编译期 RED：缺乏 exception_ptr 重复重抛机制与 task_cancelled 传播。 | 多 Handle 重复 get 保持抛出同一异常与取消类，Worker 存活。 |
| R-051 | `tests/test_task_outcome_state.cpp::test_R049_R051_value_outcome_and_state_consistency` — 证明 `TaskHandle<T>::get() const &` 返回指向共享不可变内存的 `const T&`，`TaskHandle<void>::get() const &` 返回 `void`，右值重载编译期 delete。 | 编译期 RED：无左值引用限定返回与 const T& 语义。 | 稳定引用底层存储且地址一致，临时调用在编译期拒绝。 |
| R-057 | `tests/test_task_outcome_state.cpp::test_R057_empty_handle_contract`、`test_R057_concurrent_observers`、`test_R055_R056_wait_and_wait_for` — 证明空/moved-from Handle 统一抛 `std::logic_error`，`request_cancel()` 为 no-op；`TaskState` 七态模型非阻塞读取线性化；`wait()` 与 `wait_for()` 稳定支持无界/有界观察。 | 编译期 RED：无 TaskState 七态枚举、wait/wait_for 及空对象契约。 | 空对象严格抛出 logic_error，wait/wait_for/state 观察及多线程并发全部通过。 |
| R-060 | `tests/test_task_outcome_state.cpp::test_R060_unobserved_exception_safe_destruction` — 证明丢弃带有未观察异常的 Handle 析构时不触发 `std::terminate` 或未定义异常泄露。 | 运行期 RED：未观察异常可能在析构时导致进程中止。 | 析构未观察异常句柄安全，无 terminate 发生。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 26 tests in 38.879s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.270s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 9`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `0268e99248630e48a3246a8948bbb8517503cf8c`）：
  - Standards 轴：`TaskState` 七态枚举与 `WaitResult` 布局严谨；`TaskSharedState` 实现单次终态原子发布；`get() const &` 左值限定返回 `const T&` 消除临时悬垂；Worker 异常边界捕获彻底；`wait()`/`wait_for()` 语义标准。
  - Spec 轴：R-049（不可变 Outcome 与状态发布一致性）、R-050（异常与取消重复传播）、R-051（const T& 引用）、R-057（空 Handle 契约与 TaskState 快照）、R-060（未观察异常安全析构）100% 符合 Approved Spec 与对应架构决策。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。
