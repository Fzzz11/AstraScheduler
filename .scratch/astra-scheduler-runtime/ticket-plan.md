# AstraScheduler v0.1 → v1.0 Ticket Plan

Status: approved
Approved by: project owner（user）
Approved at: 2026-08-27
Target tracker: local Markdown (`.scratch/astra-scheduler-runtime/issues/`)
Source spec: `.scratch/astra-scheduler-runtime/spec.md` (`approved`, Linux-only/WSL revision approved 2026-08-27)
Generated at: 2026-08-26
Revised at: 2026-08-27 (D-167, D-168, R-111, R-112)

## Input Audit

- Approved Spec 包含 112 个规则编号，其中 105 条为 active，7 条已 superseded。
- Active 规则中 101 条为 `implementation`，4 条为 `documentation-only`。
- `Open Questions` 为 `None`；本计划不从聊天记录或参考设计新增规范性行为。
- Phase 0 是 v0.1.0 的构建、安装、测试与版本契约前置，不单独形成发布版本。
- Linux-only/WSL修订以R-111取代R-092，并新增R-112；Ticket数量、编号与blocker DAG不变。

## Splitting Principles

1. 每个 Ticket 是一个可在新上下文中完成的纵向切片，必须先写失败测试，再做最小实现。
2. 每条 active 规则只有一个 Primary Ticket；集成、移植和 release Ticket 只能以 Supporting 角色重复引用。
3. 私有 seam 可以提前建立，未批准的 public API、配置项或语义不得提前暴露。
4. 每个里程碑必须保持可构建、可运行；后续调度后端不能删除 v0.1 Global-only 语义基线和 benchmark baseline。
5. Blockers 表示必须先具备的实现事实，不把同一里程碑内可并行的工作无意义串行化。

## Milestone Release Gates

| Milestone | Required release evidence |
|---|---|
| Phase 0 | WSL开发门禁、编译库骨架、安装导出 target、独立Linux consumer、版本查询、测试入口可运行。 |
| v0.1.0 | Global-only Runtime、TaskHandle/Result、取消、完整 Scheduler/Reaper/Finalization 生命周期；Local Deque capability 为 `None`。 |
| v0.2.0 | Locked Local Deque、bounded steal round、Park Handshake；v0.1 baseline 仍可运行。 |
| v0.3.0 | Chase-Lev portable ordering、resize retention、index 边界证据；实际 capability 才报告 lock-free。 |
| v0.4.0 | 单次 Frozen Graph、原子 admission、edge policy、GraphRun 完整报告。 |
| v0.5.0 | Cold Task、runtime-owned resume、取消握手、Graph coroutine node、Worker timer。 |
| v0.6.0 | 固定 Priority、8:4:2:1 band service、best-effort first-start Deadline、Global indexed EDF。 |
| v0.7.0 | Runtime/Process Metrics、bounded Trace、versioned events、Chrome Trace 离线导出与 wait/await 诊断。 |
| v0.8.0 | Micro harness、scenario runner、语义基线、原始 artifact 与受限 regression gate。 |
| v0.9.0 | Linux-only Tier matrix、Linux sanitizer/native AArch64 weak-memory、package consumer、单实现实例部署约束全部 harden。 |
| v1.0.0 | Public source/semantic compatibility 冻结，全部 approved-rule、文档、package、schema、benchmark gates 通过。 |

## Planned Tickets

### Phase 0 — Engineering Scaffold

#### AST-001 — 建立里程碑交付矩阵与规则门禁

- Primary Rules: R-004, R-005, R-094, R-112
- Blockers: None
- What to build: 在仓库文档和CI配置入口中固定Phase 0至v1.0的里程碑、每tag DoD、规则追踪入口、“approved Spec才可实现”门禁，以及Linux-only/WSL本机命令与cache隔离门禁。
- Test-first seam: 先增加会因缺少里程碑、规则引用、WSL入口、cache隔离或release gate而失败的文档/配置校验测试。

#### AST-002 — 建立 compiled library 与可安装 CMake package

- Primary Rules: R-110
- Supporting Rules: R-111
- Blockers: AST-001
- What to build: 在WSL/Linux中创建C++20 compiled library、隐藏实现目录、安装并导出`AstraScheduler::AstraScheduler`，加入仓库外独立Linux consumer测试。
- Test-first seam: 先在WSL写独立Linux consumer的configure/build测试，证明未安装、泄漏私有include或错误声明非Linux支持时失败。

#### AST-003 — 提供 header/library 版本查询与 mismatch 诊断

- Primary Rules: R-093
- Blockers: AST-002
- What to build: 实现 SemVer header 宏、`header_version()`、无副作用 `library_version()`/`library_version_string()`，定义 header/library mismatch 的可诊断行为。
- Test-first seam: 先写版本相等、查询不启动 Runtime/Reaper、模拟 mismatch 可被发现的 consumer 测试。

### v0.1.0 — Global Runtime, Task and Lifecycle

#### AST-004 — 固定 Scheduler 公共 policy、状态、逻辑 ID 与 capability

- Primary Rules: R-098, R-099, R-100, R-101
- Blockers: AST-002, AST-003
- What to build: 定义稳定 `SchedulerOptions`、冻结后的 resolved snapshot、成对 status snapshot、强类型不复用逻辑 ID，以及真实 Local Deque backend capability；v0.1 报告 `None`。
- Test-first seam: 先写 public compile tests、Options 修改不回写 Runtime、状态成对观察、ID 不混用及 v0.1 capability 测试。

#### AST-005 — 实现 Scheduler startup transaction 与 Finalization gate 排序

- Primary Rules: R-023, R-024, R-097
- Blockers: AST-004
- What to build: 将配置验证、Reaper registration/handoff 预留、Worker 创建和 Running 发布组织成同步强事务；与永久 registration close 建立唯一线性化顺序。
- Test-first seam: 用可注入失败点先覆盖每个 startup 阶段回滚、无公开 Starting 状态、close/start 竞态仅有两个合法结果。

#### AST-006 — 解耦 Runtime State 并实现最后 Worker Handle handoff

- Primary Rules: R-020, R-021, R-022
- Blockers: AST-005
- What to build: 用共享 Runtime State 承载执行身份；最后 Handle 在目标 Worker 上释放时只做预留好的 orphan handoff，并保留 Graceful 默认策略后立即返回。
- Test-first seam: 先写 Worker 内释放最后 Handle 的确定性测试，验证无 self-join、无悬空访问、handoff 后任务仍可完成。

#### AST-007 — 实现唯一 Reaper coordinator 的 pending/join/idle 循环

- Primary Rules: R-025, R-026, R-028, R-107
- Blockers: AST-006
- What to build: 建立单 implementation instance 下的唯一 coordinator；Pending Runtime 不阻塞其他回收，只在 Join Ready 后唯一 join、发布 Stopped，空闲不重启服务。
- Test-first seam: 用两个受控 Runtime 证明一个长期 Pending 不阻塞另一个 Join Ready，且并发 handoff 只有一次 join/coordinator。

#### AST-008 — 交付 Global-only Worker Runtime 基线

- Primary Rules: R-001, R-002
- Blockers: AST-004, AST-005
- What to build: 实现 mutex 保护的 Global Injection Queue、固定 Worker 集合和基本执行循环；所有 Ready Task 只走 Global 路径。
- Test-first seam: 先用可观察 queue seam 证明 external/internal/worker-published Ready 都进入 Global，且构建中没有 local push/pop/steal 路径。

#### AST-009 — 实现 move-only submit 与共享 TaskHandle 基础面

- Primary Rules: R-048, R-058, R-102
- Blockers: AST-008
- What to build: `submit` decay-own Callable/args 并以 stored rvalue 恰好调用一次，支持 move-only target/arg，返回可复制的同一 Task identity capability 和受限结果 API。
- Test-first seam: 先写 `operator()&&`、move-only 参数、`std::ref`、Handle copy/empty 及非法返回形态的编译与运行测试。

#### AST-010 — 实现 External Pending Capacity 与强 admission transaction

- Primary Rules: R-061, R-062
- Blockers: AST-009
- What to build: 为 External Submission 实现 pending 配额、`submit` backpressure 和 `try_submit` 非阻塞失败；二者共享一次性强事务并正确回滚资源与配额。
- Test-first seam: 注入容量耗尽、分配失败、close 竞态，先验证失败不留下 Task identity、调度引用或容量泄漏。

#### AST-011 — 发布一致的 TaskState、Terminal Outcome 与重复 get

- Primary Rules: R-049, R-050, R-051, R-057
- Supporting Rules: R-060
- Blockers: AST-009
- What to build: 实现公开生命周期投影、Value/Exception/Cancelled 单次发布、左值 Handle 持有的结果引用和可重复异常/取消传播。
- Test-first seam: 先写 value/void/reference、异常、取消、空 Handle、并发 observer 与重复 `get()` 的状态/结果一致性测试。

#### AST-012 — 实现 Unbounded/Helping wait 与 timeout 边界

- Primary Rules: R-052, R-055, R-056, R-059
- Blockers: AST-008, AST-011
- What to build: 非 Worker 使用无界同步等待；同 Runtime Worker 通过正常调度路径 Helping，受 depth/Shutdown eligibility 限制；`wait_for` 超时不伪造完成。
- Test-first seam: 构造 single-worker nested wait、direct self-wait、depth overflow、shutdown 中 helping 和 timeout/complete 边界竞态。

#### AST-013 — 实现显式 Task cancellation 的首次 start 分类

- Primary Rules: R-053, R-054
- Blockers: AST-010, AST-011
- What to build: `request_cancel()` 与首次 start 线性化竞争；未开始任务发布 Cancelled，已开始任务只收到 cooperative stop，最终 Outcome 由真实退出决定。
- Test-first seam: 用 barrier 穷举 cancel-before-start、start-wins、忽略 stop、`task_cancelled` 退出和普通异常路径。

#### AST-014 — 实现 Graceful admission closure 与 Drain Work Closure

- Primary Rules: R-006, R-007, R-012, R-019
- Blockers: AST-010, AST-011
- What to build: 线性化关闭 External Submission，继续接受获授权 Internal Submission，排空传递闭包并把 Stopped 作为吸收状态。
- Test-first seam: 先写关停边界前后 external/internal 提交、递归 internal fan-out、重复 shutdown 和 Stopped 后操作测试。

#### AST-015 — 实现 shutdown caller guard 与共享完成边界

- Primary Rules: R-010, R-011, R-013, R-016, R-108
- Blockers: AST-007, AST-014
- What to build: 非 Worker shutdown 同步等待同一次完成；同 Runtime Worker 调用两种 shutdown 均无副作用抛 `logic_error`；并发调用共享 join/Stopped 发布。
- Test-first seam: 先写 Worker/非 Worker、并发 graceful/now caller、异常前后状态和唯一 join 的确定性测试。

#### AST-016 — 实现单向 Immediate escalation 与启动状态分类

- Primary Rules: R-009, R-014, R-015, R-106
- Blockers: AST-013, AST-015
- What to build: 仅允许 Graceful→Immediate；关闭 internal admission，直接取消从未首次 start 的任务，对 Running/Suspended 已开始工作只请求协作停止并等待回收。
- Test-first seam: 用 Waiting/Ready/Running/Suspended 分类夹具验证升级幂等、Outcome 和“不强杀已开始任务”。

#### AST-017 — 实现最后非 Worker Handle 的 noexcept 同步 RAII

- Primary Rules: R-103, R-105
- Blockers: AST-007, AST-014, AST-015
- What to build: 仅最后一个非 Worker Scheduler Handle 释放触发 Graceful fallback，并作为 `noexcept` 同步完成/回收边界；非最后副本释放不关停。
- Test-first seam: 先覆盖多 Handle 释放顺序、最后释放阻塞到真实完成、异常任务和析构路径不传播异常。

#### AST-018 — 固定 FinalizationControl 公共 capability surface

- Primary Rules: R-035, R-036, R-043, R-044, R-045, R-046
- Blockers: AST-004, AST-007
- What to build: 定义有效共享控制对象、固定结果枚举与四个操作；析构无策略，不暴露 reset/restart/同义 shutdown，且只能由应用显式编排。
- Test-first seam: 先写 public compile tests、invalid/default 状态、复制共享、析构无动作和被禁止接口的负向编译测试。

#### AST-019 — 实现 begin_finalization、核算集合与 startup 竞态

- Primary Rules: R-031, R-037, R-038, R-104
- Blockers: AST-005, AST-018
- What to build: `begin_finalization()` 幂等返回唯一世代，永久关闭注册，立即返回；对已注册和赢得 startup 核算竞态的 Runtime 请求 Graceful。
- Test-first seam: 先写多线程 begin、begin/startup 线性化、已核算 Runtime、调用线程身份及“begin 不等待完成”测试。

#### AST-020 — 实现 Finalization 无界 wait、wait_for 与唯一 coordinator join

- Primary Rules: R-032, R-033, R-039, R-040, R-041, R-042
- Blockers: AST-007, AST-019
- What to build: `wait()` 仅在真实 Finalization Completion 后返回；`wait_for` 使用 steady clock，超时不改变策略；Worker caller 抛错；合法等待者共享唯一 join。
- Test-first seam: 先写超时后继续推进、timeout/completion 同边界顺序、多 waiter、任意 Scheduler Worker 拒绝和唯一 coordinator join。

#### AST-021 — 实现 Finalization escalation 与控制面 fail-fast

- Primary Rules: R-034, R-047
- Blockers: AST-016, AST-019, AST-020
- What to build: `request_immediate()` 单向覆盖全部已核算未完成 Runtime，且不伪造完成；Reaper 控制面不可恢复故障进入定义好的 fail-fast 路径。
- Test-first seam: 先用可控 Runtime 集合验证升级覆盖、幂等、升级后继续 wait，以及 fault-injection 下的终止 handler 证据。

### v0.2.0 — Locked Work Stealing

#### AST-022 — 加入 Locked Local Deque 与 Ready Routing Precedence

- Primary Rules: R-063
- Supporting Rules: R-101
- Blockers: AST-008, AST-010, AST-012
- What to build: 为每个 Worker 增加 Locked Local Deque，按专用 destination→owner Local/off-worker Global 的 precedence 路由，并保持 Global source 不饥饿。
- Test-first seam: 先写 publisher 身份、awaiter/deadline 占位 destination、local/global source 顺序和 capability=`Locked` 测试。

#### AST-023 — 实现 bounded non-repeating Steal Round

- Primary Rules: R-064
- Blockers: AST-022
- What to build: 空闲 Worker 每轮只探测有界且不重复 victim，成功窃取后返回正常执行路径，并暴露确定性 victim selector seam。
- Test-first seam: 先写固定种子 victim 序列、0/1/N Worker、轮内不重复和轮界 backoff 测试。

#### AST-024 — 实现无丢唤醒 Park Handshake

- Primary Rules: R-065
- Blockers: AST-015, AST-022, AST-023
- What to build: 在休眠前登记意图并二次检查 publication generation、Ready sources 与退出条件；work/control publication 正确唤醒。
- Test-first seam: 用 park 前/后 barrier 穷举 publish、shutdown、spurious wake 和多 Worker 唤醒竞争。

### v0.3.0 — Chase-Lev Deque

#### AST-025 — 建立 Chase-Lev seq_cst oracle 与 portable memory order

- Primary Rules: R-066
- Blockers: AST-022, AST-023
- What to build: 先实现可比对的 seq_cst oracle，再实现固定 acquire/release/fence/CAS ordering 的 owner/thief 算法。
- Test-first seam: 先写 oracle differential、last-item race、owner pop 与多 thief steal 的 stress/TSAN 测试。

#### AST-026 — 实现 Chase-Lev growth、旧 buffer retention 与单一调度引用

- Primary Rules: R-067
- Blockers: AST-025
- What to build: 扩容复制物理 cell 但不复制 Scheduling Reference；旧 buffer 保留到 Worker teardown，claim/cleanup 仍恰好一次。
- Test-first seam: 用极小初始容量强制连续 resize，验证任务不丢失、不重复执行/销毁并在 teardown 才释放旧 buffer。

#### AST-027 — 固定 Chase-Lev index 算术、边界状态与 backend truth

- Primary Rules: R-068
- Supporting Rules: R-101
- Blockers: AST-004, AST-025, AST-026
- What to build: 使用不依赖整数 wrap 的索引/差值约束与受测 rebase/boundary 策略；仅真实启用实现时报告 `ChaseLevLockFree`。
- Test-first seam: 通过小位宽/偏置 seam 加速边界，覆盖 empty/one/full、rebase 并验证 capability 不按版本虚报。

### v0.4.0 — Task Graph

#### AST-028 — 实现 consuming TaskGraph freeze 与 NodeId 验证

- Primary Rules: R-069
- Blockers: AST-004, AST-009
- What to build: 构建 mutable builder→validated move-only Frozen Graph 的 consuming freeze；按插入顺序分配强类型 NodeId，拒绝坏边与 cycle。
- Test-first seam: 先写空图、重复/越界 edge、cycle、move-only node、freeze 后不可变和二次消费失败测试。

#### AST-029 — 实现 GraphRun 原子 admission 与依赖发布

- Primary Rules: R-070
- Blockers: AST-010, AST-022, AST-028
- What to build: 一次性核算全部 Node 的资源/容量，失败全回滚；成功后 roots Ready，依赖只由 predecessor Terminal publication 推进。
- Test-first seam: 注入第 N 个 Node admission 失败，验证无部分可见 GraphRun；覆盖多 predecessor 最后完成竞态。

#### AST-030 — 实现 void 控制图与两类 Edge policy

- Primary Rules: R-071
- Blockers: AST-029
- What to build: 固定 DAG node 为 void 控制任务；实现 required-success 与 completion-only edge，对失败/取消仅传播到 required descendants。
- Test-first seam: 用菱形、多父节点、混合 edge、异常与取消矩阵验证允许执行和自动取消集合。

#### AST-031 — 实现 GraphRun cancel、完整报告与 caller-relative wait

- Primary Rules: R-072
- Blockers: AST-012, AST-013, AST-030
- What to build: GraphRun 提供显式取消、所有 Node Terminal 才完成的稳定 report，以及非 Worker/Helping wait 与 timeout。
- Test-first seam: 先写部分运行时 cancel、已终态 node 保持、完整 report 顺序、single-worker wait 和 timeout 不伪造完成。

### v0.5.0 — Coroutine and Timer

#### AST-032 — 实现 cold Coroutine Task 与 spawn 强保证

- Primary Rules: R-073
- Blockers: AST-009, AST-013
- What to build: 定义 cold `Task<T>`；`spawn` 成功才把 frame/Task identity 移交 Runtime，失败保持调用方可安全销毁且不部分发布。
- Test-first seam: 先写 cold-before-spawn、move-only frame、admission failure、exactly-once frame destruction 和 result propagation。

#### AST-033 — 实现唯一 resume ownership 与 await handshake

- Primary Rules: R-074
- Blockers: AST-024, AST-032
- What to build: 每次 suspension 只允许一个恢复所有者，通过 armed/triggered/claimed 握手消除完成与挂起竞态，禁止 inline foreign resume。
- Test-first seam: 穷举 completion-before-arm、arm-before-completion、cancel/complete race 和 exactly-once resume/destroy。

#### AST-034 — 实现 Suspended cancellation 与 Immediate cooperative resume

- Primary Rules: R-075
- Blockers: AST-016, AST-033
- What to build: 已开始 Suspended frame 收到 cancel/Immediate 时只通过 source Runtime 安排恢复以观察 stop；未开始 coroutine 仍可直接取消且不执行用户代码。
- Test-first seam: 覆盖 suspended timer/await、cancel/trigger race、Immediate、忽略 stop 和 `task_cancelled` 退出。

#### AST-035 — 实现 source-Runtime await 与受限组合 API

- Primary Rules: R-076, R-078
- Blockers: AST-012, AST-033
- What to build: TaskHandle/GraphRun await completion 只向 source Runtime 发布 continuation；提供已批准的 blocking/async 组合面，不新增隐式 inline 或多套同义 API。
- Test-first seam: 先写 same/cross-runtime await、目标先完成、source shutdown eligibility 和负向 public compile tests。

#### AST-036 — 将 Coroutine Graph Node 绑定同一 Node Task identity

- Primary Rules: R-077
- Blockers: AST-028, AST-029, AST-032, AST-035
- What to build: Graph coroutine node 从首次运行到多次 resume 复用同一 GraphRunId+NodeId+TaskId，Terminal publication 只发生一次。
- Test-first seam: 先写多 suspension node、异常/取消、dependent release 和逻辑 ID 稳定性测试。

#### AST-037 — 实现 Worker-driven timer heap 与 sleep eligibility

- Primary Rules: R-079
- Blockers: AST-024, AST-033, AST-034
- What to build: 每 Runtime 使用 Worker 驱动的 steady-clock timer 结构；Wake Time 只是 Ready eligibility 下界，到期后按 ordinary Global resume 路由。
- Test-first seam: 使用 fake clock 测试早醒禁止、同 deadline、多 timer cancel、shutdown 和到期后调度延迟不构成语义失败。

### v0.6.0 — Priority and Deadline

#### AST-038 — 在 admission 解析并冻结 Base Priority

- Primary Rules: R-080
- Blockers: AST-010, AST-022
- What to build: 扩展稳定 TaskOptions，在 admission 解析 Low/Normal/High/Critical 并固定到 Task identity；默认值与继承规则可测试。
- Test-first seam: 先写 external/internal/coroutine/graph admission 的解析矩阵和调用方后续修改 Options 不生效测试。

#### AST-039 — 实现每 Ready source 的 8:4:2:1 band service

- Primary Rules: R-081
- Blockers: AST-027, AST-038
- What to build: Global/Local/steal 各 source 使用固定加权日历产生 service opportunity，保持非抢占且避免低 band 永久饥饿。
- Test-first seam: 用 deterministic calendar 验证长期机会比例、空 band 跳过、持续 Critical 负载和正在运行任务不被抢占。

#### AST-040 — 固定 TaskDeadline 的 first-start 语义

- Primary Rules: R-082
- Blockers: AST-038
- What to build: 在 TaskOptions 接受显式 steady-clock 绝对 Deadline；只比较首次成功进入 Running，miss 仅记录事实，不等待/取消/提升优先级。
- Test-first seam: 用 fake clock 覆盖 on-time/late、retry/resume、无 deadline 和 miss 不改变 Outcome/Priority。

#### AST-041 — 实现 Priority 主导的 Global indexed EDF

- Primary Rules: R-083
- Blockers: AST-027, AST-039, AST-040
- What to build: 首次 deadline work 进入按 Priority 分区的 Global indexed EDF，支持取消/claim 删除；Priority 先于 deadline，非 deadline work 保持公平来源。
- Test-first seam: 覆盖同 band EDF、跨 band priority、相同 deadline tie、取消删除、steal/local 与 deadline destination precedence。

### v0.7.0 — Observability

#### AST-042 — 实现 Runtime Metrics level 与 Basic event schema

- Primary Rules: R-084
- Blockers: AST-004, AST-010, AST-015
- What to build: 实现 Off/Basic/Detailed 冻结配置、分片饱和 counters 和固定 Basic 状态/队列/调度/失败事件 schema，Off 热路径近零开销。
- Test-first seam: 先写 Off 无更新、Basic counter 守恒、饱和标记和 quiescent point 精确断言。

#### AST-043 — 实现 fuzzy Metrics Snapshot 与 Detailed log2 histogram

- Primary Rules: R-085
- Blockers: AST-042
- What to build: 聚合逐字段安全的不可变 snapshot，标注 capture 区间/fuzzy/saturated；Detailed 使用固定 log2 histogram 记录指定延迟。
- Test-first seam: 先写并发 snapshot 合法区间、quiescent 精确值、bucket 边界、overflow saturation 和无 reset 行为。

#### AST-044 — 实现 side-effect-free Process Metrics

- Primary Rules: R-095
- Blockers: AST-007, AST-020, AST-021, AST-043
- What to build: 提供固定 Reaper/Finalization counters、gauges、状态与时长；查询不初始化服务，Finalized 后保留终值，不聚合 Runtime task metrics。
- Test-first seam: 先写 Scheduler 创建前零值/无线程、handoff/join、timeout/escalation、Finalized 稳定终值和 fuzzy 字段安全。

#### AST-045 — 实现 bounded reusable TraceCollector

- Primary Rules: R-086
- Blockers: AST-004, AST-042
- What to build: 显式 attach/capture/stop/submit 的共享 collector，固定容量、重复代际、drop 计数，Runtime 热路径不做文件 I/O。
- Test-first seam: 先写 disabled fast path、容量溢出、重复 capture、并发 producer、collector 生命周期不拥有 Runtime。

#### AST-046 — 固定 versioned TraceEvent 与逻辑 ID 关联

- Primary Rules: R-087
- Blockers: AST-004, AST-045
- What to build: 定义固定记录布局、schema version、category、steady timestamp 和 RuntimeId/TaskId/GraphRunId/NodeId 关联，不写对象地址作 identity。
- Test-first seam: 先写 layout/version golden test、跨 Runtime identity、Graph coroutine identity 和 event decode round-trip。

#### AST-047 — 实现确定性 Chrome Trace 导出并隔离 Logging

- Primary Rules: R-088, R-109
- Blockers: AST-045, AST-046
- What to build: capture 后离线稳定导出 Chrome Trace，显式报告 drop/schema loss；Logging 不复用 TraceCollector，默认不记录每 Task 热路径。
- Test-first seam: 先写 deterministic JSON golden、loss metadata、损坏 event 诊断、无后台 writer 和 logging-disabled 热路径测试。

#### AST-048 — 接入 wait/await 与 unobserved failure 诊断

- Primary Rules: R-060, R-096
- Blockers: AST-012, AST-031, AST-035, AST-043, AST-046
- What to build: 记录 task/graph waits、timeouts、Helping、await 与拒绝的 metrics/trace；未观察异常只在已启用观测面诊断，不维护在线 wait-for graph、不改执行语义。
- Test-first seam: 先写 trace 离线重建 same/cross-runtime edge、Detailed duration bucket、Off 无诊断和 unobserved exception 不终止进程。

### v0.8.0 — Benchmark Framework

#### AST-049 — 建立 micro harness 与独立 scenario runner

- Primary Rules: R-089
- Blockers: AST-001, AST-002
- What to build: 分离 deque/queue/admission microbench 与端到端 Runtime scenario runner；每 case 固定 timed region、参数、checksum 和 primary metric。
- Test-first seam: 先写 harness self-test，验证错误 checksum、空 repetition、异常 case 和 timed-region 污染会标记 invalid。

#### AST-050 — 固定 benchmark corpus 并保存 Global baseline

- Primary Rules: R-003, R-090
- Blockers: AST-008, AST-023, AST-027, AST-031, AST-037, AST-041, AST-049
- What to build: 建立 micro/CPU/imbalanced/fork-join/DAG/coroutine/timer/priority/deadline corpus，保留 v0.1 Global baseline；外部实现仅作受限背景对比。
- Test-first seam: 先为每 case 写 correctness checksum、工作量等价和 baseline 可加载/版本不匹配测试。

#### AST-051 — 生成原始 Benchmark Artifact 与受限 regression gate

- Primary Rules: R-091
- Blockers: AST-043, AST-047, AST-050
- What to build: 保存环境、构建、case schema、全部 repetition、统计摘要和 invalid 诊断；只对批准的稳定场景/噪声阈值启用 gate。
- Test-first seam: 先写 artifact schema golden、原始样本保留、统计重算、环境 mismatch 和 flaky/noisy case 不误判测试。

### v0.9.0 — Portability and Release Hardening

#### AST-052 — 验证 Linux-only Tier matrix 与单 Astra implementation instance 部署

- Primary Rules: R-111
- Supporting Rules: R-107
- Blockers: AST-003, AST-007, AST-027, AST-044
- What to build: 固定Tier-1 Linux x86_64 GCC/Clang与Tier-2 native Linux AArch64矩阵，审计non-Linux不进入release/package声明，并验证supported deployment只有一个实现实例与coordinator/ID/metrics domain。
- Test-first seam: 先写Linux-only build-matrix manifest、无Windows/MSVC release job审计、shared consumer与不受支持双static copy的负向部署测试。

#### AST-053 — 执行 Linux sanitizer、weak-memory 与 package consumer hardening

- Primary Rules: None (integration evidence only)
- Supporting Rules: R-111, R-093, R-094, R-110
- Blockers: AST-002, AST-003, AST-047, AST-051, AST-052
- What to build: 运行Tier-1 Linux x86_64 GCC/Clang sanitizer证据、Tier-2 native Linux AArch64 weak-memory stress、独立Linux consumer与version矩阵，并审计不存在Windows/MSVC release声明。
- Test-first seam: 先建立会在缺失Linux Tier结果、出现Windows/MSVC release job、sanitizer失败或Linux consumer不能链接时失败的release job。

### v1.0.0 — Stable Source API

#### AST-054 — 冻结 v1 public source/semantic compatibility surface

- Primary Rules: None (integration evidence only)
- Supporting Rules: R-004, R-093, R-094
- Blockers: AST-031, AST-037, AST-041, AST-048, AST-053
- What to build: 审计并冻结 documented public headers、namespace、types、signatures、error/exception 与 observable semantics；明确不承诺跨 toolchain ABI。
- Test-first seam: 先生成 public API compile matrix 与 golden manifest，任何未记录的 surface 漂移都使 gate 失败。

#### AST-055 — 运行 v1 全量 release gate 并发布稳定基线

- Primary Rules: None (integration evidence only)
- Supporting Rules: R-003, R-091, R-111, R-094
- Blockers: AST-051, AST-053, AST-054
- What to build: 汇总全部approved-rule tests、Tier-1 Linux x86_64 GCC/Clang builds、native Linux AArch64 weak-memory证据及docs/package/schema/benchmark artifacts，形成Linux-only v1.0.0可重现发布基线。
- Test-first seam: release checklist 默认失败，只有所有可追踪证据具备、版本一致且 artifact 可重算时才通过。

## Dependency DAG

以下使用 `Ticket <- direct blockers` 表示精确直接依赖；它与每个 Ticket 的 `Blockers` 字段必须逐项一致。

```text
AST-001 <- None
AST-002 <- AST-001
AST-003 <- AST-002
AST-004 <- AST-002, AST-003
AST-005 <- AST-004
AST-006 <- AST-005
AST-007 <- AST-006
AST-008 <- AST-004, AST-005
AST-009 <- AST-008
AST-010 <- AST-009
AST-011 <- AST-009
AST-012 <- AST-008, AST-011
AST-013 <- AST-010, AST-011
AST-014 <- AST-010, AST-011
AST-015 <- AST-007, AST-014
AST-016 <- AST-013, AST-015
AST-017 <- AST-007, AST-014, AST-015
AST-018 <- AST-004, AST-007
AST-019 <- AST-005, AST-018
AST-020 <- AST-007, AST-019
AST-021 <- AST-016, AST-019, AST-020
AST-022 <- AST-008, AST-010, AST-012
AST-023 <- AST-022
AST-024 <- AST-015, AST-022, AST-023
AST-025 <- AST-022, AST-023
AST-026 <- AST-025
AST-027 <- AST-004, AST-025, AST-026
AST-028 <- AST-004, AST-009
AST-029 <- AST-010, AST-022, AST-028
AST-030 <- AST-029
AST-031 <- AST-012, AST-013, AST-030
AST-032 <- AST-009, AST-013
AST-033 <- AST-024, AST-032
AST-034 <- AST-016, AST-033
AST-035 <- AST-012, AST-033
AST-036 <- AST-028, AST-029, AST-032, AST-035
AST-037 <- AST-024, AST-033, AST-034
AST-038 <- AST-010, AST-022
AST-039 <- AST-027, AST-038
AST-040 <- AST-038
AST-041 <- AST-027, AST-039, AST-040
AST-042 <- AST-004, AST-010, AST-015
AST-043 <- AST-042
AST-044 <- AST-007, AST-020, AST-021, AST-043
AST-045 <- AST-004, AST-042
AST-046 <- AST-004, AST-045
AST-047 <- AST-045, AST-046
AST-048 <- AST-012, AST-031, AST-035, AST-043, AST-046
AST-049 <- AST-001, AST-002
AST-050 <- AST-008, AST-023, AST-027, AST-031, AST-037, AST-041, AST-049
AST-051 <- AST-043, AST-047, AST-050
AST-052 <- AST-003, AST-007, AST-027, AST-044
AST-053 <- AST-002, AST-003, AST-047, AST-051, AST-052
AST-054 <- AST-031, AST-037, AST-041, AST-048, AST-053
AST-055 <- AST-051, AST-053, AST-054
```

## Rule Coverage

| Rule | Primary ticket | Supporting tickets | Acceptance criterion | Coverage |
|---|---|---|---|---|
| R-001 | AST-008 | None | `[R-001]` v0.1.0 的调度路径中不存在 Ready Task 绕过 Global Injection Queue 的本地队列路径。 | Covered (1 primary) |
| R-002 | AST-008 | None | `[R-002]` v0.1.0 构建和测试不执行本地 push/pop/steal 或 Chase-Lev 算法。 | Covered (1 primary) |
| R-003 | AST-050 | AST-055 | `[R-003]` Benchmark 可在同一工作负载下运行 Global Queue 基线与后续 Scheduler。 | Covered (1 primary + 1 supporting) |
| R-004 | AST-001 | AST-054 | `[R-004]` 规格规则带有 Applies to，且不会把整体目标误写为单版本范围。 | Covered (1 primary + 1 supporting) |
| R-005 | AST-001 | None | `[R-005]` 每个实现 Ticket 记录目标版本，且不存在覆盖完整项目的单一实现 Ticket。 | Covered (1 primary) |
| R-006 | AST-014 | None | `[R-006]` 已接受父任务在 Graceful Stopping 中提交的子任务得到正常终态，Shutdown Completion 晚于该子任务终结。 | Covered (1 primary) |
| R-007 | AST-014 | None | `[R-007]` 每个竞态提交恰好落在 accepted 或 rejected 一侧，不产生孤儿任务或提前关停。 | Covered (1 primary) |
| R-009 | AST-016 | None | `[R-009]` stop-aware Callable 可自行退出；忽略 stop request 的 Callable 继续运行且阻止真实完成。 | Covered (1 primary) |
| R-010 | AST-015 | None | `[R-010]` 返回后没有目标 Worker 执行用户代码或访问目标 Runtime；忽略 stop 的任务可保持调用未返回。 | Covered (1 primary) |
| R-011 | AST-015 | None | `[R-011]` self-call 不进入 Shutdown Completion，不改变 Runtime 状态或任何任务结果。 | Covered (1 primary) |
| R-012 | AST-014 | None | `[R-012]` 关停不会以队列瞬时为空提前返回，返回后所有 Worker 已 join。 | Covered (1 primary) |
| R-013 | AST-015 | None | `[R-013]` self-call 不截断当前任务的派生权限，也不参与 Shutdown Completion。 | Covered (1 primary) |
| R-014 | AST-016 | None | `[R-014]` 并发升级最多发生一次，后续 `shutdown()` 不恢复任务、stop state 或 admission。 | Covered (1 primary) |
| R-015 | AST-016 | None | `[R-015]` admission、task start 与 mode upgrade 的竞态可被唯一排序，不出现取消后执行或升级后新增工作。 | Covered (1 primary) |
| R-016 | AST-015 | None | `[R-016]` 大量并发调用不会重复取消、重复发布 stop request、并发 join 或提前返回。 | Covered (1 primary) |
| R-019 | AST-014 | None | `[R-019]` 两种关停 API 在 Graceful/Immediate 完成后均稳定立即返回且状态不变。 | Covered (1 primary) |
| R-020 | AST-006 | None | `[R-020]` 最后 Handle 消失不会导致仍在访问的 Worker 发生 use-after-free。 | Covered (1 primary) |
| R-021 | AST-006 | None | `[R-021]` Worker 任务可释放最后 Handle 并继续返回，Runtime State 保持有效且后续真实完成。 | Covered (1 primary) |
| R-022 | AST-006 | None | `[R-022]` Worker handoff 与非 Worker 析构表达相同的默认 Graceful 意图，但前者异步返回。 | Covered (1 primary) |
| R-023 | AST-005 | None | `[R-023]` 注入注册、预留或 coordinator 建立失败时，用户任务从未获得执行窗口。 | Covered (1 primary) |
| R-024 | AST-005 | None | `[R-024]` 资源耗尽故障注入不会让已经 Running 的 handoff 丢失 Runtime State 所有权。 | Covered (1 primary) |
| R-025 | AST-007 | None | `[R-025]` 一个永久任务所在 Runtime 长期 Pending 时，其他 Runtime 仍能 join 并发布 Stopped。 | Covered (1 primary) |
| R-026 | AST-007 | None | `[R-026]` Join Ready 本身不会提前满足完成，Worker 也不会等待 Reaper 先 join 而形成循环等待。 | Covered (1 primary) |
| R-028 | AST-007 | None | `[R-028]` 多轮 Scheduler 创建/销毁复用同一休眠 coordinator，空闲时不 busy-spin 或强持有已完成 Runtime。 | Covered (1 primary) |
| R-031 | AST-019 | None | `[R-031]` begin 返回只证明 Finalization 已不可逆开始；活动 Runtime 可继续在后台推进。 | Covered (1 primary) |
| R-032 | AST-020 | None | `[R-032]` wait 返回后进程内没有 AstraScheduler Worker 或 Reaper coordinator 存活。 | Covered (1 primary) |
| R-033 | AST-020 | None | `[R-033]` 首次 TimedOut 后，同一控制对象或副本可继续等待并最终观察 Completed。 | Covered (1 primary) |
| R-034 | AST-021 | None | `[R-034]` 没有 Scheduler Handle 的 Pending Runtime 也收到 Immediate 请求，Completed Runtime 历史不变。 | Covered (1 primary) |
| R-035 | AST-018 | None | `[R-035]` 没有 begin 返回值时，公共类型系统中不存在合法 wait/upgrade 调用路径。 | Covered (1 primary) |
| R-036 | AST-018 | None | `[R-036]` 一个副本升级后所有副本观察同一过程；全部副本销毁后后台仍继续。 | Covered (1 primary) |
| R-037 | AST-019 | None | `[R-037]` 多个 begin 获得的控制对象观察同一 Completion，空进程 begin 后 Completed 且无 Reaper thread。 | Covered (1 primary) |
| R-038 | AST-019 | None | `[R-038]` Worker 可发起全局终结或升级后继续完成当前任务，不产生 self-wait。 | Covered (1 primary) |
| R-039 | AST-020 | None | `[R-039]` 任一 Scheduler 的 Worker 调用 wait 得到 logic_error，Completion 和唯一 join owner 不受影响。 | Covered (1 primary) |
| R-040 | AST-020 | None | `[R-040]` Worker 的正、零、负 timeout 调用均抛异常而不返回 TimedOut。 | Covered (1 primary) |
| R-041 | AST-020 | None | `[R-041]` wall-clock 跳变不影响等待；TimedOut 线性化后即使返回前完成，本次结果仍为 TimedOut。 | Covered (1 primary) |
| R-042 | AST-020 | None | `[R-042]` 多等待者场景只有一次 coordinator join，Completed 永远晚于该 join。 | Covered (1 primary) |
| R-043 | AST-018 | None | `[R-043]` 最后 Scheduler 消失只使 Reaper 空闲；卸载测试只有 Completed 且对象停止调用后通过。 | Covered (1 primary) |
| R-044 | AST-018 | None | `[R-044]` 调用方可使用稳定限定名比较 Completed/TimedOut，未暴露其他必需结果值。 | Covered (1 primary) |
| R-045 | AST-018 | None | `[R-045]` 编译期 API tests 验证签名、属性、构造能力和异常规格。 | Covered (1 primary) |
| R-046 | AST-018 | None | `[R-046]` public header/API inventory 只出现 R-044/R-045 的 Finalization 能力。 | Covered (1 primary) |
| R-047 | AST-021 | None | `[R-047]` 子进程故障注入确定性进入 terminate；任务异常、TimedOut 和 Pending 场景保持正常域语义。 | Covered (1 primary) |
| R-048 | AST-009 | None | `[R-048]` 复制Handle不复制执行，丢弃全部Handle后已接受Task仍能完成。 | Covered (1 primary) |
| R-049 | AST-011 | None | `[R-049]` 不存在已见终态却读不到Outcome或同一Task副本看到不同Outcome的窗口。 | Covered (1 primary) |
| R-050 | AST-011 | None | `[R-050]` 多个Handle副本可重复观察相同异常或取消，不会终止Worker线程。 | Covered (1 primary) |
| R-051 | AST-011 | None | `[R-051]` 保留任一Handle即可稳定引用Value；临时Handle调用get在编译期失败。 | Covered (1 primary) |
| R-052 | AST-012 | None | `[R-052]` 等待不创建补偿线程或执行foreign Runtime工作；动态环允许永久阻塞。 | Covered (1 primary) |
| R-053 | AST-013 | None | `[R-053]` cancellation/start竞态只有一个分类，重复调用不重复完成或执行。 | Covered (1 primary) |
| R-054 | AST-013 | None | `[R-054]` stop request本身不覆盖用户真实结果，generic callable不会意外收到token。 | Covered (1 primary) |
| R-055 | AST-012 | None | `[R-055]` wait后仍可完整get，多等待者最终观察同一完成。 | Covered (1 primary) |
| R-056 | AST-012 | None | `[R-056]` TimedOut 后Task继续，稍后wait/get仍可观察真实Outcome。 | Covered (1 primary) |
| R-057 | AST-011 | None | `[R-057]` 空对象不会伪装Task状态，有效副本并发观察同一单调生命周期。 | Covered (1 primary) |
| R-058 | AST-009 | None | `[R-058]` 编译期矩阵稳定支持void/copyable/move-only并拒绝reference/immovable。 | Covered (1 primary) |
| R-059 | AST-012 | None | `[R-059]` 深层同步组合确定性失败而不篡改目标Task，Immediate不借Helping启动新工作。 | Covered (1 primary) |
| R-060 | AST-048 | AST-011 | `[R-060]` 关闭Metrics/Trace没有隐藏输出，启用时未观察失败可计数而不改变执行。 | Covered (1 primary + 1 supporting) |
| R-061 | AST-010 | None | `[R-061]` External未启动工作有界，Worker不会因Block自锁，关闭gate能唤醒并拒绝等待者。 | Covered (1 primary) |
| R-062 | AST-010 | None | `[R-062]` 不存在orphan Handle、泄漏slot/outstanding或已拒绝却执行的Callable。 | Covered (1 primary) |
| R-063 | AST-022 | None | `[R-063]` routing source可由Trace验证，Local洪水不能永久饿死Global。 | Covered (1 primary) |
| R-064 | AST-023 | None | `[R-064]` steal_attempt上界可测，固定seed可复现victim序列。 | Covered (1 primary) |
| R-065 | AST-024 | None | `[R-065]` producer与park竞态不产生永久睡眠，空闲不busy-spin。 | Covered (1 primary) |
| R-066 | AST-025 | None | `[R-066]` oracle/production通过同一functional stress，native AArch64验证weak-memory路径。 | Covered (1 primary) |
| R-067 | AST-026 | None | `[R-067]` resize并发steal下每Task最多执行一次，故障注入仍可从Global取得工作。 | Covered (1 primary) |
| R-068 | AST-027 | None | `[R-068]` 边界值测试不越界/ABA，Retry不被误报Empty。 | Covered (1 primary) |
| R-069 | AST-028 | None | `[R-069]` 非DAG输入在admission前确定失败，freeze不重编号且move-only Node可用。 | Covered (1 primary) |
| R-070 | AST-029 | None | `[R-070]` 图不会部分接受，successor不会早启、重复Ready或永久漏release。 | Covered (1 primary) |
| R-071 | AST-030 | None | `[R-071]` dependency failure不伪装为descendant failure，cleanup continuation仍运行。 | Covered (1 primary) |
| R-072 | AST-031 | None | `[R-072]` 并发失败无任意first-error丢失，Graph aggregate不制造synthetic exception。 | Covered (1 primary) |
| R-073 | AST-032 | None | `[R-073]` body只在Worker首次resume执行，frame始终恰有一个owner。 | Covered (1 primary) |
| R-074 | AST-033 | None | `[R-074]` 无并发/递归double-resume、lost wake或double-destroy。 | Covered (1 primary) |
| R-075 | AST-034 | None | `[R-075]` frame不在suspend点被异步销毁，Immediate仍可执行必要unwind segment。 | Covered (1 primary) |
| R-076 | AST-035 | None | `[R-076]` await不形成跨Runtimesteal或递归resume，yield产生可见调度边界。 | Covered (1 primary) |
| R-077 | AST-036 | None | `[R-077]` Graph coroutine node在Metrics/Trace/Outcome中只计一个Task identity。 | Covered (1 primary) |
| R-078 | AST-035 | None | `[R-078]` public API inventory只有wait/wait_for/get/co_await等已批准入口。 | Covered (1 primary) |
| R-079 | AST-037 | None | `[R-079]` Runtime无额外timer线程，Wake Time前不因该timer恢复，取消可撤销heap entry。 | Covered (1 primary) |
| R-080 | AST-038 | None | `[R-080]` 同一Task所有resume segment使用相同base Priority。 | Covered (1 primary) |
| R-081 | AST-039 | None | `[R-081]` 饱和基准长期服务比例接近8:4:2:1且每band有进展。 | Covered (1 primary) |
| R-082 | AST-040 | None | `[R-082]` 相同absolute deadline不因admission延迟重新计时，missed Task仍正常执行。 | Covered (1 primary) |
| R-083 | AST-041 | None | `[R-083]` 同banddeadline顺序可测，低Priority早deadline不越过band策略抢占高Priority。 | Covered (1 primary) |
| R-084 | AST-042 | None | `[R-084]` Metrics启用不改变Task语义，长期counter不wrap倒退。 | Covered (1 primary) |
| R-085 | AST-043 | None | `[R-085]` 并发snapshot字段各自有效，静止后accepted/outcome/steal等关系收敛。 | Covered (1 primary) |
| R-086 | AST-045 | None | `[R-086]` buffer overflow或异常展开不阻塞Scheduler，Collector可安全启动下一代。 | Covered (1 primary) |
| R-087 | AST-046 | None | `[R-087]` 地址复用不造成identity冲突，相同snapshot可确定重放排序。 | Covered (1 primary) |
| R-088 | AST-047 | None | `[R-088]` 零loss相同snapshot/版本byte-stable，损坏输入明确失败且原snapshot可重试。 | Covered (1 primary) |
| R-089 | AST-049 | None | `[R-089]` 计时区不混入构建/销毁，错误工作量不能被报告为更快。 | Covered (1 primary) |
| R-090 | AST-050 | None | `[R-090]` artifact明确adapter限制，不把线程拓扑不同的std::async当主回归oracle。 | Covered (1 primary) |
| R-091 | AST-051 | AST-055 | `[R-091]` 性能结论可追溯原始重复，偶发共享runner噪声不阻断发布。 | Covered (1 primary + 1 supporting) |
| R-111 | AST-052 | AST-002, AST-053, AST-055 | `[R-111]` release matrix和package支持声明只包含Linux x86_64 GCC/Clang与native Linux AArch64，不存在Windows/MSVC release job；非Linux结果不得标记Supported。 | Covered (1 primary + 3 supporting) |
| R-093 | AST-003 | AST-053, AST-054 | `[R-093]` 同一安装header/library版本一致，查询不启动Reaper或分配。 | Covered (1 primary + 2 supporting) |
| R-094 | AST-001 | AST-053, AST-054, AST-055 | `[R-094]` 每个实现Ticket有目标版本且每个tag可独立构建运行。 | Covered (1 primary + 3 supporting) |
| R-095 | AST-044 | None | `[R-095]` 未创建Scheduler时查询无线程副作用，finalization超时/升级可离线诊断。 | Covered (1 primary) |
| R-096 | AST-048 | None | `[R-096]` 离线trace可重建wait edge，运行语义不受诊断启发式改变。 | Covered (1 primary) |
| R-097 | AST-005 | None | `[R-097]` 不存在public Created/Starting或半启动Handle，竞态Scheduler恰好成功纳入或零用户工作失败。 | Covered (1 primary) |
| R-098 | AST-004 | None | `[R-098]` invalid配置在无Runtime副作用前抛invalid_argument，调用方后改原options不影响Runtime。 | Covered (1 primary) |
| R-099 | AST-004 | None | `[R-099]` 并发shutdown时不返回撕裂pair，submit仍以自身transaction决定结果。 | Covered (1 primary) |
| R-100 | AST-004 | None | `[R-100]` 地址复用不改变身份，耗尽在startup/admission前抛overflow_error而不复用。 | Covered (1 primary) |
| R-101 | AST-004 | AST-022, AST-027 | `[R-101]` 同版本不同atomic平台可诚实报告不同backend，artifact复用同一snapshot。 | Covered (1 primary + 2 supporting) |
| R-102 | AST-009 | None | `[R-102]` move-only Callable/unique_ptr参数可提交，lvalue-only target无wrapper时编译期拒绝。 | Covered (1 primary) |
| R-103 | AST-017 | None | `[R-103]` 销毁一个非最后副本不改变status/admission，最后释放才按caller选择RAII或handoff。 | Covered (1 primary) |
| R-104 | AST-019 | None | `[R-104]` Finalization不因进程收尾默认取消已接受工作，半启动Runtime不获得用户执行窗口。 | Covered (1 primary) |
| R-105 | AST-017 | None | `[R-105]` 析构返回后无Worker访问Runtime，不合作任务保持析构未返回。 | Covered (1 primary) |
| R-106 | AST-016 | None | `[R-106]` never-started work不执行且Handle完成；already-started frame仍有机会运行取消点与RAII unwinding。 | Covered (1 primary) |
| R-107 | AST-007 | AST-052 | `[R-107]` 支持配置中Scheduler数量不增加coordinator数，unsupported duplicate instance被部署文档/测试明确拒绝。 | Covered (1 primary + 1 supporting) |
| R-108 | AST-015 | None | `[R-108]` same-runtime Worker得到logic_error且状态不变，other-runtime Worker仍等待目标真实Stopped。 | Covered (1 primary) |
| R-109 | AST-047 | None | `[R-109]` Task hot path不获取logger I/O锁，Trace overflow/export不递归进入日志系统。 | Covered (1 primary) |
| R-110 | AST-002 | AST-053 | `[R-110]` 安装目录可被独立最小工程消费，consumer compile line不包含项目内部依赖或强制诊断选项。 | Covered (1 primary + 1 supporting) |
| R-112 | AST-001 | None | `[R-112]` 仓库指令、开发文档与Ticket verification只给出WSL/Linux命令，WSL build目录与Windows native cache隔离，任何通过声明均可追溯到WSL或native Linux输出。 | Covered (1 primary) |

## Semantic Audit

- Coverage target: 105/105 active rules have one Primary Ticket.
- Superseded rules R-008, R-017, R-018, R-027, R-029, R-030, R-092 are intentionally absent; their replacements are covered by R-106, R-103, R-105, R-107, R-097, R-104, R-111.
- No Ticket introduces Dynamic Worker Scaling、affinity/NUMA、lock-free Global Queue、Timer Wheel、I/O Runtime、Distributed/GPU Runtime or binary ABI stability.
- Lifecycle rules are delivered in v0.1 before later subsystems depend on them; later milestones add routing/backends/features without weakening Task outcome or shutdown semantics.
- AST-053/054/055 are integration/release evidence Tickets. They own no new rule and reference active Supporting Rules only, avoiding duplicate primary ownership.
- Human semantic audit: passed。Ticket 仅切分 approved rules，不扩张变体；早期 supporting criteria 已缩小为各自里程碑的协作证据，最终主实现责任保持唯一。
- Linux-only/WSL revision audit: R-111只替换平台支持范围，R-112只约束本机开发证据；Ticket编号、里程碑与blocker边保持不变。

## Publication Results

- Tracker: local Markdown (`.scratch/astra-scheduler-runtime/issues/`).
- Published at: 2026-08-27.
- Published Tickets: 55 (`AST-001` through `AST-055`).
- Status: `AST-001`、`AST-002`、`AST-003`与`AST-004`为`done`；其余51个Ticket为`ready-for-agent`且`Claimed by: None`。
- Frontier: `AST-005`（AST-004已完成；AST-004 blocker已done）。
- Unpublished or failed items: None.
- Traceability validator: `decisions=168, rules=105, tickets=55, covered_rules=105`（WSL）。

| ID | Milestone | Published file | Status | Blocked by |
|---|---|---|---|---|
| AST-001 | Phase 0 | [01-release-rule-gates.md](issues/01-release-rule-gates.md) | done | None |
| AST-002 | Phase 0 | [02-cmake-package.md](issues/02-cmake-package.md) | done | AST-001 |
| AST-003 | Phase 0 | [03-version-contract.md](issues/03-version-contract.md) | done | AST-002 |
| AST-004 | v0.1.0 | [04-scheduler-public-contract.md](issues/04-scheduler-public-contract.md) | done | AST-002, AST-003 |
| AST-005 | v0.1.0 | [05-startup-transaction.md](issues/05-startup-transaction.md) | done | AST-004 |
| AST-006 | v0.1.0 | [06-runtime-state-handoff.md](issues/06-runtime-state-handoff.md) | done | AST-005 |
| AST-007 | v0.1.0 | [07-reaper-coordinator.md](issues/07-reaper-coordinator.md) | done | AST-006 |
| AST-008 | v0.1.0 | [08-global-worker-runtime.md](issues/08-global-worker-runtime.md) | done | AST-004, AST-005 |
| AST-009 | v0.1.0 | [09-move-only-submit.md](issues/09-move-only-submit.md) | ready-for-agent | AST-008 |
| AST-010 | v0.1.0 | [10-admission-backpressure.md](issues/10-admission-backpressure.md) | ready-for-agent | AST-009 |
| AST-011 | v0.1.0 | [11-task-outcome-state.md](issues/11-task-outcome-state.md) | ready-for-agent | AST-009 |
| AST-012 | v0.1.0 | [12-helping-wait.md](issues/12-helping-wait.md) | ready-for-agent | AST-008, AST-011 |
| AST-013 | v0.1.0 | [13-task-cancellation.md](issues/13-task-cancellation.md) | ready-for-agent | AST-010, AST-011 |
| AST-014 | v0.1.0 | [14-graceful-drain.md](issues/14-graceful-drain.md) | ready-for-agent | AST-010, AST-011 |
| AST-015 | v0.1.0 | [15-shutdown-guards.md](issues/15-shutdown-guards.md) | ready-for-agent | AST-007, AST-014 |
| AST-016 | v0.1.0 | [16-immediate-escalation.md](issues/16-immediate-escalation.md) | ready-for-agent | AST-013, AST-015 |
| AST-017 | v0.1.0 | [17-last-handle-raii.md](issues/17-last-handle-raii.md) | ready-for-agent | AST-007, AST-014, AST-015 |
| AST-018 | v0.1.0 | [18-finalization-control-api.md](issues/18-finalization-control-api.md) | ready-for-agent | AST-004, AST-007 |
| AST-019 | v0.1.0 | [19-finalization-begin.md](issues/19-finalization-begin.md) | ready-for-agent | AST-005, AST-018 |
| AST-020 | v0.1.0 | [20-finalization-wait.md](issues/20-finalization-wait.md) | ready-for-agent | AST-007, AST-019 |
| AST-021 | v0.1.0 | [21-finalization-escalation.md](issues/21-finalization-escalation.md) | ready-for-agent | AST-016, AST-019, AST-020 |
| AST-022 | v0.2.0 | [22-locked-local-routing.md](issues/22-locked-local-routing.md) | ready-for-agent | AST-008, AST-010, AST-012 |
| AST-023 | v0.2.0 | [23-steal-round.md](issues/23-steal-round.md) | ready-for-agent | AST-022 |
| AST-024 | v0.2.0 | [24-park-handshake.md](issues/24-park-handshake.md) | ready-for-agent | AST-015, AST-022, AST-023 |
| AST-025 | v0.3.0 | [25-chase-lev-ordering.md](issues/25-chase-lev-ordering.md) | ready-for-agent | AST-022, AST-023 |
| AST-026 | v0.3.0 | [26-chase-lev-growth.md](issues/26-chase-lev-growth.md) | ready-for-agent | AST-025 |
| AST-027 | v0.3.0 | [27-chase-lev-indices.md](issues/27-chase-lev-indices.md) | ready-for-agent | AST-004, AST-025, AST-026 |
| AST-028 | v0.4.0 | [28-task-graph-freeze.md](issues/28-task-graph-freeze.md) | ready-for-agent | AST-004, AST-009 |
| AST-029 | v0.4.0 | [29-graph-admission.md](issues/29-graph-admission.md) | ready-for-agent | AST-010, AST-022, AST-028 |
| AST-030 | v0.4.0 | [30-graph-edge-policies.md](issues/30-graph-edge-policies.md) | ready-for-agent | AST-029 |
| AST-031 | v0.4.0 | [31-graph-run-control.md](issues/31-graph-run-control.md) | ready-for-agent | AST-012, AST-013, AST-030 |
| AST-032 | v0.5.0 | [32-coroutine-spawn.md](issues/32-coroutine-spawn.md) | ready-for-agent | AST-009, AST-013 |
| AST-033 | v0.5.0 | [33-coroutine-resume-handshake.md](issues/33-coroutine-resume-handshake.md) | ready-for-agent | AST-024, AST-032 |
| AST-034 | v0.5.0 | [34-suspended-cancellation.md](issues/34-suspended-cancellation.md) | ready-for-agent | AST-016, AST-033 |
| AST-035 | v0.5.0 | [35-source-runtime-await.md](issues/35-source-runtime-await.md) | ready-for-agent | AST-012, AST-033 |
| AST-036 | v0.5.0 | [36-graph-coroutine-identity.md](issues/36-graph-coroutine-identity.md) | ready-for-agent | AST-028, AST-029, AST-032, AST-035 |
| AST-037 | v0.5.0 | [37-worker-timers.md](issues/37-worker-timers.md) | ready-for-agent | AST-024, AST-033, AST-034 |
| AST-038 | v0.6.0 | [38-base-priority.md](issues/38-base-priority.md) | ready-for-agent | AST-010, AST-022 |
| AST-039 | v0.6.0 | [39-priority-bands.md](issues/39-priority-bands.md) | ready-for-agent | AST-027, AST-038 |
| AST-040 | v0.6.0 | [40-task-deadline.md](issues/40-task-deadline.md) | ready-for-agent | AST-038 |
| AST-041 | v0.6.0 | [41-global-edf.md](issues/41-global-edf.md) | ready-for-agent | AST-027, AST-039, AST-040 |
| AST-042 | v0.7.0 | [42-runtime-metrics.md](issues/42-runtime-metrics.md) | ready-for-agent | AST-004, AST-010, AST-015 |
| AST-043 | v0.7.0 | [43-metrics-snapshot.md](issues/43-metrics-snapshot.md) | ready-for-agent | AST-042 |
| AST-044 | v0.7.0 | [44-process-metrics.md](issues/44-process-metrics.md) | ready-for-agent | AST-007, AST-020, AST-021, AST-043 |
| AST-045 | v0.7.0 | [45-trace-collector.md](issues/45-trace-collector.md) | ready-for-agent | AST-004, AST-042 |
| AST-046 | v0.7.0 | [46-trace-event-schema.md](issues/46-trace-event-schema.md) | ready-for-agent | AST-004, AST-045 |
| AST-047 | v0.7.0 | [47-chrome-trace-export.md](issues/47-chrome-trace-export.md) | ready-for-agent | AST-045, AST-046 |
| AST-048 | v0.7.0 | [48-wait-await-diagnostics.md](issues/48-wait-await-diagnostics.md) | ready-for-agent | AST-012, AST-031, AST-035, AST-043, AST-046 |
| AST-049 | v0.8.0 | [49-benchmark-harness.md](issues/49-benchmark-harness.md) | ready-for-agent | AST-001, AST-002 |
| AST-050 | v0.8.0 | [50-benchmark-corpus.md](issues/50-benchmark-corpus.md) | ready-for-agent | AST-008, AST-023, AST-027, AST-031, AST-037, AST-041, AST-049 |
| AST-051 | v0.8.0 | [51-benchmark-artifacts.md](issues/51-benchmark-artifacts.md) | ready-for-agent | AST-043, AST-047, AST-050 |
| AST-052 | v0.9.0 | [52-supported-tier-matrix.md](issues/52-supported-tier-matrix.md) | ready-for-agent | AST-003, AST-007, AST-027, AST-044 |
| AST-053 | v0.9.0 | [53-platform-hardening.md](issues/53-platform-hardening.md) | ready-for-agent | AST-002, AST-003, AST-047, AST-051, AST-052 |
| AST-054 | v1.0.0 | [54-v1-api-freeze.md](issues/54-v1-api-freeze.md) | ready-for-agent | AST-031, AST-037, AST-041, AST-048, AST-053 |
| AST-055 | v1.0.0 | [55-v1-release-gate.md](issues/55-v1-release-gate.md) | ready-for-agent | AST-051, AST-053, AST-054 |

## Approval

Status: approved
Approved by: project owner（user）
Approved at: 2026-08-27
Revision approved by: project owner（user）
Revision approved at: 2026-08-27（Linux-only/WSL Spec）

本计划及Linux-only/WSL修订均已获明确批准。具体 Ticket 按依赖顺序发布到 `.scratch/astra-scheduler-runtime/issues/<NN>-<slug>.md`；发布不等于实现。AST-001、AST-002、AST-003、AST-004、AST-005、AST-006、AST-007、AST-008 已完成，当前就绪执行前沿为 AST-009 与 AST-018。
