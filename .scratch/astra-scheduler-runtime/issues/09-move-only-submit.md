# AST-009 — 实现 move-only submit 与共享 TaskHandle 基础面

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-048, R-058, R-102)
Milestone: v0.1.0
Blocked by: AST-008
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-048 [primary] — TaskHandle 是共享任务 capability；source: D-041, D-042, D-043, D-067, D-153
- R-058 [primary] — submit 结果类型与基础结果 API 受限；source: D-074, D-075, D-076, D-077
- R-102 [primary] — submit decay-own并一次性rvalue调用move-only工作；source: D-059, D-165

## What to build

`submit` decay-own Callable/args 并以 stored rvalue 恰好调用一次，支持 move-only target/arg，返回可复制的同一 Task identity capability 和受限结果 API。

## Invariants

- `[R-048]` `submit()` 从 v0.1.0 起必须返回可复制、可移动的 `TaskHandle<T>`；所有副本关联同一 TaskId/完成状态，最后一个 Handle 销毁不得隐式取消或重新提交任务。 例外边界：moved-from/default Handle 为空；Graph Node 没有 per-node TaskHandle。
- `[R-058]` submit选择R-054的invocation后，只能产生 `TaskHandle<void>` 或去顶层cv且可移动构造的对象 `TaskHandle<T>`；裸引用与完全immovable结果必须编译期拒绝，move-only结果必须支持，稳定API不得增加take/try_get/exception/OutcomeView第二套结果通道。 例外边界：`std::reference_wrapper`与指针作为显式值类型可用，lifetime由调用方承担。
- `[R-102]` submit/try_submit必须对F/Args以decay_t和完美转发构造owned capture，并以stored rvalue恰好调用一次，支持move-only target/argument和operator()&&；真实引用仅通过std::ref显式表达，traits与R-054 token fallback必须基于同一stored-rvalue expression，copy-only std::function不得缩窄能力。 例外边界：Coroutine frame按R-073 ownership转移而不二次capture。

## Test-first seam

- Public seam: 普通 Callable Task 的公共 Handle 与 logical identity。；submit/spawn结果类型和TaskHandle公共surface。；v0.1起普通Task与Graph emplace的一次性work storage。
- RED evidence: 先写 `operator()&&`、move-only 参数、`std::ref`、Handle copy/empty 及非法返回形态的编译与运行测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-048]` 复制Handle不复制执行，丢弃全部Handle后已接受Task仍能完成。
- [ ] `[R-058]` 编译期矩阵稳定支持void/copyable/move-only并拒绝reference/immovable。
- [ ] `[R-102]` move-only Callable/unique_ptr参数可提交，lvalue-only target无wrapper时编译期拒绝。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-048, R-058, R-102
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-041, D-042, D-043, D-067, D-153, D-074, D-075, D-076, D-077, D-059, D-165
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending

