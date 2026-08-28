# AST-018 — 固定 FinalizationControl 公共 capability surface

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-035, R-036, R-043, R-044, R-045, R-046)
Milestone: v0.1.0
Blocked by: AST-004, AST-007
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-035 [primary] — Finalization 操作只通过有效控制对象组织；source: D-030
- R-036 [primary] — FinalizationControl 是共享且析构无策略的 capability；source: D-030, D-031
- R-043 [primary] — Finalization 只由应用显式编排；source: D-038
- R-044 [primary] — Finalization 公共类型和结果枚举固定；source: D-039
- R-045 [primary] — Finalization 四操作签名固定；source: D-039
- R-046 [primary] — Finalization 不暴露同义或重置接口；source: D-039

## What to build

定义有效共享控制对象、固定结果枚举与四个操作；析构无策略，不暴露 reset/restart/同义 shutdown，且只能由应用显式编排。

## Invariants

- `[R-035]` `begin_finalization()` 必须是创建有效 `FinalizationControl` 的唯一公共入口；控制对象不得公开默认构造，`wait()`、`wait_for()` 与 `request_immediate()` 只能作为该对象的操作提供。
- `[R-036]` `FinalizationControl` 必须可复制、可移动并支持多个线程数据竞争安全地操作同一 Finalization Completion；销毁任意或全部副本不得阻塞、取消、暂停、恢复注册、停止后台工作或隐式认领 join。 例外边界：`wait()`/`wait_for()` 的 Worker caller 限制见 R-039/R-040。
- `[R-043]` AstraScheduler 不得通过 `atexit`、进程级静态析构、最后一个 Handle/Runtime 析构或空闲超时自动触发 begin、wait 或 Immediate escalation；动态库卸载前，非 Worker 调用方必须先观察 `Completed` 并结束所有仍会调用库代码的公共对象，`TimedOut` 不得作为卸载许可；不可逆公共测试不得依赖 reset/restart。 例外边界：调用方可以在 TimedOut 后自行终止整个进程，但这不形成任务清理或 trace flush 保证。
- `[R-044]` 公共 C++20 Interface 必须在 `astra` 命名空间提供 `FinalizationControl` 与 `enum class FinalizationWaitResult { Completed, TimedOut }`。 例外边界：枚举底层整数类型和类内存布局未固定。
- `[R-045]` 公共 Interface 必须提供 `[[nodiscard]] FinalizationControl begin_finalization() noexcept`、`void FinalizationControl::wait() const`、接受 `std::chrono::duration<Rep, Period>` 的 `[[nodiscard]] FinalizationWaitResult wait_for(...) const` 与 `void request_immediate() const noexcept`；控制对象的 copy/move/析构必须为 `noexcept`，且没有公共默认构造。 例外边界：`wait()`/`wait_for()` 不带 `noexcept`，以承载 R-039/R-040 的 `std::logic_error`。
- `[R-046]` 公共 Interface 不得提供全局 `wait_finalization()`、`finalize_now()`、control accessor、reset/restart、`wait_until()`、stop-token wait、coroutine await 或 progress callback 作为当前规格的一部分。 例外边界：未来 accepted decision 与新规则可以扩展非冲突能力。

## Test-first seam

- Public seam: 进程级 Finalization 公共 Interface。；所有由幂等 begin 返回的控制对象及其副本。；进程退出、静态生命周期、动态库卸载与全局测试隔离。；Finalization 公共头文件。；Finalization 公共 C++20 API surface。；当前 Finalization 公共 API surface。
- RED evidence: 先写 public compile tests、invalid/default 状态、复制共享、析构无动作和被禁止接口的负向编译测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-035]` 没有 begin 返回值时，公共类型系统中不存在合法 wait/upgrade 调用路径。
- [x] `[R-036]` 一个副本升级后所有副本观察同一过程；全部副本销毁后后台仍继续。
- [x] `[R-043]` 最后 Scheduler 消失只使 Reaper 空闲；卸载测试只有 Completed 且对象停止调用后通过。
- [x] `[R-044]` 调用方可使用稳定限定名比较 Completed/TimedOut，未暴露其他必需结果值。
- [x] `[R-045]` 编译期 API tests 验证签名、属性、构造能力和异常规格。
- [x] `[R-046]` public header/API inventory 只出现 R-044/R-045 的 Finalization 能力。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-035, R-036, R-043, R-044, R-045, R-046
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-030, D-031, D-038, D-039
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-035 | `tests/test_finalization_control_api.cpp::test_R035_R044_R045_type_traits_and_signatures` — 验证 `FinalizationControl` 无公共默认构造，`begin_finalization()` 是唯一合法创建入口。 | 编译期 RED：允许默认构造或存在未通过控制对象的全局 wait。 | 仅 `begin_finalization()` 返回有效 capability 对象。 |
| R-036 | `tests/test_finalization_control_api.cpp::test_R035_R036_runtime_control_semantics` — 验证 `FinalizationControl` 为共享 capability，可复制可移动，副本析构无策略/无副作用。 | 运行期 RED：副本析构触发隐式取消或阻塞。 | 副本析构对共享控制状态完全无破坏，后台继续。 |
| R-043 | `tests/test_finalization_control_api.cpp::test_R035_R036_runtime_control_semantics` — 验证无 `atexit` 自动触发，仅由调用方显式调用控制对象。 | 设计/编译期负向检查：无任何全局自动触发 hook。 | 严格由用户显式编排。 |
| R-044 | `tests/test_finalization_control_api.cpp::test_R035_R044_R045_type_traits_and_signatures` — 验证命名空间 `astra` 下提供 `FinalizationControl` 与 `FinalizationWaitResult{Completed, TimedOut}`。 | 编译期 RED：缺少枚举或命名空间不符。 | 枚举与类型完全符合规范定义。 |
| R-045 | `tests/test_finalization_control_api.cpp::test_R035_R044_R045_type_traits_and_signatures` — 验证四个操作签名、`noexcept` 规格（copy/move/dtor/begin/request_immediate 均为 noexcept；wait/wait_for 不带 noexcept）。 | 编译期 RED：noexcept 规格或签名不匹配。 | 签名及 noexcept 规格 100% 匹配。 |
| R-046 | `tests/consumer/main.cpp` 与 `tools/check_cmake_package.py` — 验证无额外暴露同义或重置接口（如 reset/restart/wait_until）。 | 符号表与头文件检查：无任何多余符号泄露。 | 公开 capability surface 极简且稳定。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 33 tests in 40.006s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.229s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 16`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `17be027`）：
  - Standards 轴：`include/astra/finalization.hpp` 严格遵循 64-bit Linux export 规范；`FinalizationControl` 为共享智能指针句柄，copy/move/dtor 均为 `noexcept`。
  - Spec 轴：R-035, R-036, R-043, R-044, R-045, R-046 全部门禁与接口签名 100% 符合 Approved Spec。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

