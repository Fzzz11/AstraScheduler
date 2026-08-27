# AST-012 — 实现 Unbounded/Helping wait 与 timeout 边界

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-052, R-055, R-056, R-059)
Milestone: v0.1.0
Blocked by: AST-008, AST-011
Status: ready-for-agent
Claimed by: None

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

- [ ] `[R-052]` 等待不创建补偿线程或执行foreign Runtime工作；动态环允许永久阻塞。
- [ ] `[R-055]` wait后仍可完整get，多等待者最终观察同一完成。
- [ ] `[R-056]` TimedOut 后Task继续，稍后wait/get仍可观察真实Outcome。
- [ ] `[R-059]` 深层同步组合确定性失败而不篡改目标Task，Immediate不借Helping启动新工作。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-052, R-055, R-056, R-059
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-047, D-048, D-049, D-050, D-051, D-061, D-062, D-073, D-063, D-064, D-065, D-066, D-078, D-079, D-080
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending

