# AST-009 — 实现 move-only submit 与共享 TaskHandle 基础面

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-048, R-058, R-102)
Milestone: v0.1.0
Blocked by: AST-008
Status: done
Claimed by: Antigravity agent (2026-08-28)

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

- [x] `[R-048]` 复制Handle不复制执行，丢弃全部Handle后已接受Task仍能完成。
- [x] `[R-058]` 编译期矩阵稳定支持void/copyable/move-only并拒绝reference/immovable。
- [x] `[R-102]` move-only Callable/unique_ptr参数可提交，lvalue-only target无wrapper时编译期拒绝。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-048, R-058, R-102
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-041, D-042, D-043, D-067, D-153, D-074, D-075, D-076, D-077, D-059, D-165
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-048 | `tests/test_move_only_submit.cpp::test_R048_shared_task_handle_and_lifetime` — 证明 `TaskHandle` 支持复制/移动、所有副本共享同一 `TaskId`、多次 `.get()` 不会重复执行任务，且在外部 Handle 全部销毁后已接受任务仍能正常执行完毕。 | 编译期 RED：未提供 `TaskHandle` 模板与 `submit` 接口。 | `test_R048_*` 测试全过，Handle 共享与生命周期完全合规。 |
| R-058 | `tests/test_move_only_submit.cpp::test_R058_result_types_and_exceptions`、`tools/check_cmake_package.py::AST009MoveOnlySubmitGates` — 证明支持 `TaskHandle<void>`、`TaskHandle<copyable>`、`TaskHandle<move-only>`，支持异常传播；编译期静态断言拒绝裸引用（`T&`、`const T&`、`T&&`），并通过 `get() const && = delete` 禁止临时/右值 Handle 调用 `.get()` 防止引用悬垂。 | 编译期 RED：缺乏结果类型推导与左值限定结果接口。 | 支持 void/copyable/move-only，拒绝引用与右值 get()，门禁全部通过。 |
| R-102 | `tests/test_move_only_submit.cpp::test_R102_move_only_callable_and_arguments` — 证明支持仅有 `operator()() &&` 的 move-only Callable、支持 `std::unique_ptr` move-only 参数、支持 `std::ref` 显式引用传递，且在普通调用不可行时自动注入 `std::stop_token` fallback（`D-059`）。 | 编译期 RED：std::function 无法承载 move-only 工作。 | 成功提交并执行 move-only functor、unique_ptr 参数、std::ref 与 stop_token。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 24 tests in 18.180s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.191s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 7`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `5a1e8e888062dfabe9f527da031e6df6a1c4562f`）：
  - Standards 轴：`TaskHandle<T>` 与 `TaskHandle<void>` 采用引用计数共享状态 `TaskSharedState` 实现共享生命周期；`submit` 采用模板化 `TaskInvokerModel` 擦除类型并支持完美转发与 move-only callable/args，无 `std::function` 复制约束缺陷；`get() const &` 左值限定有效防止临时对象悬垂；符号导出严格遵循 Linux GCC visibility 约束。
  - Spec 轴：R-048（共享 Handle 语义与无隐式取消）、R-058（void/对象/move-only 结果、拒绝裸引用/immovable、lvalue-only get）、R-102（一次性 rvalue 调用 move-only 工作、std::ref 与 stop_token 注入）100% 满足 Approved Spec 及 D-041/D-059/D-074/D-075/D-076/D-165 决策。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

