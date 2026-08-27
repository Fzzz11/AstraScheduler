# AST-011 — 发布一致的 TaskState、Terminal Outcome 与重复 get

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-049, R-050, R-051, R-057, R-060)
Milestone: v0.1.0
Blocked by: AST-009
Status: ready-for-agent
Claimed by: None

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

- [ ] `[R-049]` 不存在已见终态却读不到Outcome或同一Task副本看到不同Outcome的窗口。
- [ ] `[R-050]` 多个Handle副本可重复观察相同异常或取消，不会终止Worker线程。
- [ ] `[R-051]` 保留任一Handle即可稳定引用Value；临时Handle调用get在编译期失败。
- [ ] `[R-057]` 空对象不会伪装Task状态，有效副本并发观察同一单调生命周期。
- [ ] `[R-060]` v0.1 尚未启用 Metrics/Trace 时，未观察异常不会产生隐藏输出、调用终止处理或改变 Task/Worker 执行；启用观测面后的计数与 Trace 证据留给 AST-048。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-049, R-050, R-051, R-057, R-060
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-044, D-071, D-045, D-057, D-076, D-067, D-068, D-069, D-070, D-072, D-073, D-153, D-081, D-082, D-120, D-151
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending
