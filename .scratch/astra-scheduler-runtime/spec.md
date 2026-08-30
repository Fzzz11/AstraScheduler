# AstraScheduler Runtime Spec

Status: approved
Approved by: project owner（user）
Approved at: 2026-08-30

## Problem Statement

AstraScheduler 面向 Linux-only 的现代 C++20 任务调度场景，最终范围覆盖 Global Runtime 基线、Work Stealing、Chase-Lev Deque、TaskHandle/Result、Cancellation、DAG、Coroutine/Timer、Priority/Deadline、Metrics、Chrome Trace、Benchmark，以及独立 Runtime State、Reaper 与进程级 Finalization。本规格把全项目已确认行为整理为稳定规则，使后续版本 Tickets、测试和实现不再依赖聊天上下文。

仓库当前只有设计文档、决策台账与 ADR，没有实现代码。本规格只把 accepted decisions 直接支持的行为提升为规范性规则；实现细节和已明确延期能力不因出现在参考设计中而成为要求。

## Goals

- 建立跨版本一致的 Task、Admission、Scheduling、Lifecycle 与 Observability 语义。
- 固定最终 Supported Configuration 仅为64-bit Linux，并把本机开发与验证入口统一到WSL Linux。
- 建立 v0.1.0 Global baseline 到 v1.0 stable source API 的纵向里程碑关系。
- 建立 Scheduler Handle、Runtime State、Reaper Service 与 Finalization Control 的所有权和完成边界。
- 固定 DAG、Coroutine/Timer、Priority/Deadline、Metrics/Trace 与 Benchmark 的组合边界。
- 为每条已确认行为提供稳定的 `R-xxx`、来源与可观察验证点。
- 为后续按版本拆分 Tickets 提供无需聊天上下文的输入。

## Non-goals

- Dynamic Worker Scaling、CPU affinity、NUMA、lock-free Global Queue、Timer Wheel、I/O Runtime/io_uring、Distributed/GPU Runtime 不在 v1.0 范围。
- Typed DAG dataflow、动态 reprioritize/priority donation、completion callback API 与在线 wait-for graph/cycle resolution 不在当前范围。
- foreign awaitable 强制取消、线程强杀、硬实时 deadline/wakeup 和跨版本/跨工具链 C++ ABI 不在保证范围。
- 多个 DSO 各自静态嵌入一份实现时的 process-wide singleton 语义不受支持。
- Windows/MSVC、macOS、其他非Linux OS与32-bit目标均不在支持范围；偶然编译成功不形成兼容、正确性、性能或生命周期承诺。
- 带有活动Astra线程/Runtime的进程执行`fork()`后的子进程语义不受支持。
- 本规格不拆分或发布 Tickets；版本拆票约束见 R-005。

## Domain and Actors

| Term / Actor | Meaning |
|---|---|
| Internal Submission | 调用方正在同一个 Scheduler 的执行上下文中运行任务时，向该 Scheduler 发起的任务提交。 |
| External Submission | 不属于 Internal Submission 的任务提交，包括应用线程提交以及另一个 Scheduler 的 Worker 发起的提交。 |
| Drain Work Closure | Graceful shutdown 核算的传递工作集合，由关停前已接受任务及其获准的 Internal Submission 组成。 |
| Shutdown Mode | Scheduler 处于 `Stopping` 时采用的 Graceful 或 Immediate 策略；它与 SchedulerState 是两个维度。 |
| Shutdown Completion | 全部 Worker 退出并 join、`Stopped` 发布后达成的单次 Scheduler 完成边界。 |
| Scheduler Handle | 应用访问 Scheduler Runtime 的前台所有权对象，其生命周期可以早于 Runtime State 结束。 |
| Runtime State | 与 Handle 解耦、由执行和回收路径共享持有的 Runtime 身份。 |
| Reaper / Reaper Service | 非目标 Worker 的生命周期协调者，以及承载该角色的进程级单协调线程服务。 |
| Pending Runtime State | 已移交给 Reaper、尚未达到 Join Ready 的 Runtime State。 |
| Join Ready | Worker loop 已不可逆进入终止收尾、Reaper 可以开始 join 的单调边界。 |
| Reaper Finalization | 永久关闭 Runtime 注册并终结 Reaper Service 的进程级一次性生命周期。 |
| Finalization Completion | 全部 Runtime Shutdown Completion、Reaper 工作清空、coordinator 退出并 join 后的进程级完成边界。 |
| Finalization Control | 观察或升级同一次 Finalization 的共享 capability，不是 Reaper 或 Runtime State 的 owner。 |
| Finalization Escalation | 显式将全部已核算且未完成 Runtime 单向请求为 Immediate 的进程级策略升级。 |

## Decision Coverage

| Decision | Destination |
|---|---|
| D-001 | R-001, R-002, R-003 |
| D-002 | R-006 |
| D-003 | R-007 |
| D-004 | R-004 |
| D-005 | R-005 |
| D-006 | R-008, R-106 |
| D-007 | R-009, R-054, R-075 |
| D-008 | R-010 |
| D-009 | R-011, R-108 |
| D-010 | R-012 |
| D-011 | R-013, R-108 |
| D-012 | R-014, R-015 |
| D-013 | R-016 |
| D-014 | R-017, R-018, R-103, R-105 |
| D-015 | Excluded — superseded by D-017 |
| D-016 | R-019, R-097 |
| D-017 | R-020, R-021 |
| D-018 | R-021, R-022 |
| D-019 | R-023, R-024, R-097 |
| D-020 | R-025, R-026 |
| D-021 | R-027, R-107 |
| D-022 | R-028 |
| D-023 | R-029 |
| D-024 | R-030, R-104 |
| D-025 | Excluded — rejected in favor of D-026 through D-028 |
| D-026 | R-031 |
| D-027 | R-032 |
| D-028 | R-033 |
| D-029 | R-034 |
| D-030 | R-035, R-036 |
| D-031 | R-036 |
| D-032 | R-037 |
| D-033 | R-038 |
| D-034 | R-039 |
| D-035 | R-040 |
| D-036 | R-041 |
| D-037 | R-042 |
| D-038 | R-043, Non-goals |
| D-039 | R-034, R-044, R-045, R-046 |
| D-040 | R-047 |
| D-041 | R-048 |
| D-042 | R-048 |
| D-043 | R-048 |
| D-044 | R-049 |
| D-045 | R-050 |
| D-046 | Excluded — superseded by D-076 |
| D-047 | R-052 |
| D-048 | R-052 |
| D-049 | R-052 |
| D-050 | R-052, R-096 |
| D-051 | R-052, R-096 |
| D-052 | R-053 |
| D-053 | R-053 |
| D-054 | R-053 |
| D-055 | R-053 |
| D-056 | R-054 |
| D-057 | R-050, R-054 |
| D-058 | R-054 |
| D-059 | R-054, R-102 |
| D-060 | R-054 |
| D-061 | R-055 |
| D-062 | R-055 |
| D-063 | R-056 |
| D-064 | R-056 |
| D-065 | R-056 |
| D-066 | R-056 |
| D-067 | R-048, R-057 |
| D-068 | R-057 |
| D-069 | R-057 |
| D-070 | R-057 |
| D-071 | R-049, R-057 |
| D-072 | R-057 |
| D-073 | R-055, R-057 |
| D-074 | R-058 |
| D-075 | R-058 |
| D-076 | R-051, R-058 |
| D-077 | R-058 |
| D-078 | R-059, R-098 |
| D-079 | R-059 |
| D-080 | R-059 |
| D-081 | R-060 |
| D-082 | R-060 |
| D-083 | R-061 |
| D-084 | R-061 |
| D-085 | R-061 |
| D-086 | R-061 |
| D-087 | R-062 |
| D-088 | R-062 |
| D-089 | R-062 |
| D-090 | R-063 |
| D-091 | R-063 |
| D-092 | R-063 |
| D-093 | R-064 |
| D-094 | R-065 |
| D-095 | R-065 |
| D-096 | R-065 |
| D-097 | R-066 |
| D-098 | R-066 |
| D-099 | R-067 |
| D-100 | R-067 |
| D-101 | R-068, R-101 |
| D-102 | R-068 |
| D-103 | R-068 |
| D-104 | R-069 |
| D-105 | R-069 |
| D-106 | R-070 |
| D-107 | R-070 |
| D-108 | R-071 |
| D-109 | R-071 |
| D-110 | R-071 |
| D-111 | R-072 |
| D-112 | R-072 |
| D-113 | R-072 |
| D-114 | R-073 |
| D-115 | R-073 |
| D-116 | R-074 |
| D-117 | R-074 |
| D-118 | R-074 |
| D-119 | R-075 |
| D-120 | R-060, R-076 |
| D-121 | R-076 |
| D-122 | R-076 |
| D-123 | R-077 |
| D-124 | R-077 |
| D-125 | R-078 |
| D-126 | R-079 |
| D-127 | R-079 |
| D-128 | R-079 |
| D-129 | R-080 |
| D-130 | R-081 |
| D-131 | R-081 |
| D-132 | R-082 |
| D-133 | R-083 |
| D-134 | R-083 |
| D-135 | R-084 |
| D-136 | R-084 |
| D-137 | R-085 |
| D-138 | R-086 |
| D-139 | R-087 |
| D-140 | R-088, R-109 |
| D-141 | R-089 |
| D-142 | R-090 |
| D-143 | R-091 |
| D-144 | Historical R-092 — superseded by D-167 |
| D-145 | R-093, R-110 |
| D-146 | R-094 |
| D-147 | R-063, R-076, R-083 |
| D-148 | R-095 |
| D-149 | R-096 |
| D-150 | R-090 |
| D-151 | R-060, R-084 |
| D-152 | R-072 |
| D-153 | R-048, R-057, R-087, R-100 |
| D-154 | R-075, R-106 |
| D-155 | R-062, R-097, R-103, R-105 |
| D-156 | R-097, R-104 |
| D-157 | R-098 |
| D-158 | R-086 |
| D-159 | R-107 |
| D-160 | R-099 |
| D-161 | R-069, R-100 |
| D-162 | R-101 |
| D-163 | R-086 |
| D-164 | R-093 |
| D-165 | R-102 |
| D-166 | R-108 |
| D-167 | R-092 (historical), R-101, R-110, R-111, Non-goals, Compatibility |
| D-168 | R-112, `AGENTS.md`, `docs/development.md` |
| D-169 | R-113, R-114, R-115, R-116, R-117 |
| D-170 | R-118 |
| D-171 | R-122 |
| D-172 | R-120 |
| D-173 | R-119 |
| D-174 | R-121 |
| D-175 | R-123 |

## Normative Rules

### R-001 — v0.1.0 使用全局注入队列基线
Status: active
Supersedes: None
Superseded by: None
Statement: v0.1.0 的全部 Ready Task 必须通过互斥保护的 Global Injection Queue 调度。
Applies to: v0.1.0 Basic Scheduler。
Exceptions: None。
Source decisions: D-001
Source support: D-001 => 直接确定 v0.1.0 为 mutex Global Injection Queue 的 Basic Scheduler 基线。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: v0.1.0 的调度路径中不存在 Ready Task 绕过 Global Injection Queue 的本地队列路径。

### R-002 — v0.1.0 排除本地队列与任务窃取
Status: active
Supersedes: None
Superseded by: None
Statement: v0.1.0 不得包含 Per-Worker Local Queue、Work Stealing 或 Chase-Lev Deque。
Applies to: v0.1.0 release scope。
Exceptions: 文档、接口 seam 或后续版本预留不构成 v0.1.0 功能完成。
Source decisions: D-001
Source support: D-001 => 直接排除 v0.1.0 的 Local Queue、任务窃取与 Chase-Lev 完成条件。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: v0.1.0 构建和测试不执行本地 push/pop/steal 或 Chase-Lev 算法。

### R-003 — 保留 v0.1.0 可运行基线
Status: active
Supersedes: None
Superseded by: None
Statement: 后续 Work-Stealing 版本发布后，v0.1.0 Global Queue Scheduler 必须仍可运行并作为 Benchmark 对照组。
Applies to: v0.1.0 之后的 Benchmark Framework。
Exceptions: None。
Source decisions: D-001
Source support: D-001 => 直接要求 v0.1.0 完成后保留为后续 Work-Stealing 版本的可运行 Benchmark 对照。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: Benchmark 可在同一工作负载下运行 Global Queue 基线与后续 Scheduler。

### R-004 — 规格覆盖跨版本 Runtime
Status: active
Supersedes: None
Superseded by: None
Statement: AstraScheduler 的设计规格必须覆盖整个跨版本 Runtime；仅适用于某版本的规则必须显式标出版本范围。
Applies to: 本规格及后续实质修订。
Exceptions: None。
Source decisions: D-004
Source support: D-004 => 直接确定讨论覆盖整个项目，并要求版本特有结论显式标注 Scope。
Code evidence: None（仓库当前没有实现代码）
Disposition: documentation-only
Observable result: 规格规则带有 Applies to，且不会把整体目标误写为单版本范围。

### R-005 — 实现工作按版本拆票
Status: active
Supersedes: None
Superseded by: None
Statement: 后续实现必须拆分为带目标版本的多个 Tickets，不得把整个 AstraScheduler 合并为单一实现 Ticket。
Applies to: 本规格批准后的 Ticket 规划。
Exceptions: 文档维护或不产生实现的管理工作不属于实现 Ticket。
Source decisions: D-005
Source support: D-005 => 直接要求按版本拆分实现并禁止一个巨型实现 Ticket。
Code evidence: None（仓库当前没有实现代码）
Disposition: documentation-only
Observable result: 每个实现 Ticket 记录目标版本，且不存在覆盖完整项目的单一实现 Ticket。

### R-006 — Graceful Stopping 接受授权的 Internal Submission
Status: active
Supersedes: None
Superseded by: None
Statement: Graceful Stopping 期间，Runtime 必须继续接受由同一 Scheduler 当前正在执行的已接受任务发起的 Internal Submission，并把这些任务纳入同一次 Drain Work Closure。
Applies to: 同 Scheduler Worker 在 Graceful Stopping 中的任务派生。
Exceptions: 其他线程和其他 Scheduler Worker 的提交属于 External Submission；Immediate Stopping 不适用。
Source decisions: D-002
Source support: D-002 => 直接确定授权主体、Graceful 阶段的接纳行为与传递闭包核算。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 已接受父任务在 Graceful Stopping 中提交的子任务得到正常终态，Shutdown Completion 晚于该子任务终结。

### R-007 — Graceful 转换线性化关闭 External Submission
Status: active
Supersedes: None
Superseded by: None
Statement: External Submission 与 `Running → Stopping` 必须形成单一线性化顺序；转换前线性化的提交被接受并计入 Drain Work Closure，转换后线性化的提交被拒绝且不得入队或增加 outstanding work。
Applies to: External Submission 与 Graceful Shutdown 的竞态。
Exceptions: R-006 授权的 Internal Submission。
Source decisions: D-003
Source support: D-003 => 直接确定竞态的唯一顺序、两类结果及被拒绝提交的无入队/无计数效果。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 每个竞态提交恰好落在 accepted 或 rejected 一侧，不产生孤儿任务或提前关停。

### R-008 — Immediate 取消未运行任务并发布结果
Status: superseded
Supersedes: None
Superseded by: R-106
Statement: Immediate Shutdown 线性化后，所有已接受但尚未进入 `Running` 的任务必须恰好一次转为 `Cancelled`，结果变为 ready 并唤醒全部等待者，且其用户 Callable 不得执行。
Applies to: Waiting、Ready 以及其他尚未进入 Running 的已接受任务。
Exceptions: Running Task与已启动Suspended Coroutine分别由R-009和R-075覆盖；本规则已由R-106取代。
Source decisions: D-006
Source support: D-006 => 直接确定适用任务集合、Cancelled 终态、ready/唤醒以及 Callable 不执行。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: Task start 与 cancellation 竞态只有一个赢家，Future/Result 等待者不会因队列清空而永久等待。

### R-009 — Immediate 对 Running Task 仅请求协作停止
Status: active
Supersedes: None
Superseded by: None
Statement: Immediate Shutdown 对每个 Running Task 必须发布 cooperative stop request，不得强制终止线程、注入异步异常或把 stop request 伪装为任务已终结。
Applies to: 已进入 Running 的用户 Callable。
Exceptions: 整个进程由应用选择终止不属于单 Task Runtime 终止语义。
Source decisions: D-007
Source support: D-007 => 直接要求 stop request，并排除强杀和伪造终态。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: stop-aware Callable 可自行退出；忽略 stop request 的 Callable 继续运行且阻止真实完成。

### R-010 — 非 Worker shutdown_now 同步完成
Status: active
Supersedes: None
Superseded by: None
Statement: 非目标 Scheduler Worker 调用 `shutdown_now()` 时，方法必须在全部 Worker 退出并各自完成 join、`Stopped` 发布后才返回；不合作 Running Task 可使调用无限阻塞。
Applies to: 普通应用线程以及其他 Scheduler 的 Worker 对目标 Scheduler 的调用。
Exceptions: 目标 Scheduler Worker 由 R-011 覆盖。
Source decisions: D-008
Source support: D-008 => 直接确定非 Worker 同步返回边界、无界阻塞和禁止 detach/伪完成。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 返回后没有目标 Worker 执行用户代码或访问目标 Runtime；忽略 stop 的任务可保持调用未返回。

### R-011 — 目标 Worker 的 shutdown_now 无副作用失败
Status: active
Supersedes: None
Superseded by: None
Statement: 目标 Scheduler Worker 调用该 Scheduler 的 `shutdown_now()` 必须在状态转换、任务取消、stop request、admission 或 outstanding-work 改变之前同步失败。
Applies to: 当前正在执行目标 Scheduler 任务的 Worker。
Exceptions: `Stopped` 后调用由 R-019 覆盖；其他 Scheduler Worker 按 R-010 处理。
Source decisions: D-009
Source support: D-009 => 直接确定 caller-relative 判定、同步拒绝与副作用前边界。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: self-call 不进入 Shutdown Completion，不改变 Runtime 状态或任何任务结果。

### R-012 — 非 Worker graceful shutdown 排空传递闭包
Status: active
Supersedes: None
Superseded by: None
Statement: 非目标 Scheduler Worker 调用 `shutdown()` 时，方法必须等待整个 Drain Work Closure 终结、全部 Worker 退出并 join、`Stopped` 发布后返回；闭包无法终结时可无限阻塞。
Applies to: 普通应用线程以及其他 Scheduler 的 Worker对目标 Scheduler 的调用。
Exceptions: 目标 Scheduler Worker 由 R-013 覆盖。
Source decisions: D-010
Source support: D-010 => 直接确定传递闭包、同步完成边界、无界阻塞以及禁止 detach/伪完成。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 关停不会以队列瞬时为空提前返回，返回后所有 Worker 已 join。

### R-013 — 目标 Worker 的 graceful shutdown 无副作用失败
Status: active
Supersedes: None
Superseded by: None
Statement: 目标 Scheduler Worker 调用该 Scheduler 的 `shutdown()` 必须在状态转换、admission 关闭、outstanding-work 改变或等待开始之前同步失败。
Applies to: 当前正在执行目标 Scheduler 任务的 Worker。
Exceptions: `Stopped` 后调用由 R-019 覆盖；其他 Scheduler Worker 按 R-012 处理。
Source decisions: D-011
Source support: D-011 => 直接确定 self-wait 风险、同步拒绝和副作用前边界。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: self-call 不截断当前任务的派生权限，也不参与 Shutdown Completion。

### R-014 — Shutdown Mode 只允许 Graceful 向 Immediate 升级
Status: active
Supersedes: None
Superseded by: None
Statement: Graceful Stopping 中的合法 `shutdown_now()` 必须在唯一线性化点把 Shutdown Mode 升级为 Immediate；Immediate Mode 不得降级为 Graceful。
Applies to: 进行中的 Scheduler Shutdown Completion。
Exceptions: R-011/R-013 拒绝的目标 Worker 调用不参与模式变化。
Source decisions: D-012
Source support: D-012 => 直接确定一次性升级线性化与禁止 Immediate → Graceful。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 并发升级最多发生一次，后续 `shutdown()` 不恢复任务、stop state 或 admission。

### R-015 — Immediate 升级按启动状态分类任务并关闭内部准入
Status: active
Supersedes: None
Superseded by: None
Statement: R-014 的升级点之后不得接受新的 Internal Submission；升级点前已接受且从未首次 Running 的任务按 R-106 处理，当前 Running 的任务按 R-009 处理，已经启动后 Suspended 的 Coroutine 按 R-075 处理。
Applies to: Graceful → Immediate 升级前后已接受的任务。
Exceptions: None。
Source decisions: D-012
Source support: D-012 => 直接确定升级点后的 Internal Submission 关闭及升级点前任务按是否 Running 分类。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: admission、task start 与 mode upgrade 的竞态可被唯一排序，不出现取消后执行或升级后新增工作。

### R-016 — 并发非 Worker 关停共享一次完成
Status: active
Supersedes: None
Superseded by: None
Statement: 同一次关停中的所有非 Worker `shutdown()`/`shutdown_now()` 必须幂等参与同一个 Shutdown Completion，每个调用在 `Stopped` 后返回，且每个 Worker 线程恰好被 join 一次。
Applies to: 同一个 Runtime 的重复和并发关停调用。
Exceptions: R-014 允许一次 Graceful → Immediate 升级；目标 Worker 调用由 R-011/R-013 拒绝。
Source decisions: D-013
Source support: D-013 => 直接确定共享完成、重复调用幂等、所有参与者返回边界和 unique join。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 大量并发调用不会重复取消、重复发布 stop request、并发 join 或提前返回。

### R-017 — 非 Worker 析构保留或发起 Graceful 策略
Status: superseded
Supersedes: None
Superseded by: R-103
Statement: 非目标 Scheduler Worker 销毁活动 Scheduler Handle 时，`Running` Runtime 必须发起 Graceful Shutdown，`Stopping` Runtime 必须保留现有 Shutdown Mode 并加入同一个 Shutdown Completion。
Applies to: 普通应用线程和其他 Scheduler Worker 上的最后 Handle 析构。
Exceptions: 目标 Scheduler Worker 的最后 Handle 析构由 R-021/R-022 覆盖。
Source decisions: D-014
Source support: D-014 => 直接确定 Running 默认 Graceful、Stopping 保留模式并加入现有完成。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 非 Worker 析构不把未显式选择的 Graceful 策略升级为 Immediate。

### R-018 — 非 Worker 析构是 noexcept 同步回收边界
Status: superseded
Supersedes: None
Superseded by: R-105
Statement: R-017 的析构必须为 `noexcept`，并在全部 Worker join、`Stopped` 发布后完成；Drain Work Closure 不终结时可无限阻塞，不得 detach Worker 或伪造完成。
Applies to: 非目标 Scheduler Worker 的活动 Handle 析构。
Exceptions: None。
Source decisions: D-014
Source support: D-014 => 直接确定 noexcept、同步完成、无界等待以及无 detach/伪完成。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 析构返回后没有 Worker 访问 Runtime；不合作任务使析构保持阻塞。

### R-019 — Stopped 是关停吸收状态
Status: active
Supersedes: None
Superseded by: None
Statement: `Stopped` 发布后，任何线程调用 `shutdown()` 或 `shutdown_now()` 必须成功且立即无副作用返回，不得创建新 Shutdown Completion、改变历史 Shutdown Mode、重复 join、重启 Runtime 或改写任务终态。
Applies to: 已 Stopped 的 Scheduler Runtime。
Exceptions: None。
Source decisions: D-016
Source support: D-016 => 直接确定跨线程幂等 no-op、吸收状态及被禁止的第二世代和追溯修改。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 两种关停 API 在 Graceful/Immediate 完成后均稳定立即返回且状态不变。

### R-020 — Handle 生命周期与共享 Runtime State 解耦
Status: active
Supersedes: None
Superseded by: None
Statement: Scheduler Handle 的生命周期必须与共享 Runtime State 解耦，Runtime State 必须存活到所有 Worker 停止访问、线程完成 join 且最终回收结束。
Applies to: Scheduler Handle、Worker 执行引用与 Reaper 所有权。
Exceptions: None。
Source decisions: D-017
Source support: D-017 => 直接确定对象模型分离与 Runtime State 的完整存活边界。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 最后 Handle 消失不会导致仍在访问的 Worker 发生 use-after-free。

### R-021 — 目标 Worker 最后 Handle 通过 Reaper handoff 返回
Status: active
Supersedes: None
Superseded by: None
Statement: 目标 Scheduler Worker 销毁最后一个 Handle 时，析构必须在线性化地移交 Runtime State 强所有权给非目标 Worker Reaper 后立即返回，不得 self-wait、self-join、detach、伪造 Shutdown Completion 或仅因该场景 terminate。
Applies to: 同 Scheduler Worker 上的最后 Handle 析构。
Exceptions: 已 Stopped 时仍由安全非 Worker 路径完成必要 join/reclamation。
Source decisions: D-017, D-018
Source support: D-017 => 直接确定强引用无空窗移交、Reaper 上下文和禁止 self-join/detach/fail-fast; D-018 => 直接确定 handoff 后立即返回、不等待 Shutdown Completion。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: Worker 任务可释放最后 Handle 并继续返回，Runtime State 保持有效且后续真实完成。

### R-022 — Worker orphan handoff 保留 Graceful 默认
Status: active
Supersedes: None
Superseded by: None
Statement: R-021 发生时，`Running` Runtime 必须请求 Graceful Shutdown，`Stopping` Runtime 必须保留当前 Shutdown Mode，`Stopped` Runtime 只进入安全 join/final reclamation。
Applies to: Worker 最后 Handle 析构时的 Runtime 状态分支。
Exceptions: 并发合法 `shutdown_now()` 可按 R-014 的顺序升级模式。
Source decisions: D-018
Source support: D-018 => 直接确定 Running/Stopping/Stopped 三个分支和不隐式选择 Immediate。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: Worker handoff 与非 Worker 析构表达相同的默认 Graceful 意图，但前者异步返回。

### R-023 — Reaper handoff 能力先于 Worker 启动
Status: active
Supersedes: None
Superseded by: None
Statement: Runtime 必须在任何 Worker 启动前建立并预留 Reaper handoff 能力；准备失败时启动必须失败、不得发布 `Running`，且不得留下活动 Worker。
Applies to: Scheduler 启动事务。
Exceptions: None。
Source decisions: D-019
Source support: D-019 => 直接确定资源建立时序、失败可见边界和零活动 Worker 后置条件。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 注入注册、预留或 coordinator 建立失败时，用户任务从未获得执行窗口。

### R-024 — 运行期 handoff 不获取可失败资源
Status: active
Supersedes: None
Superseded by: None
Statement: 进入 `Running` 后，Worker 最后 Handle 的 handoff 必须为 `noexcept`、不分配内存、不创建线程，并且不得等待 Drain Work Closure、Worker 退出或 Shutdown Completion。
Applies to: R-021 的运行期所有权移交路径。
Exceptions: 可以使用内部同步完成线性化。
Source decisions: D-019
Source support: D-019 => 直接确定 noexcept、无分配、无线程创建与不等待完成。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 资源耗尽故障注入不会让已经 Running 的 handoff 丢失 Runtime State 所有权。

### R-025 — Pending Runtime 不阻塞 Reaper
Status: active
Supersedes: None
Superseded by: None
Statement: Reaper 可以持有 Pending Runtime State，但不得阻塞等待其任务或 Drain Work Closure；永久 Pending Runtime 不得阻塞其他 Join Ready Runtime 的回收。
Applies to: Reaper 同时核算一个或多个 orphan Runtime State。
Exceptions: None。
Source decisions: D-020
Source support: D-020 => 直接确定 Pending 持有语义、无阻塞等待和 head-of-line 隔离。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 一个永久任务所在 Runtime 长期 Pending 时，其他 Runtime 仍能 join 并发布 Stopped。

### R-026 — Join Ready 后唯一 join 并发布 Stopped
Status: active
Supersedes: None
Superseded by: None
Statement: 仅当 Runtime 单调进入 Join Ready 后，Reaper 才能认领 join；Reaper 与同步关停路径之间只能有一个 join owner，且 `Stopped`/Shutdown Completion 只能在全部 Worker 实际 join 后发布。
Applies to: Runtime 回收和 join ownership 竞态。
Exceptions: None。
Source decisions: D-020
Source support: D-020 => 直接确定 Join Ready 前置条件、单调含义、唯一 join 竞争与 Stopped 发布边界。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: Join Ready 本身不会提前满足完成，Worker 也不会等待 Reaper 先 join 而形成循环等待。

### R-027 — 进程级 Reaper 使用单一专用 coordinator
Status: superseded
Supersedes: None
Superseded by: R-107
Statement: 一个进程内必须只有一个逻辑 Reaper Service 和恰好一条不属于任何 Scheduler 的专用 coordinator thread；不得为单个 Scheduler 或单次 handoff 创建额外 Reaper thread，coordinator 不得执行用户任务、参与 work stealing 或形成 Internal Submission。
Applies to: 进程内全部 Scheduler Runtime。
Exceptions: 从未建立服务且空集合 Finalization 由 R-037 直接完成。
Source decisions: D-021
Source support: D-021 => 直接确定进程级共享、单线程拓扑、无额外 Reaper thread 和 coordinator 角色排除。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: Scheduler/Runtime 数量增长不增加 Reaper coordinator 数量，用户任务从不在线程上执行。

### R-028 — Reaper 空闲时保持同一服务
Status: active
Supersedes: None
Superseded by: None
Statement: Reaper Service 首次成功建立后必须在空闲时阻塞等待并保持同一 coordinator，不得因最后一个 Runtime 消失、队列为空或空闲超时而自动停止或重建。
Applies to: RegistrationOpen 阶段的进程级 Reaper Service。
Exceptions: 显式 Reaper Finalization 由 R-029 至 R-043 覆盖。
Source decisions: D-022
Source support: D-022 => 直接确定空闲阻塞、最后 Runtime 不触发停止、后续复用和无自动重启。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 多轮 Scheduler 创建/销毁复用同一休眠 coordinator，空闲时不 busy-spin 或强持有已完成 Runtime。

### R-029 — Finalization 永久关闭注册并线性化启动竞态
Status: superseded
Supersedes: None
Superseded by: R-097
Statement: Reaper Finalization 的线性化点必须永久关闭新 Runtime 注册；之前成功注册的 Runtime 纳入同一次核算，之后的启动必须在创建 Worker 前失败，`Finalizing`/`Finalized` 不得恢复注册、重建 Reaper 或启动新 Scheduler Worker。
Applies to: Scheduler 启动与进程级 Finalization 的竞态。
Exceptions: None。
Source decisions: D-023
Source support: D-023 => 直接确定永久关闭、竞态两侧、失败时序和禁止 restart。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 竞态启动恰好被纳入或在零 Worker 条件下失败，不存在无 handoff 能力的 Running Runtime。

### R-030 — Finalization 对核算集合请求 Graceful
Status: superseded
Supersedes: None
Superseded by: R-104
Statement: Finalization 必须对全部已核算 Runtime 请求 Graceful Shutdown，保持既有 Immediate/Stopping/Stopped 语义；已注册但未发布 Running 的事务必须在开放用户任务或 External Submission 前观察 sticky request 并回滚，或进入不开放外部准入的 Graceful Stopping。
Applies to: R-029 线性化点前已注册的全部 Runtime，包括 Starting。
Exceptions: 并发 Immediate 请求可按 R-014 或 R-034 单向升级。
Source decisions: D-024
Source support: D-024 => 直接确定默认 Graceful、现有模式保留和 Starting Runtime 的 sticky request/无用户窗口。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: Finalization 不隐式取消待执行任务，Starting Runtime 不会在全局 Finalizing 后短暂接受用户工作。

### R-031 — begin_finalization 只发布开始请求
Status: active
Supersedes: None
Superseded by: None
Statement: `begin_finalization()` 必须完成永久注册关闭、初始 Graceful/sticky 请求可靠记录和 coordinator 通知后立即返回，不得等待 Runtime drain、Join Ready、Worker/coordinator join、`Stopped` 或 `Finalized`。
Applies to: 首次和幂等重复 begin；重复语义见 R-037。
Exceptions: 空核算集合可以在同一调用内真实完成，见 R-037。
Source decisions: D-026
Source support: D-026 => 直接确定 begin 的线性化副作用、可靠发布边界和被排除的等待阶段。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: begin 返回只证明 Finalization 已不可逆开始；活动 Runtime 可继续在后台推进。

### R-032 — wait 只观察真实 Finalization Completion
Status: active
Supersedes: None
Superseded by: None
Statement: 合法调用的 `wait()` 必须在全部已核算 Runtime 达到 Shutdown Completion 并解除注册、Reaper 工作清空、coordinator 退出并 join、`Finalized` 发布后返回；它可无限阻塞，且不得升级模式、detach、伪造完成或创建新终结世代。
Applies to: 非 Worker FinalizationControl 等待者。
Exceptions: Worker 调用由 R-039 拒绝。
Source decisions: D-027
Source support: D-027 => 直接确定完整完成集合、无界阻塞和 wait 的无策略副作用。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: wait 返回后进程内没有 AstraScheduler Worker 或 Reaper coordinator 存活。

### R-033 — wait_for 超时不改变 Finalization
Status: active
Supersedes: None
Superseded by: None
Statement: 合法 `wait_for(timeout)` 仅在真实 Finalization Completion 达成时返回 `Completed`，否则在期限结果线性化后返回 `TimedOut`；`TimedOut` 不得发布 `Finalized`、恢复注册、重启/停止 Reaper、升级模式、detach 或创建新终结世代，后台必须继续推进同一次 Finalization。
Applies to: 非 Worker FinalizationControl 有界等待者。
Exceptions: 精确 clock 与边界竞态见 R-041；Worker 调用见 R-040。
Source decisions: D-028
Source support: D-028 => 直接确定结果含义、超时后的禁止副作用和后台持续推进。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 首次 TimedOut 后，同一控制对象或副本可继续等待并最终观察 Completed。

