# AST-004 — 固定 Scheduler 公共 policy、状态、逻辑 ID 与 capability

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-098, R-099, R-100, R-101)
Milestone: v0.1.0
Blocked by: AST-002, AST-003
Status: done
Claimed by: Antigravity agent (2026-08-27)

## Rules and decisions

- R-098 [primary] — SchedulerOptions 只公开稳定policy且startup冻结；source: D-078, D-157
- R-099 [primary] — Scheduler status 是state/mode成对快照；source: D-160
- R-100 [primary] — 公共逻辑ID强类型且不wrap/reuse；source: D-153, D-161
- R-101 [primary] — SchedulerCapabilities 报告实际Local Deque backend；source: D-101, D-167, D-162

## What to build

定义稳定 `SchedulerOptions`、冻结后的 resolved snapshot、成对 status snapshot、强类型不复用逻辑 ID，以及真实 Local Deque backend capability；v0.1 报告 `None`。

## Invariants

- `[R-098]` SchedulerOptions必须使用 `recommended_worker_count()`、external capacity 65536、Reject、helping64、local burst64、steal probes8、Metrics Basic及空TraceCollector默认；所有size值必须大于0且unknown enum拒绝，options在注册/ID/Worker前验证并冻结，spin/deque/timer/priority内部tuning不得公开。 例外边界：recommended_worker_count的hardware_concurrency为0时返回1。
- `[R-099]` 有效Scheduler的 `status()`必须非阻塞、无副作用且一次线性化返回仅有Running+None、Stopping+Graceful/Immediate、Stopped+最终mode的pair；不得提供独立is_running/is_stopped/mode getter，snapshot可立即过时且不授予admission能力，空Scheduler抛logic_error。 例外边界：process Finalization state由R-095观察。
- `[R-100]` RuntimeId、TaskId、GraphRunId与NodeId必须是default-zero-invalid、trivially-copyable强值类型，支持valid/equality/order/hash且无隐式整数/指针转换；有效Scheduler::runtime_id、TaskHandle::id、GraphRun::id与GraphReport::run_id返回对应稳定值。Runtime/Task/GraphRun sequence为checked nonzero monotonic不wrap，NodeId仅graph-local；完整运行Node关联GraphRunId+NodeId+TaskId，ID不授予lookup/control/lifetime。 例外边界：sequence gap允许，跨进程不承诺唯一。
- `[R-101]` 有效Scheduler的 `capabilities()`必须返回不可由用户aggregate-initialize的trivially-copyable immutable `SchedulerCapabilities`，其 `local_deque_backend()`为 `LocalDequeBackend::{None,Locked,ChaseLevLockFree}`之一且 `lock_free_local_deque()`仅最后一种为true；v0.1为None、v0.2/fallback为Locked，Stopped后保留且不得运行时切换或按版本推断，空Scheduler抛logic_error。 例外边界：该能力不声称整个Runtime lock-free。

## Test-first seam