### R-034 — 显式 Finalization Escalation 覆盖全部未完成 Runtime
Status: active
Supersedes: None
Superseded by: None
Statement: `request_immediate()` 必须幂等地把同一核算集合内全部尚未达到 Shutdown Completion 的 Runtime 单向请求为 Immediate，包括 orphan 和 Starting Runtime；请求可靠记录并通知后返回，不得等待 Completion，已完成 Runtime 不得被改写。
Applies to: FinalizationControl 的显式进程级升级。
Exceptions: Running Task 仍只适用 R-009，因此升级不保证有界完成。
Source decisions: D-029, D-039
Source support: D-029 => 直接确定完整作用域、各状态处理、请求式返回、幂等单调和同一完成世代; D-039 => 直接固定该操作的公共名称为 `request_immediate()`。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 没有 Scheduler Handle 的 Pending Runtime 也收到 Immediate 请求，Completed Runtime 历史不变。

### R-035 — Finalization 操作只通过有效控制对象组织
Status: active
Supersedes: None
Superseded by: None
Statement: `begin_finalization()` 必须是创建有效 `FinalizationControl` 的唯一公共入口；控制对象不得公开默认构造，`wait()`、`wait_for()` 与 `request_immediate()` 只能作为该对象的操作提供。
Applies to: 进程级 Finalization 公共 Interface。
Exceptions: None。
Source decisions: D-030
Source support: D-030 => 直接确定 begin-before-wait 的 capability 形态、无公共默认构造和对象承载的三项后续操作。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 没有 begin 返回值时，公共类型系统中不存在合法 wait/upgrade 调用路径。

### R-036 — FinalizationControl 是共享且析构无策略的 capability
Status: active
Supersedes: None
Superseded by: None
Statement: `FinalizationControl` 必须可复制、可移动并支持多个线程数据竞争安全地操作同一 Finalization Completion；销毁任意或全部副本不得阻塞、取消、暂停、恢复注册、停止后台工作或隐式认领 join。
Applies to: 所有由幂等 begin 返回的控制对象及其副本。
Exceptions: `wait()`/`wait_for()` 的 Worker caller 限制见 R-039/R-040。
Source decisions: D-030, D-031
Source support: D-030 => 直接确定控制对象不拥有 Reaper/Runtime 生命周期且析构无副作用; D-031 => 直接确定 copy/move、共享同一完成状态与多线程数据竞争安全。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 一个副本升级后所有副本观察同一过程；全部副本销毁后后台仍继续。

### R-037 — begin_finalization 幂等共享唯一世代
Status: active
Supersedes: None
Superseded by: None
Statement: 首次 `begin_finalization()` 必须建立唯一 Finalization Completion；并发、Finalizing 或 Finalized 后的重复调用必须返回关联同一世代的控制对象且不重复副作用；从未建立 Reaper 且核算集合为空时必须永久关闭注册并直接完成，不得创建 coordinator。
Applies to: 所有进程级 begin 调用。
Exceptions: None。
Source decisions: D-032
Source support: D-032 => 直接确定唯一线性化、重复/并发/Finalized 后行为和空集合无资源完成。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 多个 begin 获得的控制对象观察同一 Completion，空进程 begin 后 Completed 且无 Reaper thread。

### R-038 — begin 与 request_immediate 可由任意应用线程请求
Status: active
Supersedes: None
Superseded by: None
Statement: `begin_finalization()` 与 `request_immediate()` 必须允许任意应用线程调用，包括任意 Scheduler Worker；两者只完成线性化、可靠记录和通知，不得等待 Runtime drain、Worker/coordinator 退出或 join。
Applies to: 两个 Finalization 请求式命令。
Exceptions: 允许短暂内部同步，不形成 lock-free、wait-free 或固定时延承诺。
Source decisions: D-033
Source support: D-033 => 直接确定 caller eligibility、请求式边界和非 lock-free 承诺。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: Worker 可发起全局终结或升级后继续完成当前任务，不产生 self-wait。

### R-039 — 任意 Scheduler Worker 调用 wait 抛出 logic_error
Status: active
Supersedes: None
Superseded by: None
Statement: 任意 AstraScheduler Worker 调用 `FinalizationControl::wait()` 必须在等待、join ownership 认领或 Finalization 状态改变前同步抛出 `std::logic_error`。
Applies to: 全部已注册 Runtime 的 Worker，而非只检查当前 Scheduler。
Exceptions: 普通非 Worker 按 R-032 处理。
Source decisions: D-034
Source support: D-034 => 直接确定全局 Worker 范围、异常类型和无副作用检查时序。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 任一 Scheduler 的 Worker 调用 wait 得到 logic_error，Completion 和唯一 join owner 不受影响。

### R-040 — 任意 Scheduler Worker 调用 wait_for 抛出 logic_error
Status: active
Supersedes: None
Superseded by: None
Statement: 任意 AstraScheduler Worker 调用 `FinalizationControl::wait_for(timeout)` 必须在读取 timeout、等待或状态改变前同步抛出 `std::logic_error`，包括 timeout 为零或负值时。
Applies to: 全部已注册 Runtime 的 Worker。
Exceptions: 普通非 Worker 按 R-033/R-041 处理。
Source decisions: D-035
Source support: D-035 => 直接确定 Worker 范围、异常类型、零/负 duration 及无副作用边界。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: Worker 的正、零、负 timeout 调用均抛异常而不返回 TimedOut。

### R-041 — wait_for 使用 steady_clock 与唯一边界顺序
Status: active
Supersedes: None
Superseded by: None
Statement: 合法 `wait_for(timeout)` 必须用 `std::chrono::steady_clock` 形成 deadline；timeout 小于等于零时执行一次即时无副作用观察，正 timeout 时 Completion 与 deadline 必须在同一同步域内形成唯一顺序，先观察 Completion 返回 `Completed`，先确认期限已到且 Completion 未发布返回 `TimedOut`。
Applies to: 非 Worker 的所有 `std::chrono::duration<Rep, Period>` 调用。
Exceptions: duration 到内部 deadline 的饱和转换算法属于实现选择，但不得产生未定义溢出。
Source decisions: D-036
Source support: D-036 => 直接确定 clock、非正即时观察、正 duration 竞态顺序及转换安全边界。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: wall-clock 跳变不影响等待；TimedOut 线性化后即使返回前完成，本次结果仍为 TimedOut。

### R-042 — 合法等待者唯一 join coordinator 后发布完成
Status: active
Supersedes: None
Superseded by: None
Statement: coordinator 必须在工作清空后发布 `CoordinatorExited` 并退出但不得自行发布 Finalization Completion；恰好一个合法非 Worker 等待者必须在观察 Exited 后认领并执行唯一 join，再发布 `Finalized`/Completion，其他等待者只观察同一完成事件。
Applies to: coordinator 退出与并发 Finalization 等待者。
Exceptions: 没有等待者时可保持 Exited-unjoined，直到未来合法等待者完成收尾。
Source decisions: D-037
Source support: D-037 => 直接确定 Exited/Completion 分离、唯一 join owner、发布顺序和无等待者状态。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 多等待者场景只有一次 coordinator join，Completed 永远晚于该 join。

### R-043 — Finalization 只由应用显式编排
Status: active
Supersedes: None
Superseded by: None
Statement: AstraScheduler 不得通过 `atexit`、进程级静态析构、最后一个 Handle/Runtime 析构或空闲超时自动触发 begin、wait 或 Immediate escalation；动态库卸载前，非 Worker 调用方必须先观察 `Completed` 并结束所有仍会调用库代码的公共对象，`TimedOut` 不得作为卸载许可；不可逆公共测试不得依赖 reset/restart。
Applies to: 进程退出、静态生命周期、动态库卸载与全局测试隔离。
Exceptions: 调用方可以在 TimedOut 后自行终止整个进程，但这不形成任务清理或 trace flush 保证。
Source decisions: D-038
Source support: D-038 => 直接排除所有自动触发点，确定卸载双重 gate、TimedOut 非许可和子进程/无 reset 测试边界。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 最后 Scheduler 消失只使 Reaper 空闲；卸载测试只有 Completed 且对象停止调用后通过。

### R-044 — Finalization 公共类型和结果枚举固定
Status: active
Supersedes: None
Superseded by: None
Statement: 公共 C++20 Interface 必须在 `astra` 命名空间提供 `FinalizationControl` 与 `enum class FinalizationWaitResult { Completed, TimedOut }`。
Applies to: Finalization 公共头文件。
Exceptions: 枚举底层整数类型和类内存布局未固定。
Source decisions: D-039
Source support: D-039 => 直接固定命名空间、两个公共类型和两个枚举值。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 调用方可使用稳定限定名比较 Completed/TimedOut，未暴露其他必需结果值。

### R-045 — Finalization 四操作签名固定
Status: active
Supersedes: None
Superseded by: None
Statement: 公共 Interface 必须提供 `[[nodiscard]] FinalizationControl begin_finalization() noexcept`、`void FinalizationControl::wait() const`、接受 `std::chrono::duration<Rep, Period>` 的 `[[nodiscard]] FinalizationWaitResult wait_for(...) const` 与 `void request_immediate() const noexcept`；控制对象的 copy/move/析构必须为 `noexcept`，且没有公共默认构造。
Applies to: Finalization 公共 C++20 API surface。
Exceptions: `wait()`/`wait_for()` 不带 `noexcept`，以承载 R-039/R-040 的 `std::logic_error`。
Source decisions: D-039
Source support: D-039 => 直接固定四个签名、nodiscard/noexcept 属性、duration 泛型及控制对象 special members/default construction。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 编译期 API tests 验证签名、属性、构造能力和异常规格。

### R-046 — Finalization 不暴露同义或重置接口
Status: active
Supersedes: None
Superseded by: None
Statement: 公共 Interface 不得提供全局 `wait_finalization()`、`finalize_now()`、control accessor、reset/restart、`wait_until()`、stop-token wait、coroutine await 或 progress callback 作为当前规格的一部分。
Applies to: 当前 Finalization 公共 API surface。
Exceptions: 未来 accepted decision 与新规则可以扩展非冲突能力。
Source decisions: D-039
Source support: D-039 => 直接排除全局 wait/finalize_now/accessor/reset，并把 wait_until、stop-token wait、coroutine await 与 progress callback 留在非目标范围。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: public header/API inventory 只出现 R-044/R-045 的 Finalization 能力。

### R-047 — Reaper 控制面不可恢复故障 fail-fast
Status: active
Supersedes: None
Superseded by: None
Statement: Reaper coordinator 顶层必须拦截全部逃逸异常；无法证明安全恢复的控制面故障必须先执行 `noexcept` 的尽力诊断再调用 `std::terminate()`，不得伪造 `Stopped`/`Finalized`/`TimedOut`、detach、泄漏后继续或重启 Reaper；用户 Callable 异常、合法 timeout 与永久 Pending 不得进入该 fatal path。
Applies to: coordinator 逃逸异常、join ownership、handoff 所有权连续性及其他不可恢复不变量破坏。
Exceptions: Worker 启动前可正常报告的资源准备失败按 R-023 处理。
Source decisions: D-040
Source support: D-040 => 直接确定顶层异常屏障、诊断后 terminate、禁止伪完成/继续/restart 以及预期域行为排除。
Code evidence: None（仓库当前没有实现代码）
Disposition: implementation
Observable result: 子进程故障注入确定性进入 terminate；任务异常、TimedOut 和 Pending 场景保持正常域语义。

### R-048 — TaskHandle 是共享任务 capability
Status: active
Supersedes: None
Superseded by: None
Statement: `submit()` 从 v0.1.0 起必须返回可复制、可移动的 `TaskHandle<T>`；所有副本关联同一 TaskId/完成状态，最后一个 Handle 销毁不得隐式取消或重新提交任务。
Applies to: 普通 Callable Task 的公共 Handle 与 logical identity。
Exceptions: moved-from/default Handle 为空；Graph Node 没有 per-node TaskHandle。
Source decisions: D-041, D-042, D-043, D-067, D-153
Source support: D-041 => 首版返回自定义Handle; D-042/D-043 => 共享复制与无隐式取消; D-067 => move/empty; D-153 => TaskId稳定。
Code evidence: None
Disposition: implementation
Observable result: 复制Handle不复制执行，丢弃全部Handle后已接受Task仍能完成。

### R-049 — Terminal Outcome 与终态一致发布
Status: active
Supersedes: None
Superseded by: None
Statement: 每个 Task 必须恰好一次发布不可变 Value、Exception 或 Cancelled Terminal Outcome，并在同一 completion publication 中使对应 `Succeeded/Failed/Cancelled` TaskState 与全部等待者可见。
Applies to: Callable、Coroutine 与 Graph Node Task identity。
Exceptions: 尚未成功 admission 的工作没有 TaskState/Outcome。
Source decisions: D-044, D-071
Source support: D-044 => 三类共享Outcome; D-071 => State与Outcome原子一致发布。
Code evidence: None
Disposition: implementation
Observable result: 不存在已见终态却读不到Outcome或同一Task副本看到不同Outcome的窗口。

### R-050 — 异常和取消通过 get 重复传播
Status: active
Supersedes: None
Superseded by: None
Statement: Worker 边界必须捕获逃逸的任意 C++ 异常并保存 `std::exception_ptr`；有效左值 `get()` 必须按原动态类型重复重抛 Exception，Cancelled Outcome 必须重复抛 `astra::task_cancelled`，且异常不得逃出 Worker entry。
Applies to: `TaskHandle<T>::get() const &` 与 `TaskHandle<void>`。
Exceptions: 以 `task_cancelled` 逃出用户执行按 R-054 转为 Cancelled，而非 Failed。
Source decisions: D-045, D-057
Source support: D-045 => exception_ptr与原类型重抛; D-057 => Cancelled的task_cancelled传播。
Code evidence: None
Disposition: implementation
Observable result: 多个Handle副本可重复观察相同异常或取消，不会终止Worker线程。

### R-051 — get 的结果引用由左值 Handle 持有
Status: active
Supersedes: None
Superseded by: None
Statement: `TaskHandle<T>::get() const &` 必须在完成后返回共享 `const T&`，`TaskHandle<void>::get() const &` 返回 `void`，两者的 rvalue overload 必须删除且不得提供消费式 `take()`。
Applies to: Value Outcome 的公共访问与 lifetime。
Exceptions: Exception/Cancelled 按 R-050 抛出。
Source decisions: D-076
Source support: D-076 => 左值限定、const引用/void、Handle持有lifetime与删除rvalue。
Code evidence: None
Disposition: implementation
Observable result: 保留任一Handle即可稳定引用Value；临时Handle调用get在编译期失败。

### R-052 — get 使用 caller-relative Unbounded/Helping Wait
Status: active
Supersedes: None
Superseded by: None
Statement: 未完成 Task 的 `get()` 在非 Worker 上必须执行 Unbounded Wait；同 Runtime Worker 必须 Helping source Runtime，跨 Runtime Worker 只帮助 source Runtime；Direct Self-Wait 必须在副作用前抛 `std::logic_error`，Runtime 不保证检测 Indirect Wait Cycle。
Applies to: 同步 Task result 获取。
Exceptions: source Runtime Immediate 时只可恢复已开始Coroutine segment，见 R-059/R-075。
Source decisions: D-047, D-048, D-049, D-050, D-051
Source support: D-047 => 无界等待; D-048 => same-runtime Helping; D-049 => self-wait拒绝; D-050 => 不检测间接环; D-051 => cross-runtime只帮source。
Code evidence: None
Disposition: implementation
Observable result: 等待不创建补偿线程或执行foreign Runtime工作；动态环允许永久阻塞。

### R-053 — request_cancel 按首次 start 竞态分类
Status: active
Supersedes: None
Superseded by: None
Statement: `void TaskHandle::request_cancel() const noexcept` 必须幂等、可并发且在请求可靠发布后立即返回；请求在线性化上先于首次 start 时 Task 直接发布 Cancelled且不执行用户代码，start 先胜出时只发布 cooperative stop request。
Applies to: 任意线程对有效 TaskHandle 的单 Task cancellation。
Exceptions: empty Handle 的 request_cancel 是 R-057 的 no-op；已Terminal不改写。
Source decisions: D-052, D-053, D-054, D-055
Source support: D-052 => start分类; D-053 => 请求式返回; D-054 => API签名; D-055 => 任意线程并发。
Code evidence: None
Disposition: implementation
Observable result: cancellation/start竞态只有一个分类，重复调用不重复完成或执行。

### R-054 — Cooperative stop 的真实退出决定 Outcome
Status: active
Supersedes: None
Superseded by: None
Statement: Running Callable 在 stop request 后正常返回仍必须发布 Value，抛出 `task_cancelled` 才发布 Cancelled，其他异常发布 Exception；`submit` 必须优先普通invocation，仅普通形式不可调用时在首参数注入该Task的 `std::stop_token`，并提供不挂起的 `throw_if_stop_requested(token)` cancellation point。
Applies to: stop-aware Callable invocation 与 execution boundary。
Exceptions: Coroutine内建awaiter取消见 R-075。
Source decisions: D-056, D-058, D-059, D-060
Source support: D-056 => 正常返回保留Value; D-058 => signal转Cancelled; D-059 => token注入选择; D-060 => helper。
Code evidence: None
Disposition: implementation
Observable result: stop request本身不覆盖用户真实结果，generic callable不会意外收到token。

### R-055 — wait 只同步完成且复用 Helping
Status: active
Supersedes: None
Superseded by: None
Statement: `TaskHandle::wait() const` 必须只在真实 Terminal Outcome 发布后返回，不传播或消费 Value/Exception/Cancelled，并按调用方身份复用 R-052 的 Unbounded/Helping/self-wait规则；多个等待者共享一次 completion publication且不得丢失完成。
Applies to: 无期限同步完成观察。
Exceptions: None。
Source decisions: D-061, D-062, D-073
Source support: D-061 => 不传播Outcome; D-062 => caller-relative Helping; D-073 => 多等待者无丢失。
Code evidence: None
Disposition: implementation
Observable result: wait后仍可完整get，多等待者最终观察同一完成。

### R-056 — wait_for 超时不伪造 Task 完成
Status: active
Supersedes: None
Superseded by: None
Statement: `TaskHandle::wait_for(duration)` 必须以 steady_clock 返回 `Completed/TimedOut`且不传播Outcome或取消Task；非正duration即时观察，完成与期限形成唯一顺序，Worker在等待期间按R-052帮助，但 helped Callable 不可抢占使实际返回可越过timeout。
Applies to: TaskHandle 有界同步观察。
Exceptions: Direct Self-Wait在读取期限或Helping前抛logic_error。
Source decisions: D-063, D-064, D-065, D-066
Source support: D-063 => 结果与无副作用; D-064 => clock/边界; D-065 => Helping/self优先; D-066 => 非硬返回上限。
Code evidence: None
Disposition: implementation
Observable result: TimedOut 后Task继续，稍后wait/get仍可观察真实Outcome。

### R-057 — TaskHandle 空状态、TaskState 与并发边界固定
Status: active
Supersedes: None
Superseded by: None
Statement: TaskHandle 必须支持 default/moved-from empty 与 `valid()`；空对象的 get/wait/wait_for/state/id 抛logic_error而 request_cancel为no-op；有效Task的公共State仅为 Waiting/Ready/Running/Suspended/Succeeded/Failed/Cancelled，`state()`非阻塞线性化，稳定对象操作可并发而同一对象reassociation/destruction需调用方同步。
Applies to: TaskHandle value semantics与公共TaskState。
Exceptions: 内部queue/claim/publication瞬态不公开。
Source decisions: D-067, D-068, D-069, D-070, D-071, D-072, D-073, D-153
Source support: D-067/D-068 => empty规则; D-069/D-070 => 七态snapshot; D-071/D-073 => completion一致性; D-072 => 并发边界; D-153 => id。
Code evidence: None
Disposition: implementation
Observable result: 空对象不会伪装Task状态，有效副本并发观察同一单调生命周期。

### R-058 — submit 结果类型与基础结果 API 受限
Status: active
Supersedes: None
Superseded by: None
Statement: submit选择R-054的invocation后，只能产生 `TaskHandle<void>` 或去顶层cv且可移动构造的对象 `TaskHandle<T>`；裸引用与完全immovable结果必须编译期拒绝，move-only结果必须支持，稳定API不得增加take/try_get/exception/OutcomeView第二套结果通道。
Applies to: submit/spawn结果类型和TaskHandle公共surface。
Exceptions: `std::reference_wrapper`与指针作为显式值类型可用，lifetime由调用方承担。
Source decisions: D-074, D-075, D-076, D-077
Source support: D-074 => 拒绝引用; D-075 => void/可移动对象; D-076 => shared观察; D-077 => 排除重复API。
Code evidence: None
Disposition: implementation
Observable result: 编译期矩阵稳定支持void/copyable/move-only并拒绝reference/immovable。

### R-059 — Helping depth 与 Shutdown eligibility 受配置约束
Status: active
Supersedes: None
Superseded by: None
Statement: 每个Worker的Helping嵌套深度必须受正数 `max_helping_depth` 限制且默认64，超限在启动下一层帮助前抛 `helping_depth_exceeded`；Helping始终使用source Runtime正常eligibility，Graceful仅推进Drain Closure，Immediate不得first-start新Task。
Applies to: get/wait/wait_for/GraphRun同步Helping。
Exceptions: Immediate可运行R-075规定的already-started Coroutine resume segment。
Source decisions: D-078, D-079, D-080
Source support: D-078 => 默认/正数配置; D-079 => 异常边界; D-080 => Shutdown eligibility。
Code evidence: None
Disposition: implementation
Observable result: 深层同步组合确定性失败而不篡改目标Task，Immediate不借Helping启动新工作。

### R-060 — 未观察失败仅按启用观测面诊断
Status: active
Supersedes: None
Superseded by: None
Statement: Exception Outcome 在首次get/await传播前必须幂等标记observed；最终shared state释放时若仍未观察，仅在Metrics Basic/Detailed增加稳定 `unobserved_failures`，并仅在活动Trace可用时尽力发事件，不得terminate、默认日志、回调、级联取消或维持Metrics Off隐藏计数。
Applies to: 普通Task与R-072的Graph真实Failed Node。
Exceptions: wait/state/wait_for不标记observed。
Source decisions: D-081, D-082, D-120, D-151
Source support: D-081 => 纯诊断边界; D-082 => get的observed时点/最终判断; D-120 => co_await复用get传播语义; D-151 => Metrics/Trace启用组合与字段名。
Code evidence: None
Disposition: implementation
Observable result: 关闭Metrics/Trace没有隐藏输出，启用时未观察失败可计数而不改变执行。

### R-061 — External Pending Capacity 与 backpressure 固定
Status: active
Supersedes: None
Superseded by: None
Statement: 每Runtime必须以正数 `external_pending_capacity` 限制已接受但未首次Running的External工作，默认65536；slot在admission占用并在首次start或start前Terminal释放，Internal不占用；容量策略仅为默认Reject或Block，Block只允许普通非Worker且必须在slot/gate竞态下无丢唤醒，CallerRuns不得提供。
Applies to: 普通Task与R-070的External Graph admission。
Exceptions: try_submit永不等待；started后Coroutine suspension不重新占slot；Block等待者不保证FIFO或公平顺序。
Source decisions: D-083, D-084, D-085, D-086
Source support: D-083 => 配额口径; D-084 => Reject/Block; D-085 => caller限制; D-086 => gate/slot等待协议。
Code evidence: None
Disposition: implementation
Observable result: External未启动工作有界，Worker不会因Block自锁，关闭gate能唤醒并拒绝等待者。

### R-062 — submit/try_submit 共享强 admission transaction
Status: active
Supersedes: None
Superseded by: None
Statement: `submit`与`try_submit`必须在同一强异常安全事务中完成gate、slot、capture/TCB、ID、outstanding与不可丢失publication；成功返回真实TaskHandle，失败完全回滚且不执行Callable；最终 `SubmissionError` 仅为 Stopping/Stopped/CapacityExhausted，submit抛`submission_rejected`，try_submit即时返回variant alternative，其他构造/分配异常保持原类型。
Applies to: Callable与Coroutine spawn的Runtime admission。
Exceptions: 空Scheduler为logic_error，Finalization启动拒绝由R-097的creation error表达。
Source decisions: D-087, D-088, D-089, D-155
Source support: D-087/D-155 => 最终error集合; D-088 => variant/no wait; D-089 => transaction与rollback。
Code evidence: None
Disposition: implementation
Observable result: 不存在orphan Handle、泄漏slot/outstanding或已拒绝却执行的Callable。

### R-063 — Ready Routing Precedence 与 source公平性固定
Status: active
Supersedes: None
Superseded by: None
Statement: v0.1全部Ready进Global；后续External/cross-runtime进Global、same-runtime Internal进owner Local，无专用规则的完成publication在owner Worker走Local否则Global；yield/timer强制ordinary Global，never-started Deadline进Global EDF；Worker连续Local claim最多默认64次后必须探测Global，Global FIFO、owner Local LIFO、thief取oldest。
Applies to: Ready Task首次publication和Coroutine resume routing。
Exceptions: deadline Task首次start后按触发它的awaiter回归非EDF规则。
Source decisions: D-090, D-091, D-092, D-147
Source support: D-090 => 基础路由; D-091 => Global服务界; D-092 => endpoint order; D-147 => 专用优先级与冲突消解。
Code evidence: None
Disposition: implementation
Observable result: routing source可由Trace验证，Local洪水不能永久饿死Global。

### R-064 — Steal round 有界且 victim 不重复
Status: active
Supersedes: None
Superseded by: None
Statement: 空闲Worker每个Steal Round必须默认最多探测8个不重复victim，排除自身并使用可重现seed的伪随机/轮转选择；单轮失败后进入backoff/park流程，不得无限扫描。
Applies to: v0.2+ 多Worker Work-Stealing。
Exceptions: Worker数不足时探测所有可用其他Worker。
Source decisions: D-093
Source support: D-093 => probe默认值、唯一victim集合和有界轮次。
Code evidence: None
Disposition: implementation
Observable result: steal_attempt上界可测，固定seed可复现victim序列。

### R-065 — Park Handshake 防止 Ready/控制面丢唤醒
Status: active
Supersedes: None
Superseded by: None
Statement: Worker空闲必须先有限active backoff再可通知park；所有work publisher按 publish→advance epoch→notify，Worker登记park intent前后双检work、Shutdown与epoch；单个/批量work按可并行性唤醒，控制面变化notify-all，epoch饱和必须进入无ABA slow path或禁用park。
Applies to: Global/Local/DAG/Coroutine/timer publication与Worker退出。
Exceptions: pause/yield具体次数为内部benchmark参数。
Source decisions: D-094, D-095, D-096
Source support: D-094 => 有界backoff/park; D-095 => epoch双检; D-096 => 通知fanout与control all。
Code evidence: None
Disposition: implementation
Observable result: producer与park竞态不产生永久睡眠，空闲不busy-spin。

### R-066 — Chase-Lev 以 oracle 验证固定 portable memory order
Status: active
Supersedes: None
Superseded by: None
Statement: v0.3必须先保留行为等价seq_cst oracle；production使用uint64 atomic top/bottom、atomic active-buffer和relaxed atomic Task cells：push为relaxed bottom、acquire top、relaxed cell、release fence、relaxed bottom publication；pop为relaxed bottom decrement/store、seq_cst fence、relaxed top，多元素relaxed cell，last-item用seq_cst strong top CAS(failure relaxed)并恢复canonical bottom；steal为acquire top、seq_cst fence、acquire bottom/buffer、relaxed cell、seq_cst strong top CAS(failure relaxed)，成功后才使用cell；resize release-store active-buffer。任何弱化都需新决策，memory_order_consume不得使用。
Applies to: owner bottom push/pop与thief top steal。
Exceptions: R-101报告的Locked fallback不声称ChaseLevLockFree。
Source decisions: D-097, D-098
Source support: D-097 => oracle→production流程; D-098 => 原子序与last-item仲裁。
Code evidence: None
Disposition: implementation
Observable result: oracle/production通过同一functional stress，native AArch64验证weak-memory路径。

### R-067 — Deque growth 保留旧buffer并维持单一调度引用
Status: active
Supersedes: None
Superseded by: None
Statement: Chase-Lev buffer只能增长，旧buffer必须保留到deque quiescent teardown；Ready Task使用单一侵入式Scheduling Reference，resize cell复制不复制责任，Local growth/allocation失败必须回退Global且不得丢失、重复或错误完成Task。
Applies to: v0.3+ Local Deque resize与Task publication。
Exceptions: Runtime teardown达到quiescence后可释放全部历史buffer。
Source decisions: D-099, D-100
Source support: D-099 => retention/reclamation; D-100 => intrusive责任与fallback。
Code evidence: None
Disposition: implementation
Observable result: resize并发steal下每Task最多执行一次，故障注入仍可从Global取得工作。

### R-068 — Deque index、状态与算术不得依赖wrap
Status: active
Supersedes: None
Superseded by: None
Statement: Chase-Lev必须区分Success/Empty/Retry并对empty decrement、capacity doubling与索引执行checked arithmetic；接近高水位只能在quiescent状态rebase，不能依赖unsigned wrap，所需atomic不lock-free时选择Locked semantic fallback。
Applies to: v0.3+ Local Deque backend。
Exceptions: backend报告由R-101固定。
Source decisions: D-101, D-102, D-103
Source support: D-101 => lock-free条件/rebase/fallback; D-102 => 三态内部结果; D-103 => checked arithmetic。
Code evidence: None
Disposition: implementation
Observable result: 边界值测试不越界/ABA，Retry不被误报Empty。

### R-069 — TaskGraph consuming freeze 与 NodeId 验证固定
Status: active
Supersedes: None
Superseded by: None
Statement: `TaskGraph`必须是caller-serialized move-only builder，emplace返回graph-local强类型NodeId；`freeze() &&`消费并验证foreign/self/duplicate/cycle后产生immutable single-shot `FrozenTaskGraph`，失败抛 `astra::graph_validation_error : logic_error`且 `reason()`稳定返回GraphValidationError::{ForeignNode,SelfEdge,DuplicateEdge,Cycle}，Cycle携带首尾同Node的确定NodeId witness；NodeId从nonzero checked insertion sequence分配且不公开GraphNodeId别名。
Applies to: DAG定义阶段与Graph validation error。
Exceptions: 空图合法；freeze失败builder仅保证可析构/重新赋值。
Source decisions: D-104, D-105, D-161
Source support: D-104 => 三阶段/single-shot; D-105 => 验证与witness; D-161 => NodeId类型/耗尽/旧名消除。
Code evidence: None
Disposition: implementation
Observable result: 非DAG输入在admission前确定失败，freeze不重编号且move-only Node可用。

### R-070 — Graph admission 原子核算全部 Node并按完成发布依赖
Status: active
Supersedes: None
Superseded by: None
Statement: External `run(FrozenTaskGraph&&)`必须all-or-nothing为每Node占External slot并计outstanding，过大图立即CapacityExhausted，Internal图豁免slot；Node完成先发布Terminal，再对每edge exactly-once decrement，唯一1→0 owner acquire汇合后恰好一次Ready或传播Terminal。
Applies to: GraphRun admission、root publication与successor release。
Exceptions: empty graph占0 slot并立即完成。
Source decisions: D-106, D-107
Source support: D-106 => per-node atomic admission; D-107 => completion/countdown顺序。
Code evidence: None
Disposition: implementation
Observable result: 图不会部分接受，successor不会早启、重复Ready或永久漏release。

### R-071 — DAG 是 void 控制图并区分两种 Edge policy
Status: active
Supersedes: None
Superseded by: None
Statement: 普通Graph Node必须是返回void的one-shot控制任务且不提供per-node TaskHandle/隐式typed dataflow；Edge仅为RequireSuccess或AfterCompletion，Failed/Cancelled predecessor只把RequireSuccess descendants传播Cancelled，independent branch与AfterCompletion continuation继续。
Applies to: TaskGraph node body、edge与failure propagation。
Exceptions: R-077允许显式Task<void> Coroutine Node。
Source decisions: D-108, D-109, D-110
Source support: D-108 => void/no dataflow; D-109 => 两种策略; D-110 => 传播范围。
Code evidence: None
Disposition: implementation
Observable result: dependency failure不伪装为descendant failure，cleanup continuation仍运行。

### R-072 — GraphRun 提供显式取消、完整报告与 caller-relative 等待
Status: active
Supersedes: None
Superseded by: None
Statement: copyable/movable GraphRun必须支持default/moved-from empty与valid()；invalid的id/state/wait/wait_for/get_report抛logic_error而request_cancel为no-op。有效GraphRun提供id/state/wait/wait_for/get_report/request_cancel，wait_for返回GraphWaitResult::{Completed,TimedOut}；全部Node Terminal后一次发布按NodeId排序的immutable GraphReport，含run_id、Node总数、Succeeded/Failed/Cancelled counts、每个真实Failed Node的NodeId/TaskId/exception_ptr与内部取消原因counts，状态优先Failed>Cancelled>Succeeded且空图Succeeded；get_report或co_await标记全部真实Node异常observed，wait/state不标记；同步等待复用R-052/R-056且own GraphRun wait拒绝。
Applies to: 单次Graph execution观察与全图取消。
Exceptions: request_cancel对Running/Suspended Node沿各自cooperative规则且不等待完成。
Source decisions: D-111, D-112, D-113, D-152
Source support: D-111 => 全图请求; D-112 => report/state; D-113 => waits/self; D-152 => observation计数。
Code evidence: None
Disposition: implementation
Observable result: 并发失败无任意first-error丢失，Graph aggregate不制造synthetic exception。

### R-073 — Coroutine Task cold且spawn强保证移交frame
Status: active
Supersedes: None
Superseded by: None
Statement: `astra::Task<T>` 必须是initial-suspend的cold、move-only、single-shot frame owner；`spawn/try_spawn`成功把frame一次移交Runtime并返回统一TaskHandle，admission失败保持调用方Task/frame可销毁或重试且不执行body。
Applies to: C++20 Coroutine创建与Runtime admission。
Exceptions: coroutine function call时frame allocation/parameter copy异常发生在spawn前。
Source decisions: D-114, D-115
Source support: D-114 => cold/single-owner; D-115 => strong transfer/统一Handle。
Code evidence: None
Disposition: implementation
Observable result: body只在Worker首次resume执行，frame始终恰有一个owner。

### R-074 — Coroutine resume ownership 与 await handshake 唯一
Status: active
Supersedes: None
Superseded by: None
Statement: 每个Coroutine Task同一时刻只能有一个resume owner；segment在Ready/Running/Suspended间发布且final_suspend保留frame直到Runtime在Terminal publication后恰好一次destroy；所有内建awaiter必须用generation-scoped arm-trigger handshake保证并发completion/stop最多发布一个Ready ticket且不在await_suspend返回前resume。
Applies to: 每次Coroutine resume、suspend、final destroy与内建awaitable。
Exceptions: foreign awaitable内部协议不由Runtime控制。
Source decisions: D-116, D-117, D-118
Source support: D-116 => unique resume ownership; D-117 => final destroy; D-118 => arm-trigger。
Code evidence: None
Disposition: implementation
Observable result: 无并发/递归double-resume、lost wake或double-destroy。

### R-075 — Suspended取消与 Immediate 只恢复已开始frame
Status: active
Supersedes: None
Superseded by: None
Statement: Suspended Coroutine收到取消时不得直接destroy或伪造Cancelled；内建cancellation-aware awaiter由stop winner撤销正常registration、发布source-Runtime Ready并在await_resume抛task_cancelled，foreign awaitable仅保留stop request且可永久挂起；Immediate禁止never-started frame首次start，但允许already-started resume segment运行到合作取消或自然完成。
Applies to: Task/Graph/Shutdown/Finalization cancellation of Coroutine。
Exceptions: 用户可捕获task_cancelled并继续，Runtime不保证有界终结。
Source decisions: D-119, D-154
Source support: D-119 => 内建/foreign suspension取消; D-154 => Immediate first-start与resume区分。
Code evidence: None
Disposition: implementation
Observable result: frame不在suspend点被异步销毁，Immediate仍可执行必要unwind segment。

### R-076 — Astra await 仅通过 source Runtime 异步恢复
Status: active
Supersedes: None
Superseded by: None
Statement: 左值TaskHandle/GraphRun的`co_await`必须注册continuation并只经awaiter所属source Runtime排队恢复，不inline resume、不让source执行target Runtime；TaskHandle传播同一Outcome，GraphRun返回同一Report，self-task/self-run拒绝；`cancellation_point`不挂起，`yield`必须总是挂起当前segment并经ordinary Global排队后恢复。
Applies to: 已spawn Astra Coroutine内的组合await。
Exceptions: target已完成时await_ready可不挂起；Ready destination服从R-063。
Source decisions: D-120, D-121, D-122, D-147
Source support: D-120 => TaskHandle await; D-121 => GraphRun await; D-122 => cancellation/yield; D-147 => routing precedence。
Code evidence: None
Disposition: implementation
Observable result: await不形成跨Runtimesteal或递归resume，yield产生可见调度边界。

### R-077 — Graph Coroutine Node 复用同一Node Task identity
Status: active
Supersedes: None
Superseded by: None
Statement: TaskGraph必须以显式 `emplace_coroutine(Task<void>&&)` 接受cold Coroutine并绑定同一NodeId/TaskId，不创建child Handle、第二identity、额外slot或outstanding count；普通emplace不得隐式unwrap Task，`Task<T>`本身不得直接co_await而必须先spawn。
Applies to: DAG与Coroutine组合及Task ownership。
Exceptions: 非voidCoroutine Node编译期拒绝。
Source decisions: D-123, D-124
Source support: D-123 => graph node整合; D-124 => Task不可直接await。
Code evidence: None
Disposition: implementation
Observable result: Graph coroutine node在Metrics/Trace/Outcome中只计一个Task identity。

### R-078 — Blocking/async组合API不扩张
Status: active
Supersedes: None
Superseded by: None
Statement: 当前稳定Task/Graph同步与Coroutine API不得增加wait_until、带stop_token的blocking wait或callback completion注册接口。
Applies to: TaskHandle、GraphRun与Finalization以外的completion surface。
Exceptions: 后续accepted decision可新增非冲突能力。
Source decisions: D-125
Source support: D-125 => 明确延期三个API族。
Code evidence: None
Disposition: implementation
Observable result: public API inventory只有wait/wait_for/get/co_await等已批准入口。

### R-079 — Coroutine Timer 由Worker驱动且Wake Time只限定eligibility
Status: active
Supersedes: None
Superseded by: None
Statement: `sleep_until(steady_clock::time_point)`与`sleep_for(duration)` awaiter必须使用steady-clock Wake Time、支持取消并注册到Runtime-wide indexed timer heap，由Worker在park deadline前后驱动而不得新增Timer thread；到时只使Task可Ready且经ordinary Global恢复，不保证最大jitter；timer属于原Task/Drain Closure，Graceful保留，Immediate取消恢复。
Applies to: `sleep_for/sleep_until`内建Coroutine等待。
Exceptions: 饱和到time_point::max的timer可使Graceful无界等待。
Source decisions: D-126, D-127, D-128
Source support: D-126 => steady/cancellation API; D-127 => Worker heap/index; D-128 => best-effort与drain。
Code evidence: None
Disposition: implementation
Observable result: Runtime无额外timer线程，Wake Time前不因该timer恢复，取消可撤销heap entry。

### R-080 — Priority 在 admission 解析并固定
Status: active
Supersedes: None
Superseded by: None
Statement: `Priority::{Low,Normal,High,Critical}`必须作为不可变base hint由TaskOptions显式配置；submit/try_submit/spawn/try_spawn与Graph emplace/emplace_coroutine提供options-first overload。无options External/cross-runtime为Normal，same-runtime Internal默认继承current Task，Graph Node继承GraphRun提交上下文，显式options总覆盖；不得提供动态set/boost或OS priority映射。
Applies to: Callable、Coroutine与Graph Node Task admission。
Exceptions: Deadline不继承，见R-082。
Source decisions: D-129
Source support: D-129 => 四级枚举、解析/继承/覆盖与排除动态优先级。
Code evidence: None
Disposition: implementation
Observable result: 同一Task所有resume segment使用相同base Priority。

### R-081 — 每个Ready source按四band 8:4:2:1非抢占服务
Status: active
Supersedes: None
Superseded by: None
Statement: Global与每个Local source必须分为四Priority band并以Critical:High:Normal:Low=8:4:2:1的确定加权机会选择非空band；空band机会可跳过但低优先级持续Ready时不得永久饿死，Priority只影响下一个claim且不得抢占Running segment。
Applies to: v0.6+ Ready source内部选择。
Exceptions: Local/Global outer service仍服从R-063。
Source decisions: D-130, D-131
Source support: D-130 => 每source四band; D-131 => 权重/公平/非抢占。
Code evidence: None
Disposition: implementation
Observable result: 饱和基准长期服务比例接近8:4:2:1且每band有进展。

### R-082 — TaskDeadline 是显式首次开始目标
Status: active
Supersedes: None
Superseded by: None
Statement: 最终 `TaskOptions`值类型必须含 `Priority priority{Normal}` 与 `optional<TaskDeadline> deadline{}`；TaskDeadline包装steady_clock绝对时刻并由at/after构造，after在factory调用时checked/saturating固定；它仅表示首次成功Running的best-effort目标，不是Wake Time/完成期限/取消时刻，不继承、不动态修改，miss只记录而不改变Outcome或执行。
Applies to: TaskOptions中optional deadline。
Exceptions: 无deadline Task不参与deadline disposition。
Source decisions: D-132
Source support: D-132 => 类型、factory、first-start、无继承/取消语义。
Code evidence: None
Disposition: implementation
Observable result: 相同absolute deadline不因admission延迟重新计时，missed Task仍正常执行。

### R-083 — Deadline 使用Global indexed EDF且Priority主导
Status: active
Supersedes: None
Superseded by: None
Statement: never-started Deadline Task必须进入Runtime-wide按Priority分区的indexed EDF heap，支持start/cancel O(log n)删除且由同band最早deadline优先；Priority band选择仍按R-081主导，无deadline ordinary work在同band获得有界服务，deadline Task首次start后resume不再进入EDF。
Applies to: v0.6+ first-start scheduling。
Exceptions: miss不抢占、不自动取消且无硬时延保证。
Source decisions: D-133, D-134, D-147
Source support: D-133 => heap/index/first-start; D-134 => Priority主导和ordinary公平; D-147 => 首次start后routing。
Code evidence: None
Disposition: implementation
Observable result: 同banddeadline顺序可测，低Priority早deadline不越过band策略抢占高Priority。

### R-084 — Metrics level 与 Basic事件schema固定
Status: active
Supersedes: None
Superseded by: None
Statement: Scheduler必须提供Off/Basic/Detailed且默认Basic；Off不维护measurement。Basic固定counter为submission_attempts、accepted_task_identities、rejected_lifecycle、rejected_capacity、blocking_submit_waits/wakeups、first_starts、resume_segments、succeeded、failed、cancelled_before_start、cancelled_cooperative、unobserved_failures、global/local_claims、steal_attempts/successes/failures、worker_parks/wakes、explicit_yields、coroutine_suspends、timer_registrations/fires/cancellations、graph_admission_attempts/runs_accepted/runs_rejected/nodes_terminal、deadline_admitted/met/missed/cancelled_before_start；固定gauge为waiting/ready/running/suspended_tasks、external_pending_slots_used、parked_workers、active_timer_entries、active_graph_runs。字段以per-worker/external/control shard饱和到UINT64_MAX并sticky saturated，不得有TaskId/NodeId/字符串高基数label。
Applies to: per-Runtime Metrics热路径与schema。
Exceptions: process-wide生命周期指标由R-095独立提供且始终可用。
Source decisions: D-135, D-136, D-151
Source support: D-135 => levels/default/Off; D-136 => fixed schema/shards/saturation; D-151 => Off无hidden unobserved counter。
Code evidence: None
Disposition: implementation
Observable result: Metrics启用不改变Task语义，长期counter不wrap倒退。

### R-085 — Metrics Snapshot fuzzy且Detailed使用固定log2 histogram
Status: active
Supersedes: None
Superseded by: None
Statement: `metrics_snapshot()`必须返回immutable、逐字段race-free的fuzzy snapshot并记录capture_started_at/capture_finished_at、level与saturated，运行中不声称全局单点一致；quiescent point满足R-084守恒。Detailed使用64个base-2纳秒bucket（0含0–1ns，后续[2^(n-1),2^n)，末桶吸收溢出）及饱和count/sum_ns/max_ns，固定记录ready_queue_wait、execution_segment、task_wall_time、blocking_admission_wait、timer_wake_lateness、deadline_start_lateness(仅miss)、worker_park_duration、runtime_join_latency；不存raw sample、不在Runtime算percentile、不stop-the-world或重置counter。
Applies to: Runtime metrics读取与offline analysis。
Exceptions: Off返回明确disabled/empty schema而不读未初始化shard。
Source decisions: D-137
Source support: D-137 => snapshot一致性边界、quiescent验证与Detailed histogram。
Code evidence: None
Disposition: implementation
Observable result: 并发snapshot字段各自有效，静止后accepted/outcome/steal等关系收敛。

### R-086 — Trace capture 有界、可重复且显式提交
Status: active
Supersedes: None
Superseded by: None
Statement: 线程安全shared TraceCollector必须显式附加并一次只允许一代Recording；TraceOptions默认events_per_worker=16,384、external_control_events=65,536、events_per_reaper_producer=4,096，Default启用Task/queue/claim/steal-success/Wait/Await/Coroutine/Graph/Timer/Deadline/Runtime/Reaper而逐steal-attempt/Verbose关闭。三个容量须非零，未知bit/零值在状态改变前抛invalid_argument，总buffer算术溢出抛length_error，分配失败保留Stopped和上一snapshot并重抛bad_alloc；start_capture在Recording前完成全部producer预分配，Recording中新Scheduler附加失败则startup rollback。emit不得分配/I/O/callback/block且满时drop-newest计loss；只有move-only TraceCapture显式stop产生可复制immutable Snapshot，重复/并发stop共享结果，活动析构noexcept disable/quiesce并丢弃该代。
Applies to: 多Runtime/Reaper共享capture。
Exceptions: 未附加/Stopped/category disabled为fast no-op且不算drop。
Source decisions: D-138, D-158, D-163
Source support: D-138 => collector/buffer/stop; D-158 => 默认容量/category/strong start; D-163 => explicit stop/destructor abort。
Code evidence: None
Disposition: implementation
Observable result: buffer overflow或异常展开不阻塞Scheduler，Collector可安全启动下一代。

### R-087 — TraceEvent 使用版本化固定记录与逻辑ID
Status: active
Supersedes: None
Superseded by: None
Statement: TraceEvent必须是trivially-copyable固定schema，含schema_version、capture-relative steady timestamp、EventKind、Producer/local sequence、RuntimeId/WorkerId/TaskId及可选GraphRunId/NodeId/SegmentSequence和Priority/source/TaskState/Outcome/reason/deadline枚举；EventKind至少覆盖admission/rejection、Ready/claim/first-start/segment-end/Terminal/cancel、Local/Global/steal-success、park/wake、suspend/resume/yield、timer register/fire/cancel、Graph accepted/terminal/dependency release、deadline met/miss、wait/await与runtime handoff/join/finalization。枚举值显式版本化，不得保存raw pointer、用户字符串/payload或异常文本；每producer timestamp不降且sequence递增，跨producer只按(timestamp,ProducerId,sequence)确定merge且不宣称全局线性化。
Applies to: Task/queue/steal/wait/coroutine/timer/graph/deadline/runtime/finalization事件。
Exceptions: invalid sentinel用于缺失identity。
Source decisions: D-139, D-153
Source support: D-139 => record/schema/order; D-153 => public logical IDs。
Code evidence: None
Disposition: implementation
Observable result: 地址复用不造成identity冲突，相同snapshot可确定重放排序。

### R-088 — Chrome Trace 只离线确定导出并显式报告损失
Status: active
Supersedes: None
Superseded by: None
Statement: 只有Stopped TraceSnapshot可由ostream工具离线导出Chrome Trace JSON；export必须确定merge、校验schema/identity/segment并保存capacity/category/recorded/dropped，任意loss仍输出有效JSON但标 `trace_complete=false`，不得合成事件掩盖缺口；core不得接受path或在Runtime线程写文件。
Applies to: Chrome Trace exporter与artifact完整性。
Exceptions: pretty-print可改变字节格式；关闭category不算loss。
Source decisions: D-140
Source support: D-140 => offline API、determinism、loss与corruption规则。
Code evidence: None
Disposition: implementation
Observable result: 零loss相同snapshot/版本byte-stable，损坏输入明确失败且原snapshot可重试。

### R-089 — Benchmark 分为 micro harness 与独立场景runner
Status: active
Supersedes: None
Superseded by: None
Statement: Benchmark Framework必须用pinned Google Benchmark承载纯机制micro case，并用独立 `astra_bench_scenarios` 承载多阶段/生命周期case；setup、warmup、timed region、drain verification与teardown必须分离，checksum/rejection/drop/子进程异常使sample invalid而非产生性能值。
Applies to: v0.8 benchmark targets与CI smoke。
Exceptions: consumer build默认不构建/下载benchmark依赖。
Source decisions: D-141
Source support: D-141 => 双层架构、阶段隔离与validity gate。
Code evidence: None
Disposition: implementation
Observable result: 计时区不混入构建/销毁，错误工作量不能被报告为更快。

### R-090 — Benchmark corpus 使用语义基线和受限外部背景
Status: active
Supersedes: None
Superseded by: None
Statement: corpus必须覆盖Global FIFO、locked Work-Stealing、Chase-Lev及micro/CPU/imbalanced/fork-join/DAG/Coroutine/timer/Priority/Deadline/shutdown/reaper组合；Global FIFO是primary correctness/regression baseline，oneTBB可选，`std::async(std::launch::async)`只用于粗粒度独立背景且不得参与递归/DAG等feature ranking。
Applies to: fixed workload corpus与adapter比较。
Exceptions: 外部adapter缺少等价语义的case应标not comparable。
Source decisions: D-142, D-150
Source support: D-142 => corpus/baselines; D-150 => std::async边界。
Code evidence: None
Disposition: implementation
Observable result: artifact明确adapter限制，不把线程拓扑不同的std::async当主回归oracle。

### R-091 — Benchmark artifact 保存原始重复并限定回归gate
Status: active
Supersedes: None
Superseded by: None
Statement: Standard profile必须默认2秒warmup、10个至少1秒独立repetition且不删outlier；versioned artifact保存全部raw values、median/MAD/p10/p90/bootstrap95%CI、环境/构建/options/seed/checksum/schema；共享PR CI只smoke，正式regression只在专用稳定runner同时越过versioned实践阈值与置信区间，baseline更新需review。
Applies to: performance claim、release evidence与regression automation。
Exceptions: 非Standard exploratory profile必须在artifact显式命名参数。
Source decisions: D-143
Source support: D-143 => profile、artifact字段与gate策略。
Code evidence: None
Disposition: implementation
Observable result: 性能结论可追溯原始重复，偶发共享runner噪声不阻断发布。

### R-092 — Compiled library、平台Tier与单实现实例固定
Status: superseded
Supersedes: None
Superseded by: R-111
Statement: AstraScheduler必须是C++20 exception-enabled compiled CMake library target `AstraScheduler::AstraScheduler`，static默认/shared可选；Tier-1为Linux x86_64 GCC13+/Clang17+与Windows x64 MSVC19.38+，Tier-2 native Linux AArch64；受支持进程只能加载一个implementation instance，多DSO必须共同链接同一exact-version shared library。
Applies to: build/install/release与process-wide Reaper/ID/metrics保证。
Exceptions: unsupported平台仅best-effort；单可执行程序的一份static支持。
Source decisions: D-167, D-159
Source support: D-167 => 记录并整体取代本历史Linux/Windows平台与compiled-library契约; D-159 => 本历史规则中的one-instance部署边界仍由R-107保留。
Code evidence: None
Disposition: implementation
Observable result: install consumer smoke通过；多份vendored static DSO配置明确拒绝支持。

### R-093 — SemVer保证source/semantic并分离header/library version
Status: active
Supersedes: None
Superseded by: None
Statement: 0.x minor可经decision+migration产生breaking change而patch不得计划性breaking，v1起documented source/observable semantics按SemVer；不保证跨版本/toolchain ABI。必须公开可比较的 `Version{uint32 major,minor,patch}`、ASTRA_VERSION_MAJOR/MINOR/PATCH、constexpr header_version()及无分配/无锁/不初始化Runtime的library_version()/library_version_string()，后者string_view指向进程期静态规范SemVer文本；schema版本独立，CMake exact-version检查为主要mismatch边界。
Applies to: release、shared package、artifact与consumer诊断。
Exceptions: runtime查询不使错误header/binary组合成为受支持。
Source decisions: D-145, D-164
Source support: D-145 => SemVer/ABI/schema; D-164 => 版本API与无副作用。
Code evidence: None
Disposition: implementation
Observable result: 同一安装header/library版本一致，查询不启动Reaper或分配。

### R-094 — Phase 0 至 v1.0 按纵向里程碑交付
Status: active
Supersedes: None
Superseded by: None
Statement: 路线必须依次以Phase0 scaffold、v0.1 Global Runtime+Task/lifecycle、v0.2 locked WS、v0.3 Chase-Lev、v0.4 DAG、v0.5 Coroutine+Timer、v0.6 Priority+Deadline、v0.7 Observability、v0.8 Benchmark、v0.9 hardening、v1 stable source API交付，每tag满足approved-rule测试、Tier-1 build、并发证据、docs/package/schema/benchmark gates。
Applies to: release规划与后续to-tickets。
Exceptions: private seam可提前，未定public语义不得提前暴露。
Source decisions: D-146
Source support: D-146 => 完整里程碑、DoD与release gate。
Code evidence: None
Disposition: documentation-only
Observable result: 每个实现Ticket有目标版本且每个tag可独立构建运行。

### R-095 — Process Metrics 只观察Reaper/Finalization且查询不初始化
Status: active
Supersedes: None
Superseded by: None
Statement: `astra::process_metrics_snapshot()`必须始终提供固定counter runtime_registrations、runtime_handoffs、runtimes_joined、finalization_begin_calls、finalization_wait_timeouts、finalization_escalations，固定gauge registered_runtimes、pending_runtimes、join_ready_runtimes，以及ProcessServiceState、FinalizationState、capture steady time、finalization elapsed/completion duration和saturated；调用前返回NotStarted/零且不得初始化服务，Finalized后保留终值，不聚合Runtime task counters，逐字段安全并沿用fuzzy标记/区间语义。
Applies to: process-wide coordinator lifecycle诊断。
Exceptions: per-Runtime task metrics由R-084/R-085提供。
Source decisions: D-148
Source support: D-148 => API、side-effect-free阶段值、字段范围与非聚合边界。
Code evidence: None
Disposition: implementation
Observable result: 未创建Scheduler时查询无线程副作用，finalization超时/升级可离线诊断。

### R-096 — Wait/Await edge 可观察但不形成在线依赖图
Status: active
Supersedes: None
Superseded by: None
Statement: Runtime Metrics必须记录task/graph waits、timeouts、same/cross-runtime Helping、Coroutine awaits与self/depth rejection，Detailed记录duration histogram；Trace在启用时发WaitBegin/End和AwaitArmed/Triggered/Resumed并携带source/target logical IDs，但Runtime不得据此维护在线wait-for graph或自动解环。
Applies to: TaskHandle/GraphRun同步与Coroutine await observability。
Exceptions: Metrics Off/Trace disabled按各自fast path。
Source decisions: D-050, D-051, D-149
Source support: D-050 => 不在线检测; D-051 => cross-runtime身份; D-149 => counters/histogram/events。
Code evidence: None
Disposition: implementation
Observable result: 离线trace可重建wait edge，运行语义不受诊断启发式改变。

### R-097 — Scheduler startup transaction 与 Finalization close 唯一排序
Status: active
Supersedes: R-029
Superseded by: None
Statement: `Scheduler(options)`必须同步验证、预留/注册Reaper、创建Worker并经barrier一次发布Running后才返回，失败阻止用户工作并完整join/rollback；Running publication与Finalization close线性排序，close先则startup不开放admission并回滚后抛FinalizationStarted creation rejection，Running先则构造成功且随后可立即Graceful Stopping。
Applies to: Scheduler创建、Finalization核算集合与startup race。
Exceptions: 无Runtime的空集合begin可直接Finalized。
Source decisions: D-023, D-024, D-155, D-156
Source support: D-023/D-024 => 永久close与Graceful核算; D-155 => constructor transaction; D-156 => 两种竞态结果。
Code evidence: None
Disposition: implementation
Observable result: 不存在public Created/Starting或半启动Handle，竞态Scheduler恰好成功纳入或零用户工作失败。

### R-098 — SchedulerOptions 只公开稳定policy且startup冻结
Status: active
Supersedes: None
Superseded by: None
Statement: SchedulerOptions必须使用 `recommended_worker_count()`、external capacity 65536、Reject、helping64、local burst64、steal probes8、Metrics Basic及空TraceCollector默认；所有size值必须大于0且unknown enum拒绝，options在注册/ID/Worker前验证并冻结，spin/deque/timer/priority内部tuning不得公开。
Applies to: Scheduler startup配置与Metrics/Benchmark resolved options。
Exceptions: recommended_worker_count的hardware_concurrency为0时返回1。
Source decisions: D-078, D-157
Source support: D-078 => helping默认; D-157 => 完整字段/default/validation/frozen与排除knob。
Code evidence: None
Disposition: implementation
Observable result: invalid配置在无Runtime副作用前抛invalid_argument，调用方后改原options不影响Runtime。

### R-099 — Scheduler status 是state/mode成对快照
Status: active
Supersedes: None
Superseded by: None
Statement: 有效Scheduler的 `status()`必须非阻塞、无副作用且一次线性化返回仅有Running+None、Stopping+Graceful/Immediate、Stopped+最终mode的pair；不得提供独立is_running/is_stopped/mode getter，snapshot可立即过时且不授予admission能力，空Scheduler抛logic_error。
Applies to: Scheduler lifecycle观察。
Exceptions: process Finalization state由R-095观察。
Source decisions: D-160
Source support: D-160 => API、合法pair、empty与check-then-act限制。
Code evidence: None
Disposition: implementation
Observable result: 并发shutdown时不返回撕裂pair，submit仍以自身transaction决定结果。

### R-100 — 公共逻辑ID强类型且不wrap/reuse
Status: active
Supersedes: None
Superseded by: None
Statement: RuntimeId、TaskId、GraphRunId与NodeId必须是default-zero-invalid、trivially-copyable强值类型，支持valid/equality/order/hash且无隐式整数/指针转换；有效Scheduler::runtime_id、TaskHandle::id、GraphRun::id与GraphReport::run_id返回对应稳定值。Runtime/Task/GraphRun sequence为checked nonzero monotonic不wrap，NodeId仅graph-local；完整运行Node关联GraphRunId+NodeId+TaskId，ID不授予lookup/control/lifetime。
Applies to: Scheduler/Handle/GraphReport/Metrics/Trace/Benchmark identity。
Exceptions: sequence gap允许，跨进程不承诺唯一。
Source decisions: D-153, D-161
Source support: D-153 => Runtime/Task/GraphRun API与exhaustion; D-161 => NodeId统一与scope。
Code evidence: None
Disposition: implementation
Observable result: 地址复用不改变身份，耗尽在startup/admission前抛overflow_error而不复用。

### R-101 — SchedulerCapabilities 报告实际Local Deque backend
Status: active
Supersedes: None
Superseded by: None
Statement: 有效Scheduler的 `capabilities()`必须返回不可由用户aggregate-initialize的trivially-copyable immutable `SchedulerCapabilities`，其 `local_deque_backend()`为 `LocalDequeBackend::{None,Locked,ChaseLevLockFree}`之一且 `lock_free_local_deque()`仅最后一种为true；v0.1为None、v0.2/fallback为Locked，Stopped后保留且不得运行时切换或按版本推断，空Scheduler抛logic_error。
Applies to: Runtime、Trace metadata与Benchmark artifact。
Exceptions: 该能力不声称整个Runtime lock-free。
Source decisions: D-101, D-167, D-162
Source support: D-101/D-167 => Linux支持平台上的fallback/report要求; D-162 => enum/API/各版本映射。
Code evidence: None
Disposition: implementation
Observable result: 同版本不同atomic平台可诚实报告不同backend，artifact复用同一snapshot。

### R-102 — submit decay-own并一次性rvalue调用move-only工作
Status: active
Supersedes: None
Superseded by: None
Statement: submit/try_submit必须对F/Args以decay_t和完美转发构造owned capture，并以stored rvalue恰好调用一次，支持move-only target/argument和operator()&&；真实引用仅通过std::ref显式表达，traits与R-054 token fallback必须基于同一stored-rvalue expression，copy-only std::function不得缩窄能力。
Applies to: v0.1起普通Task与Graph emplace的一次性work storage。
Exceptions: Coroutine frame按R-073 ownership转移而不二次capture。
Source decisions: D-059, D-165
Source support: D-059 => ordinary-first/token form; D-165 => decay storage、rvalue invocation与move-only requirement。
Code evidence: None
Disposition: implementation
Observable result: move-only Callable/unique_ptr参数可提交，lvalue-only target无wrapper时编译期拒绝。

### R-103 — 只有最后一个非Worker Scheduler Handle释放触发同步RAII
Status: active
Supersedes: R-017
Superseded by: None
Statement: Scheduler必须是copyable/movable shared Handle，普通副本销毁不得关停；仅最后一个Handle释放触发RAII，非目标Worker上Running发起Graceful、Stopping保留mode，目标Worker则按R-021/R-022 handoff；空/moved-from操作除valid/destruction外抛logic_error。
Applies to: Scheduler shared Handle lifetime与RAII策略选择。
Exceptions: 已Stopped最后释放只回收；显式shutdown可先完成。
Source decisions: D-014, D-017, D-018, D-155
Source support: D-014 => 非WorkerGraceful策略; D-017/D-018 => Worker handoff; D-155 => copyable/move/last Handle/empty。
Code evidence: None
Disposition: implementation
Observable result: 销毁一个非最后副本不改变status/admission，最后释放才按caller选择RAII或handoff。

### R-104 — Finalization 对已核算与启动中 Runtime 使用 Graceful
Status: active
Supersedes: R-030
Superseded by: None
Statement: Finalization必须对close前已核算Runtime请求Graceful且不降级既有Immediate；close先于Running publication的startup必须在开放用户工作前观察sticky请求并rollback，Running先发布则构造成功并作为核算成员可立即进入Graceful Stopping。
Applies to: Finalization accounted set与Scheduler startup race的shutdown mode。
Exceptions: 显式shutdown_now/request_immediate可单向升级。
Source decisions: D-024, D-156
Source support: D-024 => accounted set默认Graceful与mode保持; D-156 => close-first rollback/Running-first成功。
Code evidence: None
Disposition: implementation
Observable result: Finalization不因进程收尾默认取消已接受工作，半启动Runtime不获得用户执行窗口。

### R-105 — 最后非Worker Handle析构是noexcept同步完成边界
Status: active
Supersedes: R-018
Superseded by: None
Statement: 最后一个Scheduler Handle在非目标Worker释放时，析构必须noexcept并等待Drain Closure、全部Worker join与Stopped真实发布，允许无界阻塞且不得detach或伪造完成。
Applies to: R-103选择的非Worker RAII路径。
Exceptions: 目标Worker最后释放使用R-021/R-022的异步handoff。
Source decisions: D-014, D-155
Source support: D-014 => noexcept同步回收/无界; D-155 => 仅最后shared Handle触发。
Code evidence: None
Disposition: implementation
Observable result: 析构返回后无Worker访问Runtime，不合作任务保持析构未返回。

### R-106 — Immediate 只直接取消从未首次 start 的任务
Status: active
Supersedes: R-008
Superseded by: None
Statement: Immediate Shutdown线性化后，所有已接受且从未成功首次进入Running的Waiting/Ready Task必须恰好一次发布Cancelled、唤醒等待者且永不执行用户Callable/frame；已经首次start后处于Suspended的Coroutine不得按“非Running”直接完成，而按R-075发布stop并经resume segment到达合作取消或自然完成。
Applies to: Immediate、Graceful→Immediate与Finalization escalation的Task分类。
Exceptions: 当前Running Task按R-009；已Terminal不改写。
Source decisions: D-006, D-154
Source support: D-006 => never-started accepted work取消/唤醒/不执行; D-154 => already-started suspended resume例外。
Code evidence: None
Disposition: implementation
Observable result: never-started work不执行且Handle完成；already-started frame仍有机会运行取消点与RAII unwinding。

### R-107 — Supported Configuration 只有一个实现实例与Reaper coordinator
Status: active
Supersedes: R-027
Superseded by: None
Statement: R-111定义的Linux-only Supported Configuration中，一个进程必须只加载一个Astra Implementation Instance，并由其中一个逻辑Reaper Service和恰好一条不属于任何Scheduler的专用coordinator服务全部Runtime；不得按Scheduler/handoff增加Reaper线程，coordinator不得执行用户任务或参与steal；多个DSO各自静态嵌入实现不享有这些process-wide保证。
Applies to: process-wide Reaper、Finalization gate、ID allocator与Process Metrics拓扑。
Exceptions: 从未建立服务且空集合Finalization不创建coordinator。
Source decisions: D-021, D-159
Source support: D-021 => 单服务/单线程/角色排除; D-159 => one-instance支持前提与multi-static DSO排除。
Code evidence: None
Disposition: implementation
Observable result: 支持配置中Scheduler数量不增加coordinator数，unsupported duplicate instance被部署文档/测试明确拒绝。

### R-108 — 同 Runtime Worker self-shutdown 抛 logic_error
Status: active
Supersedes: None
Superseded by: None
Statement: `void Scheduler::shutdown()`与`void Scheduler::shutdown_now()`不得标记noexcept；当前同Runtime Worker调用任一方法必须在读取/改变lifecycle、关闭admission、发布stop、取消Task、认领join或等待前抛 `std::logic_error`，异常文本不稳定；其他Runtime Worker仍按目标Runtime的非Worker同步语义执行。
Applies to: Scheduler shutdown caller classification与公共异常边界。
Exceptions: empty/moved-from Scheduler也按R-103抛logic_error，但原因不通过enum区分。
Source decisions: D-009, D-011, D-166
Source support: D-009/D-011 => 两种self-shutdown副作用前拒绝; D-166 => void/non-noexcept签名、logic_error类型与caller矩阵。
Code evidence: None
Disposition: implementation
Observable result: same-runtime Worker得到logic_error且状态不变，other-runtime Worker仍等待目标真实Stopped。

### R-109 — Logging 与 Trace 分离且不记录每Task热路径
Status: active
Supersedes: None
Superseded by: None
Statement: Logging必须与Trace使用独立sink/锁并仅承载低频ERROR/WARN/INFO控制面诊断；Worker每Task/queue/steal事件不得同步写日志，Trace emit不得调用logger，Benchmark除专用observability-overhead case外关闭Trace和高频日志并在artifact记录启用状态。
Applies to: Runtime/Reaper diagnostics、Trace与Benchmark。
Exceptions: D-040 fail-fast前可执行noexcept尽力诊断。
Source decisions: D-140
Source support: D-140 => 日志/Trace分离、低频控制面、禁止每Task同步日志与Benchmark默认关闭。
Code evidence: None
Disposition: implementation
Observable result: Task hot path不获取logger I/O锁，Trace overflow/export不递归进入日志系统。

### R-110 — CMake package 隐藏实现并验证独立consumer
Status: active
Supersedes: None
Superseded by: None
Statement: public headers必须仅安装于include/astra且不泄漏internal/第三方依赖，CMake target声明cxx_std_20并导出AstraSchedulerConfig.cmake/version file；tests/examples/benchmarks/tools由ASTRA_BUILD_*控制且consumer默认不下载其依赖；static/shared共用语义/tests，public symbol经export macro控制，internal symbol hidden，独立find_package/link/run smoke必须通过；不支持-fno-exceptions，core不要求RTTI。
Applies to: Phase0、install/export、static/shared release package。
Exceptions: warnings-as-errors、sanitizer与内部编译选项不传播给consumer。
Source decisions: D-167, D-145
Source support: D-167 => compiled target、header/package/options/exception、Linux-only consumer smoke; D-145 => symbol visibility和shared/static compatibility边界。
Code evidence: None
Disposition: implementation
Observable result: 安装目录可被独立最小工程消费，consumer compile line不包含项目内部依赖或强制诊断选项。

### R-111 — Supported Configuration 仅限 64-bit Linux
Status: active
Supersedes: R-092
Superseded by: None
Statement: AstraScheduler的Supported Configuration必须仅包含64-bit Linux：Tier-1为Linux x86_64 GCC13+/Clang17+，Tier-2为native Linux AArch64 GCC/Clang weak-memory验证；Windows/MSVC、macOS、其他非Linux OS与32-bit目标必须标记为unsupported且不得进入release gate、package支持声明或性能/正确性承诺，偶然编译成功不得升级支持状态。
Applies to: build/install/release、CI、package metadata、Benchmark artifact与公开支持文档。
Exceptions: compiled CMake package由R-110约束，single implementation instance由R-107约束；Linux atomic能力不足时按R-101报告Locked fallback而不改变平台支持状态。
Source decisions: D-167
Source support: D-167 => 直接固定Linux-only范围、Tier-1/Tier-2矩阵、非Linuxunsupported及release声明边界。
Code evidence: 当前`.github/workflows/ci.yml`只使用Linux runner；完整编译矩阵尚待实现。
Disposition: implementation
Observable result: release matrix和package支持声明只包含Linux x86_64 GCC/Clang与native Linux AArch64，不存在Windows/MSVC release job；非Linux结果不得标记Supported。

### R-112 — 本机开发与验证命令必须在 WSL Linux 内执行
Status: active
Supersedes: None
Superseded by: None
Statement: 所有本机configure、build、test、format、lint、package consumer、sanitizer、stress、benchmark与release verification命令必须在WSL Linux用户空间执行，canonical workspace为`/mnt/d/code/cppStudy/AstraScheduler`；Windows PowerShell/cmd仅可启动WSL或做非开发性宿主编排，不得直接运行Windows-native toolchain或项目二进制形成验证证据，Windows与WSL不得复用build cache或产物。
Applies to: 本机开发者、Coding Agent、Ticket verification与本机Benchmark artifact。
Exceptions: 文件编辑工具可操作共享工作区；CI/release可以在native Linux runner执行，WSL不是最终运行时依赖；本机WSL benchmark的正式性能claim仍需按R-089至R-091记录环境并可由native Linux复核。
Source decisions: D-168
Source support: D-168 => 直接固定WSL命令入口、canonical path、宿主shell边界、build隔离与native Linux CI例外。
Code evidence: `AGENTS.md`与`docs/development.md`保存当前操作约束。
Disposition: documentation-only
Observable result: 仓库指令、开发文档与Ticket verification只给出WSL/Linux命令，WSL build目录与Windows native cache隔离，任何通过声明均可追溯到WSL或native Linux输出。

### R-113 — API freeze 只冻结 documented public contract 且发布 manifest 不可变
Status: active
Supersedes: None
Superseded by: None
Statement: v1.1起API freeze必须使用明确维护的documented public header/type/function/symbol allowlist与独立consumer compile contract，不得以整头文件哈希或全部`astra`动态符号替代语义分类；每个已发布版本manifest必须不可变，新版本只能新增独立manifest并与project/header/package版本一致。
Applies to: v1.1及后续source compatibility、release manifest与package gate。
Exceptions: 不承诺跨toolchain ABI。安装头中允许的非 documented 模板形状仅限 R-118 至 R-121 列出的结果格、F信封与薄awaiter，不得以“模板需要”为由保留完整运行协议类型。
Source decisions: D-169, D-170
Source support: D-169 => 区分documented contract与accidental implementation surface，并禁止改写旧发布证据; D-170 => 收紧模板可保留detail声明的例外，完整协议类型不得留在安装头。
Code evidence: None
Disposition: implementation
Observable result: 私有实现可在不改变public contract时演进，删除或改变documented API会使gate失败，v1.0 manifest保持字节不变。

### R-114 — Consumer 不可访问实现状态和测试控制入口
Status: active
Supersedes: None
Superseded by: None
Statement: `astra::detail`、`*_internal`、原始coroutine handle、Task shared-state mutator、FrozenGraph node storage、GraphRun completion storage与Scheduler fault-injection入口必须私有化或仅存在于非安装internal header；public headers不得授予普通consumer修改运行时不变量的能力。
Applies to: Scheduler、TaskHandle、Coroutine Task、FrozenTaskGraph、GraphRun和测试seam。
Exceptions: 编译失败型boundary probe可有意引用被禁止名称。最小friend仅得连接 R-118 至 R-121 允许的结果格/F信封/薄awaiter与库内协议实现，不得把协议类型完整定义留在安装头。
Source decisions: D-169, D-170
Source support: D-169 => 收回审查确认的 accidental control surface; D-170 => 禁止以friend/bridge为由在安装头保留完整协议类型。
Code evidence: None
Disposition: implementation
Observable result: public consumer对上述入口的编译探测失败，而已文档化用例继续编译运行。

### R-115 — Task identity 与 admission transaction 归 Runtime 所有
Status: active
Supersedes: None
Superseded by: None
Statement: TaskId必须由目标Scheduler Runtime状态分配，不得由public header中的进程静态对象生成；submit、try_submit、spawn、try_spawn与Graph node admission必须共享同一分配与回滚协议，失败不得泄漏admission slot、pending accounting、TaskId或已接纳工作。
Applies to: 全部Task与Graph admission路径。
Exceptions: R-100规定的进程期不复用与overflow语义保持不变。
Source decisions: D-169
Source support: D-169 => 将身份与事务所有权放回拥有admission状态的Runtime。
Code evidence: None
Disposition: implementation
Observable result: public header无TaskId全局分配器，全部admission失败注入保持计数平衡且TaskId不复用。

### R-116 — GraphExecution 深模块拥有图运行状态机
Status: active
Supersedes: None
Superseded by: None
Statement: Graph依赖计数、ready传播、edge policy、取消、terminal publication、completion callback与report snapshot必须由非安装的内部GraphExecution模块封装；Scheduler只提供窄的Task admission/publication、timer与observability桥接，不得直接读写GraphRunSharedState字段。
Applies to: FrozenTaskGraph执行和GraphRun控制。
Exceptions: 不改变R-069至R-072及R-077定义的observable graph semantics。
Source decisions: D-169
Source support: D-169 => 把约260行跨对象图协议收拢到拥有状态的深模块。
Code evidence: None
Disposition: implementation
Observable result: Scheduler graph入口委托GraphExecution，Graph state只能通过模块操作且现有图测试语义不变。

### R-117 — Public tests 与 internal seams 物理隔离
Status: active
Supersedes: None
Superseded by: None
Statement: public consumer tests不得获得`src/` include path、不得包含internal header或调用`detail`/`*_internal`/fault-injection入口；需要白盒控制的测试必须显式注册为internal test并仅通过非安装`src/test_seam.hpp`访问。
Applies to: tests CMake topology、package consumer与API boundary gates。
Exceptions: 编译失败型boundary test可在独立probe中有意引用被禁止的名称。
Source decisions: D-169
Source support: D-169 => 让测试拓扑执行public/internal边界而非依赖约定。
Code evidence: None
Disposition: implementation
Observable result: public tests在移除源码include后通过，误用internal入口的静态audit或compile probe失败。

### R-118 — 安装头不得提供可完成的运行协议类型
Status: active
Supersedes: None
Superseded by: None
Statement: package consumer翻译单元不得完成`TaskSharedStateBase`、`AwaitHandshake`或等价的TaskControlBlock/handshake协议类型；mutex、完成回调、rescheduler、timer注册、handshake状态机与invoker执行协议必须编入库实现，不得以完整类型出现在installed headers中。
Applies to: v1.2.0及后续installed public headers与独立consumer。
Exceptions: TaskHandle private nested结果格、F信封与公开awaitable薄包装由R-119至R-121约束，不属于本规则禁止的协议类型。
Source decisions: D-170
Source support: D-170 => compiled TaskControlBlock；consumer不能完成协议类型；协议编进库。
Code evidence: None
Disposition: implementation
Observable result: 独立consumer对上述协议类型的完成型compile probe失败，documented public用例继续编译运行。

### R-119 — TaskHandle 结果格为 private nested 且安装头不出现 TaskSharedState 名
Status: active
Supersedes: None
Superseded by: None
Statement: 安装头中的结果存储必须是`TaskHandle<T>`与`TaskHandle<void>`的private nested模板，只承载值`T`（void特化无值）、异常与终态观察所需状态；consumer不得将该nested类型作为独立入口命名或构造；安装头不得提供可被consumer完成的`TaskSharedState`或`TaskSharedStateBase`类型名。
Applies to: v1.2.0及后续TaskHandle安装头。
Exceptions: `get()`/`wait()`/`request_cancel`的可观察语义仍由R-048至R-058约束，本规则不改变那些结果。结果格不得包含mutex、回调列表、rescheduler、timer hook或handshake。
Source decisions: D-173, D-170
Source support: D-173 => private nested结果格且去掉TaskSharedState名称; D-170 => 任意T的get()仍可在consumer侧实例化且协议不回安装头。
Code evidence: None
Disposition: implementation
Observable result: public `get()`行为测试继续通过；对`TaskSharedState`/`TaskSharedStateBase`的完成型probe失败。

### R-120 — 公开 awaitable 以薄包装留在安装头且不暴露协议
Status: active
Supersedes: None
Superseded by: None
Statement: `co_await` TaskHandle（仅左值）、GraphRun（仅左值）、`yield()`、`sleep_for`/`sleep_until`与`cancellation_point`必须保持既有可观察挂起/恢复/取消语义；其awaiter可以完整类型出现在installed headers，但必须是薄包装：`await_suspend`只调用compiled协议实现，且类型及其成员不得暴露`AwaitHandshake`、TaskControlBlock/`TaskSharedState`、mutex/cv、rescheduler或timer registrar。documented compatibility surface是这些`co_await`操作，不承诺awaiter的名字、嵌套类型或布局。
Applies to: v1.2.0及后续Coroutine/TaskHandle/GraphRun公开awaitable。
Exceptions: 不把第三方自定义awaiter列为documented扩展面。
Source decisions: D-172, D-170
Source support: D-172 => 薄包装留在安装头、不暴露协议、不承诺类型名; D-170 => 挂起路径不得把handshake状态机展开在安装头。
Code evidence: None
Disposition: implementation
Observable result: 现有spawn/yield/sleep/await行为测试通过；对handshake/TCB完成型probe失败。

### R-121 — submit/emplace 安装头只保留 F 信封
Status: active
Supersedes: None
Superseded by: None
Statement: `submit`/`try_submit`/`spawn`/`try_spawn`与`TaskGraph::emplace`/`emplace_coroutine`在安装头中只得保留F信封：模板存储可调用对象`F`及是否接受`stop_token`的调用形态，`execute()`只调用`F`并把返回值交给结果格或compiled TaskControlBlock；admission、try_start、metrics、helping、timer与handshake不得出现在该信封的安装头实现中；不得在header缝上把`F`擦成`std::function`。
Applies to: v1.2.0及后续submit与Graph builder安装头。
Exceptions: 可调用对象约束仍由R-058与R-102约束（`f(args...)`或`f(stop_token, args...)`、不得返回裸引用、结果可移动、move-only F可提交）；Graph节点返回void仍由R-071约束。
Source decisions: D-174, D-170
Source support: D-174 => F信封、禁止std::function、禁止大invoker模板; D-170 => 协议不得留在安装头invoker实现中。
Code evidence: None
Disposition: implementation
Observable result: 现有submit/spawn/emplace与move-only callable测试通过；安装头invoker实现不含协议调用。

### R-122 — 协议内收后项目 VERSION 为 1.2.0 且既有 manifest 不可改写
Status: active
Supersedes: None
Superseded by: None
Statement: 实施R-118至R-121时`project(AstraScheduler VERSION …)`必须为1.2.0；consumer exact-version钉住值、installed `version.hpp`宏与package version必须同为1.2.0；`tools/api_manifest/v1.0.0.json`与`v1.2.0`之前的`v1.1.0.json`均不得改写；1.2.0必须使用新的semantic manifest。documented public observable semantics不因该版本上调而改变。
Applies to: v1.2.0 release、CMake package与API freeze gate。
Exceptions: 钉住1.1.0的find_package对1.2.0安装必须configure失败。不构成documented public的major break。
Source decisions: D-171
Source support: D-171 => VERSION升到1.2.0、不保持1.1.0、不升2.0.0、旧manifest不可改写、exact-version随单一版本源。
Code evidence: None
Disposition: implementation
Observable result: R-093一致性检查对1.2.0通过；改写v1.0.0/v1.1.0 manifest会使release/API gate失败。

### R-123 — 共享库默认隐藏并只导出 documented allowlist
Status: active
Supersedes: None
Superseded by: None
Statement: Linux shared `libAstraScheduler.so`必须hidden-by-default（version script或等价可见性控制），只导出与v1.2.0 semantic manifest/`public_contract.cpp`对齐的documented public符号；`astra::detail`协议类型、`record_metrics_*`、handshake与TaskControlBlock mutator不得出现在默认dynsym导出；package gate的封闭集必须等于该allowlist，不得再要求detail协议符号存在。header模板若仍引用被隐藏的协议符号，必须链接失败，不得把符号加回导出表。
Applies to: v1.2.0及后续shared install与R-110 package consumer。
Exceptions: 不承诺跨toolchain ABI（R-093）。static install无dynsym面，但仍受R-118至R-121的安装头约束。internal tests不得依赖已隐藏的public dynsym去测协议。
Source decisions: D-175, D-170
Source support: D-175 => version script只导出documented allowlist、封闭集不再要求detail; D-170 => 协议内收后不得靠导出绕过封装。
Code evidence: None
Disposition: implementation
Observable result: `nm -D --defined-only`不含协议detail符号；独立consumer仍能链接documented符号；残留header引用会导致链接失败。

## State and Lifecycle

| Scope | Transition / stage | Governing rules |
|---|---|---|
| Scheduler | startup transaction → `Running` | R-097, R-098, R-100, R-101 |
| Scheduler | `Running → Stopping + Graceful` | R-006, R-007, R-012, R-022, R-097, R-103 |
| Scheduler | `Graceful → Immediate` | R-009, R-014, R-015, R-034, R-075, R-106 |
| Scheduler | `Stopping → Join Ready → Stopped` | R-016, R-025, R-026 |
| Scheduler | `Stopped` absorption | R-019 |
| Ownership | Handle present → orphan Runtime State → Reaper | R-020 through R-024 |
| Reaper | RegistrationOpen / idle | R-028, R-107 |
| Process | `RegistrationOpen → Finalizing` | R-031, R-037, R-038, R-097 |
| Process | `Finalizing → CoordinatorExited` | R-032 through R-034, R-041, R-042 |
| Process | `CoordinatorExited → Finalized` | R-042 |
| Failure | Unrecoverable control-plane failure | R-047 |
| Task | admission → Waiting/Ready/Running/Suspended → Terminal | R-049, R-053, R-057, R-062 |
| Graph | builder → FrozenTaskGraph → GraphRun → GraphReport | R-069 through R-072 |
| Coroutine | cold frame → spawned → resume/suspend → Terminal/destroy | R-073 through R-079 |
| Trace | Stopped → Recording → explicit Snapshot or destructor abort | R-086 through R-088 |

## Variant Rules

| Variant | Summary | Rules |
|---|---|---|
| v0.1.0 | mutex Global Injection Queue correctness/benchmark baseline | R-001 through R-003 |
| v0.2 / v0.3 | locked Work-Stealing then portable Chase-Lev with capability reporting | R-063 through R-068, R-101 |
| Graceful | External admission closed; authorized Internal Submission forms Drain Work Closure | R-006, R-007, R-012 |
| Immediate | never-started cancellation、Running cooperative stop与already-started Coroutine resume | R-009, R-010, R-014, R-015, R-075, R-106 |
| Target Worker synchronous shutdown | no-side-effect rejection | R-011, R-013 |
| Non-Worker last Handle | synchronous RAII completion | R-103, R-105 |
| Target Worker last Handle | asynchronous preallocated Reaper handoff | R-020 through R-024 |
| Pending / Join Ready | liveness isolation followed by unique join | R-025, R-026 |
| begin / request_immediate | request-style commands available to any application thread | R-031, R-034, R-038 |
| wait / wait_for | non-Worker completion observation | R-032, R-033, R-039 through R-042 |
| Task result/cancel/wait | shared Outcome, cooperative cancel and caller-relative wait | R-048 through R-060 |
| External admission | bounded Reject/Block and strong transaction | R-061, R-062 |
| DAG | control graph with two edge policies and aggregate report | R-069 through R-072 |
| Coroutine/Timer | cold Task, source-Runtime resume and Worker timer heap | R-073 through R-079 |
| Priority/Deadline | immutable four-band hint and best-effort first-start EDF | R-080 through R-083 |
| Metrics/Trace | bounded opt-in observability and offline export | R-084 through R-088, R-095, R-096 |
| Logging | low-frequency control-plane diagnostics separated from Trace | R-109 |
| Benchmark/Release | verified corpus, artifacts, platform and SemVer policy | R-089 through R-094 |
| Package | isolated public headers and independent CMake consumer | R-110, R-123 |
| v1.2.0 installed surface | compiled TaskControlBlock; result cell, F envelope, thin awaiters only | R-118 through R-123 |

## Compatibility

- v0.1.0 的 Global Queue基线及后续纵向版本由 R-001 至 R-005、R-094 汇总。
- v1 documented source/observable semantic兼容、Linux-only支持范围和非ABI承诺由 R-093、R-110、R-111 汇总；本机开发入口由R-112约束。
- v1.2.0 安装面收紧与版本/导出由 R-118 至 R-123 汇总；v1.0.0 与 v1.1.0 manifest 仍不可变（R-113、R-122）。
- Finalization公共surface由 R-035、R-044至R-046汇总；startup竞态以R-097取代历史R-029/R-030。
- D-015/ADR-0006与D-025是历史方案；R-020至R-024和R-031至R-033分别表达替代设计。
- R-027由R-107取代，把process-wide单Reaper保证限定在R-111的Linux-only Supported Configuration及D-159的one-instance部署前提。
- R-017由R-103取代，R-018由R-105取代，以共享Scheduler的“最后Handle”作为RAII触发边界。
- Metrics、Trace与Benchmark schemas按R-084至R-096独立于项目SemVer演进。

## Failure Behaviour

| Failure / incomplete condition | Observable outcome | Rules |
|---|---|---|
| submission loses Graceful admission race | rejected without enqueue/outstanding increment | R-007 |
| Immediate reaches never-started task | Cancelled result becomes ready; Callable does not run | R-106 |
| Running task ignores stop | shutdown/finalization remains incomplete | R-009, R-010, R-032, R-034 |
| target Worker calls synchronous Scheduler shutdown | `std::logic_error` before side effects | R-011, R-013, R-108 |
| Reaper capability setup fails before start | startup failure with zero active Workers | R-023 |
| Finalization wait expires | TimedOut; same background completion continues | R-033, R-041 |
| any Scheduler Worker calls Finalization wait API | `std::logic_error` before side effects | R-039, R-040 |
| unrecoverable Reaper control-plane failure | best-effort diagnostic followed by `std::terminate()` | R-047 |
| capture/allocation before admission fails | original exception, Runtime reservations rolled back | R-062, R-102 |
| Helping depth exceeds configured bound | `helping_depth_exceeded` before next nested task starts | R-059 |
| Graph validation fails | `graph_validation_error` with stable reason/witness | R-069 |
| foreign awaitable ignores stop | Coroutine and shutdown/finalization may remain pending | R-075 |
| trace buffer fills | drop-newest plus loss metadata; scheduling unchanged | R-086, R-088 |
| ID or Node sequence exhausts | `overflow_error` before startup/admission/builder mutation commits | R-069, R-100 |
| Scheduler construction loses Finalization race | rollback and `FinalizationStarted` creation rejection | R-097 |

## Testing Decisions

- Version/API inventory evidence: R-001 through R-005, R-044 through R-046, R-093 through R-103 and R-110 through R-112；R-092仅保留历史superseded证据。
- Linearization and state-race tests: R-006 through R-016, R-019, R-037, R-041, R-049 through R-072 and R-097.
- Destructor, ownership and failure-injection tests: R-020 through R-024, R-062, R-073 through R-079, R-086 and R-103.
- Multi-Runtime liveness and unique-join stress tests: R-025 through R-028, R-042.
- Split-phase Finalization, concurrent controls and Worker caller tests: R-031 through R-043.
- Fatal failure behaviour and irreversible global-state tests use child-process isolation: R-043, R-047.
- Sanitizer and stress suites provide supporting evidence for ownership continuity, data-race freedom and no-use-after-free aspects of R-016, R-020 through R-027 and R-036 through R-042.
- Chase-Lev oracle/native weak-memory suites cover R-066 through R-068 and R-101.
- Metrics/Trace schema fixtures and loss tests cover R-084 through R-088, R-095 and R-096.
- Benchmark validity, artifact and dedicated-runner fixtures cover R-089 through R-094.
- Linux-only build/package/CI matrix覆盖R-111；所有本机验证命令与build-cache隔离检查覆盖R-112并必须从WSL执行。
- Semantic API manifest与consumer boundary probes覆盖R-113、R-114和R-117；admission fault injection覆盖R-115；Graph状态机/取消/报告测试覆盖R-116。
- v1.2.0协议类型完成型negative compile probes覆盖R-118至R-121；VERSION/manifest一致性覆盖R-122；shared dynsym allowlist覆盖R-123。

## Traceability

| Rule | Source | Observable verification | Planned ticket |
|---|---|---|---|
| R-001 | D-001 | v0.1 Ready Task path inventory | Pending |
| R-002 | D-001 | v0.1 build/API inventory excludes local/steal | Pending |
| R-003 | D-001 | comparative benchmark invokes preserved baseline | Pending |
| R-004 | D-004 | spec scope audit | Pending |
| R-005 | D-005 | ticket version-field audit | Pending |
| R-006 | D-002 | graceful internal-child drain test | Pending |
| R-007 | D-003 | submit/shutdown linearization stress test | Pending |
| R-008 | D-006 | pending cancellation/result wake test | Pending |
| R-009 | D-007 | cooperative/non-cooperative Callable tests | Pending |
| R-010 | D-008 | non-Worker immediate completion boundary | Pending |
| R-011 | D-009 | target Worker immediate no-side-effect failure | Pending |
| R-012 | D-010 | transitive graceful drain and unbounded task test | Pending |
| R-013 | D-011 | target Worker graceful no-side-effect failure | Pending |
| R-014 | D-012 | one-time mode-upgrade race test | Pending |
| R-015 | D-012 | admission/start/upgrade classification test | Pending |
| R-016 | D-013 | concurrent callers and unique Worker join stress | Pending |
| R-017 | D-014 | non-Worker destructor mode test | Pending |
| R-018 | D-014 | destructor completion/no-detach test | Pending |
| R-019 | D-016 | post-Stopped idempotency test | Pending |
| R-020 | D-017 | Runtime State lifetime sanitizer test | Pending |
| R-021 | D-017, D-018 | Worker last-Handle handoff stress test | Pending |
| R-022 | D-018 | handoff mode matrix test | Pending |
| R-023 | D-019 | pre-start failure injection | Pending |
| R-024 | D-019 | allocator/thread-creation failure handoff test | Pending |
| R-025 | D-020 | permanent Pending isolation test | Pending |
| R-026 | D-020 | Join Ready and unique join ordering test | Pending |
| R-027 | D-021 | coordinator topology and role test | Pending |
| R-028 | D-022 | idle no-spin/reuse test | Pending |
| R-029 | D-023 | start/finalization registration race test | Pending |
| R-030 | D-024 | accounted Runtime and sticky Starting test | Pending |
| R-031 | D-026 | begin return-boundary test | Pending |
| R-032 | D-027 | true Completion and unbounded wait test | Pending |
| R-033 | D-028 | timeout/background-continuation test | Pending |
| R-034 | D-029, D-039 | global escalation including orphan/Starting test | Pending |
| R-035 | D-030 | compile-time construction/interface test | Pending |
| R-036 | D-030, D-031 | copy/concurrency/destructor test | Pending |
| R-037 | D-032 | repeated/concurrent/empty begin test | Pending |
| R-038 | D-033 | Worker request-command test | Pending |
| R-039 | D-034 | multi-Scheduler Worker wait exception test | Pending |
| R-040 | D-035 | Worker wait_for positive/zero/negative test | Pending |
| R-041 | D-036 | controlled steady-clock boundary test | Pending |
| R-042 | D-037 | Exited-unjoined and single join-owner test | Pending |
| R-043 | D-038 | explicit teardown and unload-gate child-process test | Pending |
| R-044 | D-039 | public enum/type compile test | Pending |
| R-045 | D-039 | signature/noexcept/nodiscard compile test | Pending |
| R-046 | D-039 | public API inventory test | Pending |
| R-047 | D-040 | fatal fault-injection child-process test | Pending |
| R-048 | D-041, D-042, D-043, D-067, D-153 | shared Handle identity/lifetime test | Pending |
| R-049 | D-044, D-071 | terminal publication consistency test | Pending |
| R-050 | D-045, D-057 | repeated exception/cancel propagation test | Pending |
| R-051 | D-076 | compile-time lvalue get and lifetime test | Pending |
| R-052 | D-047–D-051 | caller-relative Helping/self-cycle test | Pending |
| R-053 | D-052–D-055 | cancel/start linearization stress | Pending |
| R-054 | D-056, D-058–D-060 | token selection and Outcome matrix | Pending |
| R-055 | D-061, D-062, D-073 | wait multi-observer test | Pending |
| R-056 | D-063–D-066 | controlled timeout/Helping test | Pending |
| R-057 | D-067–D-073, D-153 | empty/state/concurrency API test | Pending |
| R-058 | D-074–D-077 | compile-time result type matrix | Pending |
| R-059 | D-078–D-080 | Helping depth and shutdown eligibility test | Pending |
| R-060 | D-081, D-082, D-120, D-151 | observability enablement matrix | Pending |
| R-061 | D-083–D-086 | backpressure/gate wake stress | Pending |
| R-062 | D-087–D-089, D-155 | admission fault-injection matrix | Pending |
| R-063 | D-090–D-092, D-147 | routing and Global fairness trace | Pending |
| R-064 | D-093 | bounded deterministic steal probes | Pending |
| R-065 | D-094–D-096 | park publication race stress | Pending |
| R-066 | D-097, D-098 | seq_cst oracle/portable litmus | Pending |
| R-067 | D-099, D-100 | resize/reclamation/fallback stress | Pending |
| R-068 | D-101–D-103 | index/retry/arithmetic boundary tests | Pending |
| R-069 | D-104, D-105, D-161 | builder/validation/NodeId tests | Pending |
| R-070 | D-106, D-107 | atomic graph admission/countdown stress | Pending |
| R-071 | D-108–D-110 | edge-policy propagation matrix | Pending |
| R-072 | D-111–D-113, D-152 | cancellation/report/wait tests | Pending |
| R-073 | D-114, D-115 | cold frame/spawn rollback tests | Pending |
| R-074 | D-116–D-118 | resume handshake/destroy stress | Pending |
| R-075 | D-119, D-154 | suspended cancellation/Immediate test | Pending |
| R-076 | D-120–D-122, D-147 | await routing/self rejection tests | Pending |
| R-077 | D-123, D-124 | Graph Coroutine identity compile/runtime test | Pending |
| R-078 | D-125 | public API inventory test | Pending |
| R-079 | D-126–D-128 | timer ordering/cancel/drain test | Pending |
| R-080 | D-129 | Priority inheritance/override matrix | Pending |
| R-081 | D-130, D-131 | weighted fairness/non-preemption benchmark | Pending |
| R-082 | D-132 | deadline factory/semantics tests | Pending |
| R-083 | D-133, D-134, D-147 | EDF/order/miss tests | Pending |
| R-084 | D-135, D-136, D-151 | metrics level/schema/saturation tests | Pending |
| R-085 | D-137 | fuzzy/quiescent snapshot tests | Pending |
| R-086 | D-138, D-158, D-163 | capture/loss/RAII generation tests | Pending |
| R-087 | D-139, D-153 | event schema/identity golden tests | Pending |
| R-088 | D-140 | deterministic Chrome export golden | Pending |
| R-089 | D-141 | benchmark phase/invalid sample tests | Pending |
| R-090 | D-142, D-150 | corpus/adapter inventory tests | Pending |
| R-091 | D-143 | artifact schema/regression policy tests | Pending |
| R-092 | D-167, D-159 | historical rule superseded by R-111 | Superseded |
| R-093 | D-145, D-164 | SemVer/header-library version tests | Pending |
| R-094 | D-146 | release gate audit | Pending |
| R-095 | D-148 | process metrics lifecycle tests | Pending |
| R-096 | D-050, D-051, D-149 | wait/await observability fixtures | Pending |
| R-097 | D-023, D-024, D-155, D-156 | startup/finalization race test | Pending |
| R-098 | D-078, D-157 | options default/validation tests | Pending |
| R-099 | D-160 | legal status pair concurrency test | Pending |
| R-100 | D-153, D-161 | logical ID/overflow/type tests | Pending |
| R-101 | D-101, D-167, D-162 | backend capability matrix on supported Linux | Pending |
| R-102 | D-059, D-165 | move-only capture/invocation tests | Pending |
| R-103 | D-014, D-017, D-018, D-155 | shared last-Handle RAII test | Pending |
| R-104 | D-024, D-156 | accounted startup Graceful race test | Pending |
| R-105 | D-014, D-155 | last non-Worker destructor completion test | Pending |
| R-106 | D-006, D-154 | Immediate never-started/suspended classification test | Pending |
| R-107 | D-021, D-159 | one-instance coordinator topology test | Pending |
| R-108 | D-009, D-011, D-166 | self/other Worker shutdown caller matrix | Pending |
| R-109 | D-140 | logger/trace hot-path isolation test | Pending |
| R-110 | D-167, D-145 | Linux install/find_package static/shared smoke | Pending |
| R-111 | D-167 | Linux-only release/package/CI matrix audit | AST-052 |
| R-112 | D-168 | repository instruction and WSL verification audit | AST-001 |
| R-113 | D-169 | semantic public API manifest and immutable release evidence | AST-057 |
| R-114 | D-169 | public compile boundary probes | AST-057 |
| R-115 | D-169 | TaskId ownership and admission rollback tests | AST-057 |
| R-116 | D-169 | Graph execution state/cancel/report tests | AST-057 |
| R-117 | D-169 | public/internal test topology audit | AST-057 |
| R-118 | D-170 | protocol-type completeness negative compile probes | Pending |
| R-119 | D-173, D-170 | TaskHandle get/wait tests plus TaskSharedState completeness probes | Pending |
| R-120 | D-172, D-170 | yield/sleep/await behavior tests plus handshake completeness probes | Pending |
| R-121 | D-174, D-170 | submit/spawn/emplace and move-only callable tests | Pending |
| R-122 | D-171 | R-093 version contract and immutable v1.0.0/v1.1.0 manifests | Pending |
| R-123 | D-175, D-170 | shared-library documented export allowlist | Pending |

## Open Questions

None。已确认范围内没有未决语义；明确排除项保留在 Non-goals，不构成 draft blocker。

## Further Notes

- Authoritative rationale、拒绝方案与后果保留在 `decision-log.md` 和 ADR-0001 至 ADR-0048。
- 总设计是架构参考；冲突处以accepted decision和对应R-rule为准。
- R-008由R-106取代，R-017由R-103取代、R-018由R-105取代，R-027由R-107取代、R-029由R-097取代、R-030由R-104取代；其余早期R-ID语义保持。
- 本次Linux-only修订以R-111取代R-092，并新增R-112固定WSL开发入口；R-101与R-110仅替换为当前accepted来源D-167，行为边界保持。
- 本次Linux-only/WSL修订已由项目owner于2026-08-27批准；通过非draft校验后同步修订已发布Tickets。
- 本次v1.1封装性修订由项目owner于2026-08-30批准；只收回未文档化的实现表面，不改变既有observable semantics。
- 本次v1.2.0 compiled TaskControlBlock修订（D-170至D-175，R-118至R-123，ADR-0048）已由项目owner于2026-08-30批准；不深化GraphExecution或拆Scheduler::Impl（R-116仍active）。