- Public seam: Scheduler startup配置与Metrics/Benchmark resolved options。；Scheduler lifecycle观察。；Scheduler/Handle/GraphReport/Metrics/Trace/Benchmark identity。；Runtime、Trace metadata与Benchmark artifact。
- RED evidence: 先写 public compile tests、Options 修改不回写 Runtime、状态成对观察、ID 不混用及 v0.1 capability 测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-098]` invalid配置在无Runtime副作用前抛invalid_argument，调用方后改原options不影响Runtime。
- [x] `[R-099]` 并发shutdown时不返回撕裂pair，submit仍以自身transaction决定结果。
- [x] `[R-100]` 地址复用不改变身份，耗尽在startup/admission前抛overflow_error而不复用。
- [x] `[R-101]` 同版本不同atomic平台可诚实报告不同backend，artifact复用同一snapshot。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-098, R-099, R-100, R-101
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-078, D-157, D-160, D-153, D-161, D-101, D-167, D-162
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-27；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-098 | `tests/test_scheduler_contract.cpp::test_R098_*`、`tests/consumer/main.cpp` — `SchedulerOptions` 各字段默认值断言（`recommended_worker_count()` >= 1、capacity 65536、Reject、helping 64、local burst 64、steal probes 8、Basic、trace_collector 为空）；5 个 size 字段为 0 及未知 enum 值均抛出 `std::invalid_argument`；构造后原 options 变量修改不影响 Runtime 配置。 | 编译期 RED：`fatal error: astra/scheduler_options.hpp: No such file or directory`（选项定义与校验未实现）。 | `100% tests passed, 0 tests failed out of 2`；`check_cmake_package.py` 19 项全部通过。 |
| R-099 | `tests/test_scheduler_contract.cpp::test_R099_*`、`tests/consumer/main.cpp` — `Scheduler::status()` 返回成对 `SchedulerStatus`；初始状态为 `Running + None`；枚举 5 个合法状态对并拦截非法组合；空/moved-from Handle 抛出 `std::logic_error`；概念静态断言确认无 `is_running()`、`is_stopped()`、`mode()` 独立 getter；单字原子状态编码保证多线程并发读取不返回撕裂 pair。 | 编译期 RED：`fatal error: astra/status.hpp: No such file or directory`（状态结构与查询接口未建立）。 | `test_R099_*` 与 consumer 运行期状态断言通过。 |
| R-100 | `tests/test_scheduler_contract.cpp::test_R100_*`、`tests/consumer/main.cpp` — `RuntimeId`、`TaskId`、`GraphRunId`、`NodeId` 均为 trivially-copyable、default-zero-invalid、explicit bool 强类型，静态断言拒绝与整数/指针隐式转换；提供 equality、`<`、`<=>` 全序及 `std::hash` 特化；多实例 `Scheduler` 分配单调唯一 `RuntimeId`，copy 共享，move 后源为 invalid；`allocate_runtime_id` 耗尽时抛 `std::overflow_error` 且不 wrap/reuse。 | 编译期 RED：`fatal error: astra/id.hpp: No such file or directory`（强类型 ID 未定义）。 | `test_R100_*` 强类型约束、单调性与哈希测试全部通过。 |
| R-101 | `tests/test_scheduler_contract.cpp::test_R101_*`、`tests/consumer/main.cpp`、`tools/check_cmake_package.py::AST004PublicContractGates` — `SchedulerCapabilities` 为可平凡复制非 aggregate 类型；v0.1.0 报告 `LocalDequeBackend::None` 且 `lock_free_local_deque() == false`；空 Handle 抛出 `std::logic_error`；共享库动态符号导出通过严格隐藏校验。 | 编译期 RED：`fatal error: astra/capabilities.hpp: No such file or directory`（能力快照类型缺失）。 | `test_R101_*`、consumer 与 package gate 验证全部通过。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 19 tests in 14.132s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.205s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake -S . -B build/wsl-gcc-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 2`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `ffbb7ef`，改动为工作区未提交文件）：
  - Standards 轴：全头文件包含 include guards 与 64-bit Linux 静态架构约束（`export.hpp`）；头文件均安装至 `include/astra/` 目录；`-fvisibility=hidden` 与 `-fvisibility-inlines-hidden` 有效阻止内部模板符号外泄；`Scheduler::Impl` 标记为 `ASTRA_NO_EXPORT`；动态库导出符号经 `nm -D` 严格对齐公开 API 集合；通过 `Threads::Threads` 经 CMake Config 文件正确传播给 consumer。
  - Spec 轴：R-098（SchedulerOptions 稳定字段及默认值、参数正数/未知枚举前置校验、冻结快照）、R-099（成对非阻塞状态快照、无独立 check-then-act getter、空实例抛 `std::logic_error`）、R-100（4 类强逻辑 ID、default-zero-invalid、全序与 std::hash、不 wrap/reuse 单调生成）、R-101（不可 aggregate 初始化能力对象、v0.1.0 报告 None）均 100% 对齐 Approved Spec 与决策台账。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。
