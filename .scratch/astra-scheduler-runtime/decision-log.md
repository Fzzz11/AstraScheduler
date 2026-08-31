# AstraScheduler Runtime Decision Log

## D-001 — v0.1.0 保留为全局队列正确性基线

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

参考设计稿给出了互相冲突的里程碑边界：Phase 1 与 Git 里程碑把 v0.1.0 定义为 Basic Scheduler、把 Work Stealing 放到后续版本；第 49 节则把带锁 Local Queue 和 Work Stealing 也纳入 v0.1。必须先统一这个边界，后续计划、测试与 Benchmark 才能引用同一基线。

### Decision

v0.1.0 不包含 Per-Worker Local Queue 或 Work Stealing，而是保留为基于互斥 Global Injection Queue 的 Basic Scheduler 正确性与性能基线。

### Invariants

- v0.1.0 不实现任务窃取。
- v0.1.0 不以 Chase-Lev Deque 作为完成条件。
- v0.1.0 完成后仍保留为后续 Work-Stealing 版本的可运行 Benchmark 对照组。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| v0.1.0 | Basic Scheduler 基线 | 所有 Ready Task 通过带锁 Global Injection Queue 调度 |
| 后续版本 | Work-Stealing 演进 | 引入 Per-Worker Local Queue，再独立替换为 Chase-Lev Deque |

### Rationale

该分层把生命周期与 Future 等基础运行时错误、Work-Stealing 调度错误、Chase-Lev 内存模型错误分开定位；同时提供真实的全局队列性能基线，避免后续 Benchmark 只能与另一个项目比较。

### Rejected alternatives

- v0.1.0 直接包含带锁 Work Stealing：把基础生命周期与任务结果错误和 Work-Stealing 调度错误混入同一里程碑，也失去独立的全局队列发布基线。

### Consequences

- 每个里程碑都有可独立构建、测试和测量的结果。
- 首次交付 Work Stealing 会晚一个里程碑。
- 后续计划必须明确区分 Basic Scheduler、Locked Work-Stealing Scheduler 与 Chase-Lev Scheduler。

### Non-goals and deferred risks

- 本决策不定义 v0.1.0 的完整 API、shutdown、背压或任务结果语义。
- 本决策不固定后续 Work-Stealing 与 Chase-Lev 版本号。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 v0.1.0 仅作为 Global Queue Basic Scheduler 基线。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 1734–1788、2185–2219、2237–2249 行；仓库当前没有实现代码。

### Traceability

- ADR: None
- Spec destinations: R-001, R-002, R-003
- Tickets: Pending
- Tests: Pending

## D-002 — Graceful shutdown 允许同 Scheduler 的运行中任务继续派生工作

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

Graceful shutdown 承诺完成已经接受的任务，但一个已接受任务可能在 `Stopping` 阶段调用同一个 Scheduler 的 `submit()` 派生子任务。若这种内部提交与外部新提交一起被拒绝，父任务的既有工作可能无法完成，等待子任务结果时还可能永久阻塞。

### Decision

Scheduler 进入 graceful `Stopping` 后，仍接受由该 Scheduler 当前 Worker 上正在执行的任务发起的内部提交；这类派生任务属于本次 drain 的工作闭包。

### Invariants

- 只有调用线程当前正在执行该 Scheduler 已接受的任务时，提交才具有此内部派生权限。
- 来自其他线程或其他 Scheduler Worker 的提交不因本决策获得内部派生权限。
- Graceful shutdown 不得在此类已接受派生任务仍未终结时报告完成。
- 每个获准的内部提交必须得到正常完成、失败或取消结果，不得静默丢弃。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Running | 所有合法提交 | 按正常 admission 规则处理 |
| Graceful Stopping | 同 Scheduler Worker 内部提交 | 继续接受并纳入 drain 工作闭包 |
| Graceful Stopping | 其他来源提交 | 不由本决策授权；外部 admission 规则另行确定 |
| Immediate Stopping | 所有来源 | 不由本决策授权；`shutdown_now()` 语义另行确定 |

### Rationale

Graceful shutdown 应排空“已接受工作的传递闭包”，而不只是调用 `shutdown()` 瞬间的队列快照。实现上通常需要识别所属 Scheduler 的 WorkerContext，并以包含 queued、running 和获准派生任务的 outstanding-work 计数决定 drain 完成。

### Rejected alternatives

- `Stopping` 后无条件拒绝所有 `submit()`：会截断已接受工作的必要派生链，使 graceful shutdown 退化为仅排空某个瞬时队列快照。

### Consequences

- 已接受任务在关停竞态中不会仅因派生时间稍晚而丢失必要子任务。
- v0.1.0 即需可靠识别 same-scheduler Worker 内部提交，并测试 submit/shutdown 线性化竞态。
- 该权限不自动解决 Worker 阻塞等待同 Scheduler 子任务可能造成的线程饥饿死锁；等待策略需另行决定。

### Non-goals and deferred risks

- 本决策不定义外部提交在 `Stopping` 时的错误表示。
- 本决策不定义 `shutdown_now()`、Worker 内调用 `shutdown()` 或 TaskHandle 等待策略。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 graceful `Stopping` 期间继续接纳同 Scheduler Worker 的内部派生任务。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 382–404、1029–1107 行；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0001](../../docs/adr/0001-graceful-shutdown-drains-internal-work.md)
- Spec destinations: R-006
- Tickets: Pending
- Tests: Pending

## D-003 — Graceful Stopping 转换线性化地关闭 External Submission

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

External Submission 可能与 `shutdown()` 并发发生。若提交先检查 `Running`、关停随后观察到无工作并让 Worker 退出、提交最后才入队，就会产生已返回给调用方但永远无人执行的孤儿任务。

### Decision

External Submission 与 Scheduler 的 `Running → Stopping` 转换必须存在单一线性化顺序；只有 admission 线性化点早于该状态转换的 External Submission 才被接受，晚于该转换的提交一律拒绝。

### Invariants

- Scheduler 一旦线性化进入 graceful `Stopping`，不得再接受 External Submission。
- 每个与 `shutdown()` 竞态的 External Submission 必须恰好得到“已接受”或“已拒绝”之一，不存在中间状态。
- 已接受的 External Submission 必须在关停完成判断前计入 Drain Work Closure。
- 已拒绝的 External Submission 不得入队，也不得增加 outstanding work。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| External Submission 先线性化 | `Running` | 接受并纳入 Drain Work Closure |
| `Running → Stopping` 先线性化 | Graceful `Stopping` | 拒绝且不创建运行时任务 |
| Internal Submission | Graceful `Stopping` | 由 D-002 单独授权，不适用本决策的拒绝规则 |

### Rationale

把 admission 与状态转换放入同一可证明的顺序，可以避免 check-then-enqueue 竞态、孤儿 Future、关停提前返回和未计数工作。具体线性化机制可先使用互斥保护，后续再根据 Benchmark 优化。

### Rejected alternatives

- `Stopping` 期间 best-effort 接受 External Submission：无法给竞态提交建立可验证的唯一结果，可能产生未计数任务或孤儿 Future。

### Consequences

- submit/shutdown 并发测试可以验证每个提交恰好落在转换的一侧。
- v0.1.0 的 admission gate、任务入队与 outstanding-work 核算必须作为一个原子协议设计。
- 拒绝提交的 API 表示方式需另行决定。

### Non-goals and deferred risks

- 本决策不规定拒绝通过异常、状态值还是 rejected handle 表达。
- 本决策不规定 `shutdown_now()` 的 admission 语义。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受以 `Running → Stopping` 的线性化点关闭 External Submission。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 1029–1107、1645–1650 行；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0001](../../docs/adr/0001-graceful-shutdown-drains-internal-work.md)
- Spec destinations: R-007
- Tickets: Pending
- Tests: Pending

## D-004 — 当前设计讨论覆盖整个 AstraScheduler 跨版本目标

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-001 只界定首个可交付版本，但这容易被误读为本次讨论只设计 v0.1.0。项目 owner 明确澄清，本次讨论面向整个项目开发，后续版本只是分阶段实现完整运行时的载体。

### Decision

当前设计讨论与本台账覆盖 AstraScheduler 整个跨版本目标；各版本用于分阶段交付，不构成彼此割裂的设计范围。

### Invariants

- 风险 frontier 不在 v0.1.0 语义讨论完成后停止，而应继续覆盖计划中的完整运行时能力。
- 跨版本公共概念与不变量应按最终运行时的一致演进进行评审。
- 某项结论若只适用于特定版本，必须在该决策的 Scope and variants 中明确标注。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 整体设计 | 全部计划版本 | 定义跨版本架构、公共语义、边界与演进约束 |
| 版本交付 | 单个版本 | 选择该阶段实现的能力，不缩小整体设计讨论范围 |

### Rationale

先建立贯穿各版本的一致决策，能够避免早期版本形成与 DAG、Coroutine、Priority、Deadline 或 Cancellation 不兼容的公共契约，同时仍允许以小步版本交付控制实现风险。

### Rejected alternatives

- 本次只讨论 v0.1.0：会把后续特性所需的跨模块约束推迟到公共 API 已形成之后。

### Consequences

- 台账 feature slug 从 `scheduler-foundation` 调整为 `astra-scheduler-runtime`。
- 后续问题会覆盖完整项目，但仍按风险与依赖逐个确认。
- 版本范围决策继续单独记录，不能把“最终需要”误写成“首版必须实现”。

### Non-goals and deferred risks

- 本决策不要求在一个版本或一个 Ticket 中实现全部能力。
- 本决策不在当前条目中固定最终版本数量或发布日期。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确说明本次讨论覆盖整个项目开发，不只针对某一个版本。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 105–156、1717–1907、2111–2250 行。

### Traceability

- ADR: None
- Spec destinations: R-004
- Tickets: Pending
- Tests: Documentation-only

## D-005 — 后续实现按版本拆分为 Tickets

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

完整运行时跨越多个版本与模块，若作为单一实现任务推进，将失去可审查的依赖边、验收边界与阶段性完成证据。项目 owner 明确计划后续通过 Tickets 完成各版本开发。

### Decision

后续实现工作按目标版本拆分为 Tickets 推进，而不是把整个 AstraScheduler 作为一个不可分割的实现任务。

### Invariants

- 整个项目不得合并为一个巨型实现 Ticket。
- 每个实现 Ticket 必须明确其目标版本。
- Ticket 拆分发生在本轮设计结论形成之后。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 当前讨论 | 跨版本设计 | 形成可供后续拆票使用的持久决策 |
| 后续开发 | 版本实现 | 通过归属于目标版本的 Tickets 分阶段完成 |

### Rationale

按版本拆票使工作范围、阻塞关系、验收证据和发布边界可独立追踪，同时保留 D-004 所要求的整体架构一致性。

### Rejected alternatives

- 整个项目使用单一实现 Ticket：范围过大，无法形成可靠的完成条件和依赖顺序。

### Consequences

- 后续拆票必须保持版本归属，并引用本台账形成的约束。
- 当前会话继续设计，不自动开始实现或拆票。

### Non-goals and deferred risks

- 本决策不规定每个版本需要多少 Tickets。
- 本决策不规定 Ticket 承载平台或发布时间。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确表示后续会拆分为 Tickets，完成各版本开发。
- Code or data evidence: None。

### Traceability

- ADR: None
- Spec destinations: R-005
- Tickets: Pending
- Tests: Documentation-only

## D-006 — Immediate Shutdown 将所有尚未开始的已接受任务终结为 Cancelled

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

`shutdown_now()` 需要停止执行尚未开始的已接受任务。若实现只清空队列或释放 DAG Waiting 节点，而没有完成其共享结果状态，TaskHandle/Future 的等待者将永久阻塞；若取消与 Worker 启动任务的竞态没有唯一结果，同一任务还可能既被报告取消又实际执行。

### Decision

Immediate Shutdown 线性化后，所有尚未进入 `Running` 的已接受任务都必须终结为 `Cancelled`，其结果状态变为 ready 并唤醒全部等待者；这些任务的用户 Callable 不得再执行。

### Invariants

- 每个尚未进入 `Running` 的已接受任务必须恰好一次转入终态 `Cancelled`。
- 被标记为 `Cancelled` 的任务不得随后进入 `Running` 或调用用户 Callable。
- Task 启动与 Immediate Shutdown 取消之间必须有唯一胜者：任务要么先进入 `Running`，要么先进入 `Cancelled`。
- 清空调度队列或图节点不得静默丢弃任务的结果状态。
- 所有等待该任务的调用方都必须在取消终态发布后被唤醒。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Waiting / Ready | 已接受但尚未开始 | 终结为 `Cancelled`，Callable 不执行 |
| Running | 已开始执行 | 不适用本决策；只能请求协作停止，规则另行确定 |
| Suspended Coroutine | 已执行后挂起 | 不适用本决策；frame 与 awaiter 取消规则另行确定 |

### Rationale

Cancellation 必须是可观察的任务终态，而不是队列管理副作用。通过原子化 `Running`/`Cancelled` 竞争并完成共享结果，可以同时避免 double execution、取消后仍执行以及 Future 永不 ready。

### Rejected alternatives

- 直接丢弃队列元素、不完成结果状态：会产生永久等待的 TaskHandle/Future，并使取消不可观察。

### Consequences

- Task 控制块必须独立于队列元素存活，直到所有结果观察者释放它。
- Queue、DAG execution state 与 TaskHandle/Future 都必须遵守同一终态发布协议。
- 取消结果由 `get()` 如何表达需另行决定。

### Non-goals and deferred risks

- 本决策不定义 Running Task 的停止保证。
- 本决策不定义 Coroutine frame 注销、`shutdown_now()` 是否阻塞或取消异常类型。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 Immediate Shutdown 将所有尚未开始的已接受任务终结为 `Cancelled` 并唤醒等待者。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 580–624、690–698、735–785、1087–1107 行；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0002](../../docs/adr/0002-immediate-shutdown-publishes-cancellation.md)
- Spec destinations: R-008, Open Questions
- Tickets: Pending
- Tests: Pending

## D-007 — Immediate Shutdown 仅请求 Running Task 协作停止

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

Immediate Shutdown 发生时，部分任务可能已经进入 `Running` 并持有锁、内存、文件或其他资源。C++ 没有通用且安全的方式在任意指令点强制终止这类函数；强杀线程会跳过栈展开并破坏进程内不变量。

### Decision

Immediate Shutdown 对已经进入 `Running` 的 Task 只发布协作式 stop request，绝不强制终止其线程或用户 Callable；任务保持运行，直到 Callable 自行返回或抛出异常。

### Invariants

- Runtime 不得使用强制线程取消、异步异常或进程终止来停止单个 Running Task。
- Immediate Shutdown 必须向每个 Running Task 的 cancellation state 发布 stop request。
- stop request 不等价于任务已经停止，也不立即把 Running Task 标记为 `Cancelled`。
- 忽略 stop request 的 Callable 可以继续运行；Runtime 不得伪造其已终结状态。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Stop-aware Callable | `Running` | 观察 stop request，并自行决定安全退出点 |
| Non-cooperative Callable | `Running` | 可以继续执行直到自然返回或抛出 |
| Waiting / Ready Task | 尚未开始 | 由 D-006 直接终结为 `Cancelled` |
| Suspended Coroutine | 已挂起 | awaiter 注销与 frame 生命周期另行确定 |

### Rationale

协作式停止保留 C++ 栈展开、RAII 与用户代码不变量；代价是 Runtime 无法为不合作任务提供有界停止时间。该限制必须成为公共契约，而不能由 `shutdown_now()` 的命名暗示强制终止能力。

### Rejected alternatives

- 强制终止执行 Running Task 的 Worker 线程：会跳过可靠的栈展开与 RAII 清理，并可能永久破坏进程内共享状态。

### Consequences

- Task cancellation state 需要在进入 `Running` 前建立，并能被 `std::stop_token` 或等价机制观察。
- `shutdown_now()` 是否等待 Running Task、以及任务响应 stop request 后发布何种终态，需分别决定。
- 文档不得宣称 AstraScheduler 能安全强杀任意 C++ Callable。

### Non-goals and deferred risks

- 本决策不定义 stop-aware Callable 的最终 `Completed`/`Cancelled` 判定。
- 本决策不定义等待时长、超时、Worker detach 或 Scheduler 析构行为。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受禁止 Runtime 强杀 Running Task，并将 Immediate Shutdown 限定为发布协作式 stop request。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 735–785、1087–1107 行；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0002](../../docs/adr/0002-immediate-shutdown-publishes-cancellation.md)
- Spec destinations: R-009, Open Questions
- Tickets: Pending
- Tests: Pending

## D-008 — 非 Worker 调用的 shutdown_now 同步等待全部 Worker 退出

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

如果 `shutdown_now()` 在发布取消与 stop request 后立即返回，Worker 仍可能执行用户代码并访问 Scheduler 内部状态。调用方此时销毁 Scheduler、释放任务依赖资源或把返回解释为“已经停止”，会产生 use-after-free 与生命周期竞态。另一方面，D-007 已确认不合作 Callable 可能无限运行，因此同步等待也可能没有时间上界。

### Decision

从非 Worker 线程调用 `shutdown_now()` 时，该调用是同步的；它只在全部 Worker 线程退出并完成 join、Scheduler 进入 `Stopped` 后返回。名称中的 “now” 表示立即采用取消待执行任务并请求运行中任务停止的策略，不表示立即返回。

### Invariants

- `shutdown_now()` 从非 Worker 线程返回时，不得仍有 Worker 执行用户 Callable 或访问 Scheduler Worker Runtime。
- 返回前必须完成所有 Worker join，并发布 `Stopped` 生命周期状态。
- 若 Running Task 忽略 stop request，`shutdown_now()` 可以无限期阻塞；Runtime 不得 detach Worker 来伪造完成。
- 本决策不允许通过超时返回一个仍在后台运行但表面已 `Stopped` 的 Scheduler。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 非 Worker 调用方 | `shutdown_now()` | 同步等待全部 Worker 退出并 join |
| 当前 Scheduler 的 Worker | `shutdown_now()` | 自 join 风险，调用契约另行确定 |
| 异步停止请求 | 潜在独立 API | 不由本决策提供或命名 |

### Rationale

同步返回为 Scheduler 状态和资源所有权提供清晰边界：方法返回后，调用方可以确定 Worker Runtime 已经静止。接受无界等待比 detach 线程更符合 C++ RAII 与内存安全；若未来需要非阻塞控制，应使用名称和生命周期都不同的请求式 API。

### Rejected alternatives

- 发布 stop request 后立即返回并让 Worker 后台继续运行：会使调用方无法判断 Scheduler 资源何时可以安全释放，并增加 detach 与 use-after-free 风险。

### Consequences

- 测试必须覆盖不合作任务导致关停等待，以及最后一个 Worker 退出后的状态发布顺序。
- API 文档必须明确 “Immediate” 描述取消策略而非响应时延。
- Worker 内调用、并发关停调用与析构策略需另行决定。

### Non-goals and deferred risks

- 本决策不定义 Worker 内调用 `shutdown_now()` 的行为。
- 本决策不提供 timeout、detach 或异步 shutdown handle。
- Suspended Coroutine 的注销与 frame 终结仍需单独闭合。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受非 Worker 调用的 `shutdown_now()` 同步等待全部 Worker 退出并 join，即使可能因不合作任务而无限期阻塞。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 1029–1107、1645–1650 行；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0002](../../docs/adr/0002-immediate-shutdown-publishes-cancellation.md)
- Spec destinations: R-010
- Tickets: Pending
- Tests: Pending

## D-009 — 同 Scheduler Worker 调用 shutdown_now 在改变状态前被拒绝

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-008 要求 `shutdown_now()` 同步等待全部 Worker 退出并 join。若当前正在该 Scheduler 上执行任务的 Worker 调用此方法，它既无法在方法返回前退出，又必须等待包含自身在内的 Worker 集合退出，形成确定性的 self-join 死锁。若方法只对 Worker 调用方偷偷改成异步，则同一 API 的返回保证会依调用线程而变化。

### Decision

当前 Scheduler 的 Worker 调用 `shutdown_now()` 必须在任何生命周期状态转换、任务取消或 stop request 发生前被同步拒绝；该调用不得启动 Immediate Shutdown。

### Invariants

- Runtime 必须识别调用方是否为当前 Scheduler 的 Worker。
- 被拒绝的 self-shutdown 调用不得改变 SchedulerState。
- 被拒绝的调用不得取消 Waiting/Ready Task，也不得向 Running Task 发布 stop request。
- `shutdown_now()` 不得因调用方是 Worker 而静默降级为异步语义。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 当前 Scheduler Worker | `shutdown_now()` | 在任何副作用前拒绝 |
| 非 Worker 调用方 | `shutdown_now()` | 适用 D-008 的同步 join 语义 |
| 其他 Scheduler Worker | 对目标 Scheduler 而言不是内部 Worker | 不适用本决策；按目标 Scheduler 的非 Worker 规则处理 |

### Rationale

在副作用前拒绝能保留 `shutdown_now()` 单一且可证明的同步契约，并避免 self-join。若任务未来需要发起停止，应设计不承诺 join 的独立 request API，而不是让阻塞式 shutdown 方法根据调用上下文改变含义。

### Rejected alternatives

- Worker 调用时继续执行同步 join：当前 Worker 无法在方法返回前退出，会形成确定性的 self-join 死锁。
- Worker 调用时静默改为异步返回：使同一 API 的返回保证依赖调用上下文，破坏 D-008 的同步契约。

### Consequences

- WorkerContext 必须能够可靠判断 Scheduler 身份，不能只使用一个无归属的布尔标志。
- 错误通过异常、状态值或其他机制表达，需在公共 API 设计中单独决定。
- 任务主动请求 Runtime 停止的需求需由独立 API 评估。

### Non-goals and deferred risks

- 本决策不定义 graceful `shutdown()` 的 Worker 调用行为。
- 本决策不承诺提供异步 Immediate Shutdown request API。
- 本决策不处理两个 Scheduler 之间互相等待造成的跨 Runtime 死锁。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受同 Scheduler Worker 的 `shutdown_now()` 在产生任何副作用前被同步拒绝。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 398–404、1029–1107 行；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0002](../../docs/adr/0002-immediate-shutdown-publishes-cancellation.md)
- Spec destinations: R-011, Open Questions
- Tickets: Pending
- Tests: Pending

## D-010 — 非 Worker 调用的 graceful shutdown 同步排空工作闭包并 join

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-002 与 D-003 已定义 Graceful Stopping 的接纳和排空边界，但尚未固定 `shutdown()` 何时向调用方返回。若方法仅发起关停便返回，调用方仍无法安全销毁 Scheduler 或已接受任务依赖的资源；若同步等待，则由 Internal Submission 形成的 Drain Work Closure 可能持续任意长时间。

### Decision

从非 Worker 线程调用 graceful `shutdown()` 时，该调用是同步的；它只在 Drain Work Closure 中全部任务到达终态、所有 Worker 退出并完成 join、Scheduler 发布 `Stopped` 后返回。

### Invariants

- `shutdown()` 返回前，Drain Work Closure 中不得仍有 Waiting、Ready 或 Running Task。
- 返回前必须完成所有 Worker join，并发布 `Stopped` 生命周期状态。
- 由 D-002 授权的 Internal Submission 必须纳入同一次同步排空，不能因发生在 `Stopping` 阶段而遗留到后台。
- 若任务、依赖链或内部派生链不能终结，`shutdown()` 可以无限期阻塞；Runtime 不得 detach Worker 或伪造 `Stopped`。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 非 Worker 调用方 | graceful `shutdown()` | 同步排空 Drain Work Closure 并 join 全部 Worker |
| 当前 Scheduler 的 Worker | graceful `shutdown()` | 存在 self-wait/self-join 风险，调用契约另行确定 |
| Immediate Shutdown | `shutdown_now()` | 由 D-006 至 D-009 单独定义 |
| 异步 graceful 请求 | 潜在独立 API | 不由本决策提供或命名 |

### Rationale

同步返回为 Graceful Shutdown 建立与 RAII 一致的资源安全边界：方法返回即表示已接受工作的传递闭包已经终结，Worker Runtime 已静止。若未来需要异步编排，应由返回 completion handle 的独立请求式 API 表达，而不是弱化 `shutdown()` 的完成含义。

### Rejected alternatives

- 发起 graceful stopping 后立即返回：调用方无法据此确定已接受任务、Worker Runtime 与相关资源何时真正静止，容易造成过早销毁和 use-after-free。

### Consequences

- 终止检测必须覆盖 queued、running 以及 `Stopping` 阶段获准的 Internal Submission，不能只观察某个队列为空。
- API 文档必须明确 graceful `shutdown()` 可能因不终结的任务或无限派生链而无限期阻塞。
- Worker 内调用、多个并发关停调用与析构策略仍需分别决定。

### Non-goals and deferred risks

- 本决策不定义当前 Scheduler Worker 调用 `shutdown()` 的行为。
- 本决策不提供 timeout、detach 或异步 completion handle。
- 本决策不定义任务内部同步等待同 Scheduler 子任务所导致的线程饥饿死锁如何处理。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受非 Worker 调用的 graceful `shutdown()` 同步排空 Drain Work Closure、join 全部 Worker 并在 `Stopped` 发布后返回。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 1029–1083、1645–1650 行；D-002、D-003 与 ADR-0001 已固定 Drain Work Closure 的接纳边界。

### Traceability

- ADR: [ADR-0001](../../docs/adr/0001-graceful-shutdown-drains-internal-work.md)
- Spec destinations: R-012
- Tickets: Pending
- Tests: Pending

## D-011 — 同 Scheduler Worker 调用 graceful shutdown 在改变状态前被拒绝

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-010 要求 `shutdown()` 同步等待 Drain Work Closure 全部终结并 join Worker。若当前正在该 Scheduler 上执行任务的 Worker 调用该方法，当前任务本身已经属于 Drain Work Closure：任务必须等 `shutdown()` 返回才能终结，而 `shutdown()` 又必须等该任务终结，形成确定性的 self-wait；之后还存在 self-join。若只对 Worker 调用方静默改成异步，同一 API 的返回保证又会依调用上下文而变化。

### Decision

当前 Scheduler 的 Worker 调用 graceful `shutdown()` 必须在任何生命周期状态转换或 admission 关闭发生前被同步拒绝；该调用不得启动 Graceful Shutdown。

### Invariants

- Runtime 必须识别调用方是否为当前 Scheduler 的 Worker。
- 被拒绝的调用不得改变 SchedulerState，也不得线性化 `Running → Stopping`。
- 被拒绝的调用不得关闭 External Submission admission，也不得改变 outstanding-work 核算。
- `shutdown()` 不得因调用方是 Worker 而静默降级为异步语义。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 当前 Scheduler Worker | graceful `shutdown()` | 在任何副作用前拒绝 |
| 非 Worker 调用方 | graceful `shutdown()` | 适用 D-010 的同步 drain 与 join 语义 |
| 其他 Scheduler Worker | 对目标 Scheduler 而言不是内部 Worker | 不适用本决策；按目标 Scheduler 的非 Worker 规则处理 |
| Worker 发起异步 graceful 请求 | 潜在独立 API | 不由本决策提供或命名 |

### Rationale

在副作用前拒绝同时避免 self-wait 和 self-join，并保留 `shutdown()` 单一、可证明的同步完成契约。若任务需要发起 Runtime 关停，应由不承诺同步 drain/join 的独立 request API 表达。

### Rejected alternatives

- Worker 调用时继续同步 drain 与 join：当前任务本身属于 Drain Work Closure，会先形成 self-wait，随后还存在 self-join。
- Worker 调用时静默改为异步返回：使同一 API 的返回保证依赖调用上下文，破坏 D-010 的同步契约。

### Consequences

- WorkerContext 的 Scheduler 身份判断可与 D-009 复用，但 graceful 与 immediate 的拒绝测试必须分别覆盖。
- 错误通过异常、状态值或其他机制表达，留到公共 API 设计中单独决定。
- 任务主动请求 Graceful Shutdown 的需求需由独立 API 评估。

### Non-goals and deferred risks

- 本决策不承诺提供异步 graceful shutdown request API。
- 本决策不定义多个非 Worker 并发调用关停 API 的协调规则。
- 本决策不处理两个 Scheduler 之间互相同步关停造成的跨 Runtime 死锁。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受同 Scheduler Worker 的 graceful `shutdown()` 在产生任何副作用前被同步拒绝。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 398–404、1029–1083 行；D-002、D-003、D-010 与 ADR-0001 已固定同步 drain 和 join 语义。

### Traceability

- ADR: [ADR-0001](../../docs/adr/0001-graceful-shutdown-drains-internal-work.md)
- Spec destinations: R-013, Open Questions
- Tickets: Pending
- Tests: Pending

## D-012 — Immediate Shutdown 可单向升级进行中的 Graceful Shutdown

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

非 Worker 线程可能先调用 `shutdown()` 使 Scheduler 进入 Graceful Stopping，另一个非 Worker 线程随后调用 `shutdown_now()`。若首个模式永久获胜，后一个调用无法兑现 D-006 与 D-007 的取消和 stop request 语义；若允许模式来回切换，已经取消的任务又不可能恢复。当前单一 `Stopping` 状态不足以表达这种单向策略变化。

### Decision

当 Scheduler 正在 Graceful Stopping 时，非 Worker 调用的 `shutdown_now()` 必须以单一线性化点把当前关停模式升级为 Immediate，并从该点起应用 D-006 至 D-008；关停模式一旦为 Immediate，后续 `shutdown()` 不得将其降级回 Graceful。

### Invariants

- Graceful → Immediate 升级必须恰好线性化一次，并可与任务启动、取消及 Internal Submission admission 建立唯一顺序。
- 升级线性化点之前获准的 Internal Submission 已被接受；其中尚未进入 `Running` 的任务在升级后适用 D-006，已经 `Running` 的任务适用 D-007。
- 升级线性化点之后不得再接受任何 Internal Submission；External Submission 已由 Graceful Stopping 关闭。
- Immediate 模式不可降级；后续 `shutdown()` 不得恢复任务、撤销 stop request 或重新开放任何 admission。
- 模式升级不改变 D-008 的同步完成边界：`shutdown_now()` 仍等待全部 Worker 退出、join 并发布 `Stopped`。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Running + `shutdown()` | 尚未关停 | 进入 Graceful Stopping，适用 D-002、D-003、D-010 |
| Running + `shutdown_now()` | 尚未关停 | 直接进入 Immediate Stopping，适用 D-006 至 D-009 |
| Graceful Stopping + `shutdown_now()` | 已在 graceful drain | 原子升级为 Immediate，取消未运行任务并请求 Running Task 停止 |
| Immediate Stopping + `shutdown()` | 已采用 Immediate 策略 | 保持 Immediate，不允许降级 |
| 当前 Scheduler Worker | 任一同步关停 API | 先适用 D-009 或 D-011 的无副作用拒绝规则 |

### Rationale

Immediate Shutdown 表达比 Graceful Shutdown 更强且不可逆的停止意图。允许单向升级能让后来的紧急停止请求仍具有实际意义，同时避免取消后恢复、admission 重开或调用顺序决定 API 是否兑现名称承诺。

### Rejected alternatives

- 首个关停模式永久获胜：后来的 `shutdown_now()` 无法兑现其取消未运行任务并请求 Running Task 停止的公共语义。
- Graceful Stopping 期间拒绝 `shutdown_now()`：紧急停止意图无法升级已经开始但可能长期不终结的 graceful drain。
- 允许 Immediate → Graceful 降级：已经取消的任务、已发布的 stop request 和已关闭的 admission 都无法安全撤销。

### Consequences

- 生命周期模型必须区分 Graceful Stopping 与 Immediate Stopping，或在 `Stopping` 外维护等价的单调关停模式。
- 测试必须覆盖 Internal Submission、任务启动与 Graceful → Immediate 升级三者的线性化竞态。
- 多个同步关停调用方如何共享完成通知和 Worker join 所有权，仍需单独决定。

### Non-goals and deferred risks

- 本决策不定义同模式重复调用的返回值或错误表示。
- 本决策不定义多个非 Worker 线程中由谁执行实际 Worker join。
- 本决策不提供取消 Immediate Shutdown 或重新启动同一 Scheduler 的能力。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 `shutdown_now()` 将进行中的 Graceful Shutdown 单向升级为 Immediate Shutdown，并禁止后续降级。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 1029–1107、1645–1650 行仅给出单一 `Stopping` 状态，未定义两种关停模式的竞态；D-002、D-003、D-006 至 D-011 已形成相关行为约束。

### Traceability

- ADR: [ADR-0003](../../docs/adr/0003-immediate-shutdown-monotonically-escalates-graceful-shutdown.md)
- Spec destinations: R-014, R-015
- Tickets: Pending
- Tests: Pending

## D-013 — 并发非 Worker 关停调用共享一次完成过程

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

多个非 Worker 线程可能同时调用 `shutdown()`、`shutdown_now()`，或在 Scheduler 已经 `Stopping` 时再次调用关停 API。若每个调用方都独立 join 同一组 `std::thread`，会形成并发 join 与所有权竞态；若后来的调用只观察到 `Stopping` 就提前返回，则会破坏 D-008 与 D-010 的同步完成边界。

### Decision

在一次进行中的关停过程中，所有非 Worker `shutdown()`/`shutdown_now()` 调用都是同一个关停完成过程的参与者；除 D-012 允许的 Graceful → Immediate 升级外，重复调用是幂等的，并且每个调用都必须等待共同的 `Stopped` 完成事件后才返回。

### Invariants

- 同模式重复调用不得重复关闭 admission、重复取消任务或重复发布 stop request 所导致的可观察副作用。
- `shutdown_now()` 加入 Graceful Stopping 时只按 D-012 执行一次模式升级；其他调用只能观察或等待当前 Shutdown Mode。
- 每个成功返回的非 Worker 关停调用都必须观察到全部 Worker 已退出并 join、Scheduler 已发布 `Stopped`。
- Runtime 必须保证每个 Worker thread 恰好被 join 一次；多个调用方不得并发调用同一个 Worker 的 `join()`。
- 等待中的参与者必须由同一个关停完成事件唤醒，不能依赖轮询队列为空来判断完成。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Graceful Stopping + `shutdown()` | 同模式重复调用 | 幂等加入并等待共同完成 |
| Immediate Stopping + `shutdown_now()` | 同模式重复调用 | 幂等加入并等待共同完成 |
| Graceful Stopping + `shutdown_now()` | 更强模式调用 | 按 D-012 升级一次，然后等待共同完成 |
| Immediate Stopping + `shutdown()` | 更弱模式调用 | 不降级，只加入并等待 Immediate 完成 |
| 当前 Scheduler Worker | 任一关停 API | 仍由 D-009/D-011 在副作用前拒绝，不成为参与者 |

### Rationale

把并发调用建模为一次关停完成过程的多个参与者，可以同时保持所有同步 API 的返回保证、避免重复副作用和并发 join，并让模式升级与完成等待成为两个清晰阶段。调用者无需在 Runtime 外额外串行化关停。

### Rejected alternatives

- 后来的调用观察到 `Stopping` 后立即返回：破坏 D-008 与 D-010 的同步资源安全边界。
- 后来的调用报错：迫使调用方在 Runtime 外自行串行化关停，并使 Graceful → Immediate 升级难以安全表达。
- 每个调用方独立 join：会对同一组 Worker thread 产生并发 join 和所有权竞态。

### Consequences

- Runtime 需要共享的关停完成状态与多等待者通知机制。
- join 协调者的具体身份可以是首个调用方、专用控制路径或其他安全实现，但不得改变公共完成语义。
- 测试必须覆盖大量同模式调用、Graceful → Immediate 升级和全部调用者返回时的状态可见性。

### Non-goals and deferred risks

- 本决策不固定由哪个线程承担实际 join。
- 本决策不定义 Scheduler 已经 `Stopped` 后再次调用关停 API 的行为。
- 本决策不处理关停调用与 Scheduler 对象析构并发发生的未同步生命周期错误。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受所有并发非 Worker 关停调用共享同一次完成过程、幂等参与并等待 `Stopped`，且每个 Worker 只被 join 一次。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 1645–1650 行要求覆盖多线程 submit + shutdown，但未定义多个关停调用方的 join 协调；D-008、D-010 与 D-012 已固定同步完成和模式升级语义。

### Traceability

- ADR: [ADR-0004](../../docs/adr/0004-concurrent-shutdown-callers-share-one-completion.md)
- Spec destinations: R-016
- Tickets: Pending
- Tests: Pending

## D-014 — 非 Worker 析构以 Graceful Shutdown 作为活动 Scheduler 的 RAII 后备

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

调用方可能在未显式调用关停 API 时销毁仍为 `Running` 的 Scheduler。若析构直接销毁内部状态，Worker 会继续访问失效对象；若默认采用 Immediate Shutdown，会意外取消已经接受但尚未运行的任务；若要求必须显式关停并直接终止进程，则 Scheduler 不具备稳健的 RAII 后备。参考设计只要求测试“析构时仍有任务”，没有定义策略。

### Decision

从非 Worker 线程析构 Scheduler 时，若其仍为 `Running`，析构函数必须发起 Graceful Shutdown；若已在 `Stopping`，析构函数必须加入现有 Shutdown Completion 且不得改变当前 Shutdown Mode。析构函数只在全部 Worker join 且 `Stopped` 已发布后完成。

### Invariants

- 析构函数必须是 `noexcept` 的 RAII 边界，不得向调用方传播异常。
- `Running` 状态下的非 Worker 析构必须采用 Graceful，而不是默认取消尚未运行的已接受任务。
- Graceful Stopping 状态下的析构加入当前 Shutdown Completion；Immediate Stopping 状态下同样加入，但不得降级为 Graceful。
- 析构完成时不得仍有 Worker 执行用户代码或访问 Scheduler Runtime；所有 Worker 必须已被 join。
- 若 Drain Work Closure 无法终结，析构可以无限期阻塞；不得 detach Worker 或伪造完成。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 非 Worker 析构 + Running | 活动 Scheduler | 发起 Graceful Shutdown 并同步等待完成 |
| 非 Worker 析构 + Graceful Stopping | 已在 graceful drain | 加入现有 Shutdown Completion，不改变模式 |
| 非 Worker 析构 + Immediate Stopping | 已在 immediate stop | 加入现有 Shutdown Completion，不降级 |
| 非 Worker 析构 + Stopped/未启动 | 无活动 Worker | 不产生任务取消或 stop request |
| 当前 Scheduler Worker 上触发析构 | self-destruction | 不由本决策定义，需单独处理 |

### Rationale

Graceful 析构后备符合资源所有者离开作用域时“完成已接受工作并回收线程”的直觉，也与 D-010 的同步资源安全边界一致。显式 `shutdown_now()` 仍可在析构前选择取消策略；析构本身不应悄悄把未显式选择的 Graceful 语义升级为 Immediate。

### Rejected alternatives

- 析构默认 Immediate Shutdown：会在调用方未明确选择取消语义时，意外取消已经接受但尚未运行的任务。
- 要求必须显式关停，否则析构 fail-fast：削弱 RAII 恢复边界，遗漏显式关停会直接终止进程。
- detach Worker 后让析构返回：破坏 Scheduler 的对象生命周期与 Worker 访问内部状态之间的安全边界。

### Consequences

- 用户可以依赖 RAII 回收 Worker，但必须知道析构可能无限期阻塞。
- 长生命周期或需要有界退出的应用仍应显式选择并调用关停 API。
- 析构内部不可恢复错误的诊断与 fail-fast 细节需在实现和错误策略中单独设计。

### Non-goals and deferred risks

- 本决策不定义同 Scheduler Worker 触发最后一个所有者析构时的行为。
- 本决策不使 Scheduler 对象的并发析构与其他成员调用成为合法用法；调用方仍须保证对象生命周期同步。
- 本决策不提供析构超时或后台 detach 模式。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受非 Worker 析构在 Running 时同步执行 Graceful Shutdown，在 Stopping 时加入现有 Shutdown Completion，并允许因 Drain Work Closure 无法终结而无限期阻塞。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 1645–1650 行要求测试“析构时仍有任务”，但未定义析构策略；D-010、D-012 与 D-013 已固定同步 graceful completion、模式不可降级和共享完成边界。

### Traceability

- ADR: [ADR-0005](../../docs/adr/0005-destructor-uses-graceful-shutdown-as-raii-fallback.md)
- Spec destinations: R-017, R-018
- Tickets: Pending
- Tests: Pending

## D-015 — 同 Scheduler Worker 触发自身析构时在任何副作用前 fail-fast

Status: superseded

Date: 2026-08-25

Supersedes: None

Superseded by: D-017

### Context

任务可能释放 Scheduler 的最后一个强所有者，或直接在该 Scheduler 的 Worker 上删除 Scheduler，使析构函数在自己管理的 Worker 线程中执行。析构函数无法像 D-009/D-011 的关停 API 一样拒绝调用后安全返回：同步等待会产生 self-wait/self-join；继续销毁内部状态会让当前 Worker 在任务返回和 Worker loop 收尾时访问失效内存。若要合法支持此场景，需要把运行状态从 Scheduler 对象中分离为独立的共享生命周期，并引入非 Worker reaper/join 协调者，这会成为贯穿整个 Runtime 的架构承诺。

### Decision

若析构函数检测到当前线程是同一个 Scheduler 的 Worker，且 Runtime 尚未完成 `Stopped`，则该行为属于生命周期契约违例，必须在任何部分销毁或关停副作用之前通过 `std::terminate()` 确定性 fail-fast。Runtime 不为此场景提供静默异步清理、detach 或继续执行语义。

### Invariants

- 检测到同 Scheduler Worker 自身析构后，必须在发布 stop、取消任务、销毁内部状态、detach Worker 或改变 Shutdown Completion 之前调用 `std::terminate()`。
- 不得从析构函数返回并让当前 Worker 继续执行，因为任务返回路径与 Worker loop 仍需要访问 Runtime 状态。
- 不得同步等待自身退出或 join 自身。
- Scheduler 的所有权契约必须要求：至少一个非该 Scheduler Worker 路径上的所有者维持对象生命周期，直至 Shutdown Completion 完成。
- 任务可使用受生命周期约束的非 owning handle 或 weak reference，但不得成为释放 Scheduler 最后一个强所有者的路径。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 同 Scheduler Worker + 活动 Runtime | Worker 触发最后所有者释放或直接 delete | 无副作用地 `std::terminate()` |
| 其他 Scheduler 的 Worker + 目标 Scheduler 析构 | 当前线程不是目标 Scheduler 的 Worker | 视为非 Worker，遵循 D-014 |
| 非 Worker 析构 | 正常 RAII 路径 | 继续遵循 D-014 |
| 已完成 `Stopped` | 不再存在活动的本 Scheduler Worker | 不产生本决策的自身析构场景 |

### Rationale

这是一个必须在“进程级失败”和“重大生命周期架构”之间明确选择的边界。当前项目没有支持 Worker 自持有并异步回收整个 Scheduler 的已确认用例；先采用可诊断、确定性的 fail-fast，可以避免把未定义行为、偶发 UAF 或隐式 detach 伪装成支持。若未来出现真实用例，可通过新 ADR 引入独立 Runtime State 与外部 reaper，并显式取代本决策。

### Rejected alternatives

- 在析构中同步执行 Graceful/Immediate Shutdown：当前 Worker 必须等待或 join 自己，无法完成。
- 析构继续返回或跳过 join：Scheduler 状态可能先于 Worker loop 被销毁，导致 UAF，或由仍 joinable 的 `std::thread` 在更晚位置触发不可控终止。
- 静默 detach 当前或全部 Worker：无法证明 Scheduler 状态在最后一个 Worker 访问结束前仍存活，且违反 D-014 的资源安全边界。
- 默认引入共享 Runtime State 与非 Worker reaper：可以支持该用例，但会显著改变对象模型、错误传播、关停完成与资源所有权；在没有用例驱动前不承担该复杂度。

### Consequences

- 文档和测试必须明确禁止任务成为 Scheduler 最后一个强所有者的释放路径。
- 实现需要可靠识别“当前线程是否为此 Scheduler 的 Worker”，并在析构入口尽早检查。
- 违反契约会确定性终止进程，而不是产生未定义行为或偶发死锁；`std::terminate_handler` 仍可用于项目级诊断。

### Non-goals and deferred risks

- 本决策不支持 Worker 自主销毁整个 Scheduler 的合法用例。
- 本决策不设计 shared Runtime State、reaper thread 或异步 join 服务。
- 本决策不使 Scheduler 析构与其他线程上的成员调用并发发生成为合法用法。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受同 Scheduler Worker 触发自身析构属于生命周期契约违例，并要求在任何部分销毁或关停副作用前通过 `std::terminate()` fail-fast，不提供静默异步清理。
- Code or data evidence: D-009 与 D-011 已确认同 Scheduler Worker 上的显式关停必须在副作用前拒绝；D-014 要求析构完成前 join 全部 Worker。析构无法返回错误，因此 self-destruction 不能复用显式关停 API 的拒绝模型。

### Traceability

- ADR: [ADR-0006](../../docs/adr/0006-worker-self-destruction-fails-fast.md)
- Spec destinations: Excluded — superseded by D-017
- Tickets: Pending
- Tests: Pending

## D-016 — Stopped 后的关停调用是立即返回的幂等 no-op

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-013 规定并发非 Worker 关停调用共享一次 Shutdown Completion，但明确延期了 `Stopped` 发布之后再次调用 `shutdown()` 或 `shutdown_now()` 的行为。通用资源清理路径可能无法知道别的组件是否已经完成关停；若重复调用报错，调用方必须在 Runtime 外进行额外状态检查和串行化，而该检查本身仍可能与关停完成竞态。另一方面，`shutdown_now()` 在 Graceful Shutdown 已经完成后不可能追溯取消已完成任务。

### Decision

Scheduler 已发布 `Stopped` 后，任何线程再次调用 `shutdown()` 或 `shutdown_now()` 都必须作为成功的幂等 no-op 立即返回，不创建新的 Shutdown Completion、不改变历史 Shutdown Mode，也不产生任务取消或 stop request。

### Invariants

- `Stopped` 是关停 API 的吸收状态；重复调用不得让 Scheduler 离开该状态。
- `shutdown_now()` 不得在 Graceful Shutdown 已完成后追溯升级模式、取消结果或改写任务终态。
- 重复调用不得再次 join Worker、重新发布完成通知或创建第二个关停世代。
- 调用与 `Stopped` 发布之间必须存在唯一顺序：发布前线性化的调用按 D-012/D-013 参与现有完成过程；发布后线性化的调用立即返回。
- 幂等 no-op 不代表 Scheduler 可以重新启动；restart 语义不由本决策定义。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| `Stopped` + `shutdown()` | 已完成任意模式关停 | 立即成功返回 |
| `Stopped` + `shutdown_now()` | 已完成任意模式关停 | 立即成功返回，不追溯升级或取消 |
| 调用在线性化顺序上早于 `Stopped` | 仍处于 `Stopping` | 按 D-012/D-013 加入或升级当前过程 |
| 析构观察到 `Stopped` | RAII 清理 | 继续遵循 D-014，不产生关停副作用 |

### Rationale

关停是资源释放操作，采用吸收状态和幂等调用可让多个所有者、清理守卫与错误路径安全地重复请求同一最终状态。`Stopped` 已经证明全部 Worker 被 join 且任务终态完成，此后再制造错误或新的完成过程没有运行时价值。

### Rejected alternatives

- 对重复关停返回错误或抛出异常：迫使调用方执行有竞态的 check-then-shutdown，并降低 RAII 与错误路径的可组合性。
- 重新创建 Shutdown Completion 并再次执行 join：没有可回收资源，还会引入虚假的第二个关停世代。
- 让 `shutdown_now()` 改写先前 Graceful Shutdown 的历史结果：无法追溯取消已完成任务，并破坏任务终态不可逆性。

### Consequences

- 用户可以在多个清理路径中安全重复调用任一关停 API。
- 测试必须覆盖 Graceful 与 Immediate 完成后的两种重复调用，以及调用与 `Stopped` 发布竞态的线性化行为。
- 若未来 API 暴露关停结果，可报告“already stopped”作为观察信息，但不得改变本决策的成功、无副作用语义。

### Non-goals and deferred risks

- 本决策不决定 `Created` 状态上的关停行为。
- 本决策不允许 `Stopped` Scheduler restart，也不决定是否提供独立 restart API。
- 本决策不规定关停 API 的具体 C++ 返回类型。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案，因此确认 `Stopped` 是关停吸收状态，后续 `shutdown()`/`shutdown_now()` 成功、无副作用地立即返回。
- Code or data evidence: D-013 将全部进行中调用汇聚到一个 Shutdown Completion，并明确延期 `Stopped` 后的行为；参考设计把 `Stopped` 画为生命周期终态，但未定义重复关停调用。

### Traceability

- ADR: [ADR-0004](../../docs/adr/0004-concurrent-shutdown-callers-share-one-completion.md)
- Spec destinations: R-019, Open Questions
- Tickets: Pending
- Tests: Pending

## D-017 — Scheduler Handle 与共享 Runtime State 解耦并由非 Worker Reaper 回收

Status: accepted

Date: 2026-08-25

Supersedes: D-015

Superseded by: None

### Context

D-015 曾把同 Scheduler Worker 触发自身析构定义为必须 `std::terminate()` 的生命周期契约违例。项目 owner 现决定合法支持该场景，并接受此前明确延期的架构成本：公开 Scheduler 对象不能再同时承担 Runtime 的唯一生命周期，Worker 也不能负责等待或 join 自己，因此需要独立共享状态与非 Worker 回收路径。

### Decision

公开 Scheduler Handle 的生命周期必须与共享 Runtime State 解耦。同 Scheduler Worker 触发最后一个 Scheduler Handle 析构时，不再 fail-fast；析构路径必须在释放自身所有权之前，把 Runtime State 的强引用安全移交给一个不属于目标 Scheduler Worker 集合的 Reaper。Runtime State 必须存活到所有 Worker 停止访问、线程完成 join 且最终回收完成。

### Invariants

- Scheduler Handle 的销毁不得在任何 Worker 仍可能访问时销毁 Runtime State。
- Worker 自身析构路径不得同步等待或 join 自己，也不得仅因该生命周期场景调用 `std::terminate()`。
- Runtime State 的强所有权必须在线性化的 handoff 中完成移交；不得出现 Handle 已释放而 Reaper 尚未取得所有权的空窗。
- Reaper 的执行上下文不得是目标 Scheduler 的 Worker；线程 join 与 Runtime State 最终销毁必须发生在安全的非目标 Worker 路径上。
- 不得通过 detach Worker、提前发布 `Stopped` 或伪造 Shutdown Completion 来完成回收。
- D-009/D-011 继续有效：同 Scheduler Worker 对公开同步关停 API 的直接调用仍在副作用前被拒绝；Reaper handoff 是不同的内部生命周期路径。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 同 Scheduler Worker 触发最后 Handle 析构 | Runtime 尚未完成回收 | 析构不等待，原子移交 Runtime State 给 Reaper |
| 非 Worker 析构 | 普通 RAII 路径 | 继续遵循 D-014，同步等待 Shutdown Completion |
| 其他 Scheduler 的 Worker 析构目标 Scheduler | 不是目标 Scheduler Worker | 相对目标仍按 D-014 处理 |
| 公开 `shutdown()`/`shutdown_now()` 由目标 Worker 调用 | 显式同步关停 | 继续按 D-009/D-011 拒绝，不转化为 Reaper handoff |

### Rationale

共享 Runtime State 让 Worker 的执行期所有权不依赖公开 Handle；非 Worker Reaper 则提供无法由当前 Worker 自己完成的 join 与最终销毁上下文。该方案以更复杂的对象模型换取 Worker 触发最后 Handle 释放的合法、无死锁、无 UAF 行为，并保留“不 detach、不伪造完成”的资源安全边界。

### Rejected alternatives

- D-015 的确定性 fail-fast：已由项目 owner 撤回，因为项目现在选择合法支持 Worker 自身析构场景。
- Worker 析构后直接销毁 Runtime State：当前 Worker 的任务返回路径和 Worker loop 仍可能访问状态，会产生 UAF。
- Worker 自己同步 join：存在 self-wait/self-join，无法完成。
- detach 后依赖进程退出回收：破坏 Shutdown Completion 与资源回收保证。

### Consequences

- Scheduler 成为面向用户的 Handle，Runtime State 成为具有独立共享生命周期的运行时身份。
- 所有 Worker 入口、任务上下文和关停协调都必须以 Runtime State 的安全存活为前提。
- Worker 上的最后 Handle 析构可以先返回，Runtime 的关停完成与最终销毁随后由 Reaper 协调。
- 实现与测试必须覆盖 handoff 竞争、最后 Worker 退出、只发生一次 join、Reaper 与显式关停并发等场景。

### Non-goals and deferred risks

- 本决策不固定 Reaper 是进程级单例、专用服务线程还是其他非目标 Worker 执行体。
- 本决策不决定 Worker 触发最后 Handle 释放时，在 `Running` 状态下选择 Graceful 还是 Immediate Shutdown。
- 本决策不改变非 Worker 析构的同步语义，也不改变公开关停 API 的 Worker 调用限制。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确撤回 D-015 的 fail-fast 选择，决定引入独立共享 Runtime State 和非 Worker Reaper，以合法支持同 Scheduler Worker 触发自身析构。
- Code or data evidence: D-015 已识别共享 Runtime State 与非 Worker Reaper 是安全支持该场景所需的架构替代；D-014 仍约束非 Worker 析构，D-009/D-011 仍约束公开同步关停 API。

### Traceability

- ADR: [ADR-0007](../../docs/adr/0007-decouple-scheduler-handle-from-runtime-state.md)
- Spec destinations: R-020, R-021
- Tickets: Pending
- Tests: Pending

## D-018 — Worker 最后 Handle 析构在 Running 时请求 Graceful Shutdown

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-017 已确认 Worker 可以把共享 Runtime State 移交给 Reaper，但尚未决定移交发生时的关停模式。若 Runtime 仍为 `Running`，选择 Immediate 会取消已经接受但尚未运行的任务；选择 Graceful 则与 D-014 的析构后备一致，但 Drain Work Closure 无法终结时，Runtime State 与 Reaper 回收会无限期延后。

### Decision

同 Scheduler Worker 触发最后 Scheduler Handle 析构时，若 Runtime 仍为 `Running`，handoff 必须请求 Graceful Shutdown；若已为 `Stopping`，必须保留当前 Shutdown Mode；若已为 `Stopped`，仅安排安全的 join/final reclamation。Worker 析构路径完成所有权移交后立即返回，不等待 Shutdown Completion。

### Invariants

- Worker 析构后备不得在没有显式 `shutdown_now()` 选择的情况下取消尚未运行的已接受任务。
- Graceful handoff 必须继续遵循 D-002 的 Drain Work Closure，允许已经运行的同 Scheduler 任务提交内部派生工作。
- 已处于 Immediate Stopping 时不得降级；已处于 Graceful Stopping 时不得重启或创建第二次关停。
- handoff 与并发关停调用必须存在唯一线性化顺序，并共享 D-013 的同一个 Shutdown Completion。
- Worker 析构返回只表示 Runtime State 已安全移交，不表示 Scheduler 已 `Stopped` 或 Worker 已 join。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Worker 最后 Handle 析构 + `Running` | 尚未开始关停 | 请求 Graceful，移交后立即返回 |
| Worker 最后 Handle 析构 + Graceful Stopping | 已在 drain | 保持 Graceful，加入现有完成过程 |
| Worker 最后 Handle 析构 + Immediate Stopping | 已在取消路径 | 保持 Immediate，不降级 |
| Worker 最后 Handle 析构 + `Stopped` | 工作已终结 | 仅由安全非 Worker 路径完成 join/最终回收 |

### Rationale

析构没有表达“取消已接受任务”的显式意图，因此默认 Graceful 与 D-014 的 RAII 原则一致；Reaper 解决的是执行上下文和生命周期，而不是借机改变关停策略。应用若需要有界退出或取消，应在最后 Handle 只能由 Worker 释放之前，由非 Worker 显式调用 `shutdown_now()`。

### Rejected alternatives

- `Running` 时默认 Immediate：把对象所有权位置意外转化为取消策略，会静默丢弃尚未执行的已接受任务。
- handoff 后继续保持 `Running`：公开 Handle 已消失，Runtime 可能无限接受 Internal Submission 而没有外部所有者负责关停。
- Worker 析构等待 Reaper 完成：重新引入 self-wait，破坏 D-017 的异步 handoff 目的。

### Consequences

- Worker 最后 Handle 析构与非 Worker 析构具有一致的默认 Graceful 意图，但前者异步、后者按 D-014 同步。
- 不终结的任务仍可能使 Runtime State 永久存活；这是 cooperative cancellation 与无强杀边界的直接后果。
- 测试必须区分“handoff 已完成”和“Shutdown Completion 已完成”。

### Non-goals and deferred risks

- 本决策不固定 Reaper 的线程拓扑、全局生命周期或队列结构。
- 本决策不提供 handoff 超时或自动升级为 Immediate 的策略。
- 本决策不改变 D-016 对 `Stopped` 后公开关停调用的候选语义。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 Worker 触发最后 Scheduler Handle 析构时，Running 默认请求 Graceful Shutdown、Stopping 保持当前模式，并在安全 handoff 后立即返回而不等待关停完成。
- Code or data evidence: D-014 已确认非 Worker 析构默认 Graceful；D-012 禁止模式降级；D-017 已确认 Worker 析构通过 Reaper handoff 合法化，但未确定模式。

### Traceability

- ADR: [ADR-0008](../../docs/adr/0008-worker-orphan-handoff-uses-graceful-shutdown.md)
- Spec destinations: R-021, R-022
- Tickets: Pending
- Tests: Pending

## D-019 — Reaper handoff 能力必须在 Worker 启动前建立

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-017/D-018 要求 Worker 析构路径以 `noexcept` 方式把 Runtime State 安全移交给 Reaper。若直到最后 Handle 析构时才创建 Reaper 线程、分配队列节点或注册回收记录，线程创建和内存分配都可能失败；此时析构既不能传播异常，也不能销毁仍被 Worker 访问的状态，只能重新退化为 `std::terminate()`、泄漏或 UAF。这个风险不能留给低概率资源耗尽路径。

### Decision

在 Scheduler 的任何 Worker 启动之前，Runtime 必须建立并预留可用的 Reaper handoff 能力。若建立或预留失败，Scheduler 必须在进入 `Running` 前报告启动失败，且不得留下活动 Worker。进入 `Running` 后，Worker 最后 Handle 析构的 handoff 必须为 `noexcept`、不分配内存且不创建线程，只使用已建立的所有权移交记录和通知能力。

### Invariants

- 不得把 Reaper 初始化、OS thread 创建或可能分配内存的容器扩容推迟到 Worker 析构 handoff。
- Reaper handoff 所需的所有权槽位、队列/注册记录或等价资源必须在第一个 Worker 可运行用户任务前准备完成。
- 准备失败必须表现为 Scheduler 构造或启动失败；失败返回时 Scheduler 不得处于 `Running`，也不得存在需要回收的 Worker。
- 运行期 handoff 不得因常规内存耗尽或线程资源耗尽而失去 Runtime State 所有权。
- handoff 可以使用内部同步，但不得等待 Drain Work Closure、Worker 退出或 Shutdown Completion。
- 正常的显式关停或非 Worker 析构完成后，未使用的 handoff 能力必须可安全注销或回收，不得永久持有 Runtime State。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Scheduler 建立 Reaper 能力成功 | Worker 尚未启动 | 允许继续进入 `Running` |
| 建立、注册或预留失败 | Worker 尚未启动 | 启动失败，不创建活动 Worker |
| Worker 最后 Handle 析构 | 已处于运行期 | 无分配、无线程创建地完成 handoff |
| 正常 Shutdown Completion | 未使用异步回收路径 | 注销/释放预留能力，不延长 Runtime State 生命周期 |

### Rationale

把可失败的资源获取提前到 Worker 启动前，可以在仍有正常错误通道时报告失败，并使最危险的 `noexcept` 析构路径只执行预先准备好的状态转换与所有权移交。代价是每个可运行 Scheduler 都要承担少量预留或注册成本，即使最终从未发生 Worker 自身析构。

### Rejected alternatives

- 第一次 Worker handoff 时惰性创建 Reaper：`std::thread` 创建可能抛出，析构路径没有安全恢复手段。
- handoff 时向普通动态队列插入并允许扩容：内存分配失败会打破 D-017 的强所有权连续性。
- handoff 失败时退回 D-015 的 `std::terminate()`：会让已经决定合法支持的场景在资源压力下变成进程级失败，公共语义不稳定。
- handoff 失败时泄漏 Runtime State：隐藏资源失控，且无法满足可验证的最终回收语义。

### Consequences

- Scheduler 的启动事务需要把 Reaper 能力建立纳入“全部成功才发布 Running”的原子边界。
- 测试必须注入注册失败、线程创建失败和预留失败，证明没有 Worker 泄漏且没有发布 `Running`。
- 具体使用进程级服务、预注册记录还是其他机制仍可选择，但必须满足运行期 handoff 的无分配、无线程创建保证。

### Non-goals and deferred risks

- 本决策不固定 Reaper 是进程级单线程服务还是其他拓扑。
- 本决策不规定 Scheduler 启动失败的具体 C++ 返回类型或异常类型。
- 本决策不要求 handoff lock-free，只要求它不依赖可失败的资源获取且不等待关停完成。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受在任何 Worker 启动前建立并预留 Reaper handoff 能力；准备失败不得进入 Running，运行期 Worker 析构 handoff 必须 noexcept、不分配内存且不创建线程。
- Code or data evidence: D-014 已要求析构为 `noexcept` RAII 边界；D-017 要求所有权无空窗移交；标准线程创建与动态分配均存在失败通道，因此不能首次发生在 Worker 析构 handoff。

### Traceability

- ADR: [ADR-0009](../../docs/adr/0009-establish-reaper-handoff-before-workers-start.md)
- Spec destinations: R-023, R-024, Open Questions
- Tickets: Pending
- Tests: Pending

## D-020 — Reaper 仅对 Join Ready Runtime 执行 join

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-017 要求 Reaper 接管失去最后 Scheduler Handle 的 Runtime State，D-018 又允许 Graceful Drain 无限期延长。若共享 Reaper 在 handoff 后立即同步等待该 Runtime 的活动任务或 Drain Work Closure，一个不协作终结的 Runtime 会造成 head-of-line blocking，使其他已经可以回收的 Scheduler 也无法完成 join 和 `Stopped` 发布。

### Decision

Reaper 可以在 handoff 时立即持有 Pending Runtime State，但不得阻塞等待其活动任务或 Drain Work Closure。只有当 Runtime 单调进入 `Join Ready`——全部 Worker loop 已不可逆地进入终止收尾路径、不再执行或调度用户任务，且无需等待 Reaper 才能返回——Reaper 才能为其认领 join 协调权并执行 join、发布 `Stopped` 与最终回收。

### Invariants

- `Join Ready` 必须是单调边界；Runtime 一旦进入，不得再次执行用户任务、接受提交或回到 Worker 调度循环。
- Worker 必须能够在没有 Reaper 响应的情况下完成退出收尾；不得让 Worker 等待 Reaper 先 join，形成循环等待。
- Pending Runtime 可以因不终结的任务而无限期保留，但不得阻塞 Reaper 处理其他 Join Ready Runtime。
- handoff 与 Join Ready 通知都必须继续满足 D-019 的无分配、无线程创建运行期保证。
- Reaper 与非 Worker 同步关停调用竞争 join 协调权时，只能有一个路径执行 join；其他参与者观察同一个 Shutdown Completion，继续遵循 D-013。
- `Stopped` 仍只能在全部 Worker 实际 join 后发布；Join Ready 本身不是 Shutdown Completion。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 已 handoff、尚有活动任务 | Pending Runtime | Reaper 只持有状态，不等待、不 join |
| Drain Work Closure 永不终结 | 永久 Pending | 保留 Runtime State，但不阻塞其他回收 |
| 全部 Worker 进入终止收尾 | Join Ready Runtime | Reaper 可认领并执行 join |
| 已有非 Worker join 协调者 | 并发同步关停 | Reaper 不重复 join，只观察共同完成 |

### Rationale

把“保持 Runtime State 存活”和“执行阻塞 join”拆成 Pending 与 Join Ready 两个阶段，可以让一个共享 Reaper 同时管理多个 Runtime，而不会被任一不合作任务占住。代价是 Runtime State 需要额外的单调就绪状态、预注册通知和 join 协调权仲裁。

### Rejected alternatives

- handoff 后由 Reaper 立即调用同步 `shutdown()`：Graceful drain 可能无限阻塞，造成所有回收任务排队停滞。
- Reaper 轮询每个 Runtime 是否退出：引入无界延迟或持续 CPU 消耗，且仍需要安全生命周期通知。
- Join Ready 时提前发布 `Stopped` 再异步 join：破坏 D-008/D-010/D-013 已确认的资源完成边界。
- 为每个 Pending Runtime 创建一个阻塞 Reaper 线程：避免 head-of-line blocking，但产生无界辅助线程，并与 D-019 的运行期禁止创建线程相冲突。

### Consequences

- Runtime State 需要 Pending/Join Ready 的可观察回收阶段，但它们不替代公开 SchedulerState。
- Reaper 可以对 Pending 数量、等待时长与 Join Ready 延迟提供 Metrics/Trace。
- 测试必须构造一个永久不终结 Runtime 与多个可终结 Runtime，证明后者仍能完成 join 和回收。

### Non-goals and deferred risks

- 本决策不固定 Reaper 使用一个还是多个协调线程，也不决定其进程级生命周期。
- 本决策不规定 Join Ready 通知的具体队列、原子变量或同步原语。
- 本决策不为永久 Pending Runtime 提供超时强杀或自动 Immediate 升级。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 Reaper 在 handoff 后只持有 Pending Runtime State，不等待活动任务；仅在 Runtime 单调进入 Join Ready 后执行 join，永久 Pending Runtime 不得阻塞其他 Scheduler 回收。
- Code or data evidence: D-018 明确允许无法终结的 Graceful handoff 无限期延后回收；D-013 要求每个 Worker 只 join 一次且共享完成；因此 Reaper 不能通过串行阻塞等待 Pending Runtime 来协调多个回收。

### Traceability

- ADR: [ADR-0010](../../docs/adr/0010-reaper-joins-only-join-ready-runtimes.md)
- Spec destinations: R-025, R-026
- Tickets: Pending
- Tests: Pending

## D-021 — Reaper 使用进程级单协调线程服务

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-019 已把 Reaper 能力建立提前到 Worker 启动前，D-020 又确保 Reaper 不会阻塞等待 Pending Runtime 的活动任务，因此多个 Scheduler 可以安全共享一个回收协调者。现在需要决定是为每个 Scheduler 预留独立 Reaper thread、按 handoff 创建辅助线程，还是使用进程级服务。前两者会让线程数量随 Scheduler 或孤儿 Runtime 数量增长，并使 Reaper 自身生命周期更难回收。

### Decision

AstraScheduler 在一个进程内使用一个逻辑 Reaper Service，由恰好一个不属于任何 Scheduler 的专用协调线程驱动。服务在第一个 Scheduler 启动事务中建立并验证，后续 Scheduler 在 Worker 启动前向其预注册 handoff 能力；全部 Pending Runtime State 与 Join Ready 通知由该服务统一协调，不为单个 Scheduler 或单次 handoff 创建额外 Reaper thread。

### Invariants

- Reaper coordinator thread 不得计入任何 Scheduler 的 Worker 数量，不得执行用户任务、参与 work stealing 或成为 Internal Submission 上下文。
- 第一个 Scheduler 只有在 Reaper Service 建立成功后才能启动 Worker；后续 Scheduler 只有在预注册成功后才能启动 Worker，继续遵循 D-019。
- 一个进程内不得因 Scheduler 数量或 Pending Runtime 数量增加而自动增加 Reaper thread。
- coordinator 必须按 D-020 只对 Join Ready Runtime 执行 join；Pending Runtime 不得占住协调线程。
- 每个 Runtime 的 join 协调权仍只能被认领一次；Reaper Service 不得与同步关停调用重复 join。
- Reaper Service 的内部故障不得被当作某个 Scheduler 的用户任务异常传播。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 进程内第一个 Scheduler 启动 | Reaper 尚未建立 | 先建立单协调线程服务，失败则启动失败 |
| 后续 Scheduler 启动 | Reaper 已运行 | 只预注册 handoff 能力 |
| 多个 Pending Runtime | 未达 Join Ready | 单服务持有并继续处理其他通知 |
| 多个 Join Ready Runtime | 可回收队列 | 由单协调线程依次认领、join、完成回收 |

### Rationale

D-020 已消除不终结任务带来的主要 head-of-line blocking，因此回收工作只剩少量线程 join、状态发布和引用释放；单协调线程足以承担这一低频控制面工作，同时把辅助线程数量固定为每进程一个，并为 Runtime Metrics 与 Chrome Trace 提供统一观察点。

### Rejected alternatives

- 每个 Scheduler 一个预启动 Reaper thread：即使从不发生 Worker 析构也永久增加一条线程，且 Reaper 自身的所有权与回收仍需额外协调。
- 每次 handoff 创建并 detach 一个 Reaper thread：违反 D-019 的运行期禁止线程创建保证，并产生无界、不可 join 的辅助线程。
- 使用任意 Scheduler Worker 代替专用协调线程：可能重新引入目标 Worker self-join，或用一个 Scheduler 的业务执行容量阻塞回收另一个 Scheduler。
- 多线程 Reaper pool：在 Join Ready 隔离下缺乏已确认吞吐需求，却增加 join 协调、关闭和全局生命周期复杂度。

### Consequences

- 进程运行 AstraScheduler Worker 时通常额外存在一条 Reaper coordinator thread。
- 多个 Runtime 的最终 join 串行执行；若 Join Ready 定义被错误实现、线程退出收尾卡住，仍可能影响后续回收，因此需要 watchdog metrics 与压力测试。
- Reaper Service 成为进程级内部基础设施；退出顺序由 D-023 至 D-037 固定，静态析构与动态库卸载边界由 D-038 固定。

### Non-goals and deferred risks

- 本决策不决定 Reaper Service 在最后一个 Scheduler 回收后退出还是保持到进程结束。
- 本决策本身不决定永久 Pending Runtime 的进程退出策略；D-027 至 D-029 后续固定等待、超时观察与显式升级，D-038 保留调用方终止进程的最终选择。
- 本决策不固定 Pending/Join Ready 队列使用 intrusive list、MPSC queue 或其他同步原语。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受进程内使用一个由单专用非 Worker 线程驱动的 Reaper Service，所有 Scheduler 预注册并共享它，不为每个 Scheduler 或每次 handoff 创建额外 Reaper thread。
- Code or data evidence: D-019 要求 handoff 前建立能力且禁止运行期创建线程；D-020 保证 Pending Runtime 不阻塞 Reaper，因此单一共享协调线程具备可行性。

### Traceability

- ADR: [ADR-0011](../../docs/adr/0011-use-one-process-wide-reaper-coordinator.md)
- Spec destinations: R-027
- Tickets: Pending
- Tests: Pending

## D-022 — Reaper Service 空闲时保持存活而不自动停启

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-021 引入了进程级单协调线程。若最后一个注册 Runtime 离开时自动销毁 Reaper Service，最后一次回收可能正由 coordinator thread 自己执行，使服务销毁面临 self-join；同时，新 Scheduler 注册可能与“最后一个注销触发停止”竞态，要求复杂的 Starting/Running/Stopping/Restarting 全局状态机。另一选择是接受一条低成本空闲线程，直到独立的进程级终结阶段。

### Decision

Reaper Service 在首次成功建立后，不因当前注册 Runtime、Pending Runtime 或 Join Ready 队列变为空而自动停止。无工作时 coordinator 必须进入阻塞空闲等待；后续 Scheduler 复用同一服务和线程。服务只在另行定义的进程级显式终结阶段退出，不由单个 Scheduler 的 shutdown、析构或最后一次回收隐式触发。

### Invariants

- 最后一个 Runtime 注销或回收不得要求 Reaper coordinator join 或销毁自己。
- 空闲 Reaper Service 不得继续强持有已完成 Runtime State，也不得 busy-spin 或周期轮询。
- 新 Scheduler 注册到已建立的空闲服务时，不得重新创建 coordinator thread。
- 单个 Scheduler 的生命周期不得决定进程级 Reaper Service 的终止。
- 从首次成功建立到显式终结之间，D-019 所需的 Reaper 执行能力必须持续可用。
- 显式终结的调用权限、Pending Runtime 策略和动态库卸载行为由后续 D-023 至 D-039 固定，不属于本决策的空闲生命周期规则。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 有 Pending/Join Ready Runtime | 服务有工作 | 按 D-020 处理通知与 join |
| 注册表与队列全部为空 | 服务空闲 | coordinator 阻塞等待，线程保留 |
| 后续 Scheduler 启动 | 服务已经空闲 | 复用同一线程并预注册 |
| 单个/最后 Scheduler shutdown | Scheduler 生命周期结束 | 不触发 Reaper Service 停止 |

### Rationale

Reaper 是进程级基础设施而非某个 Scheduler 的子资源。保持服务空闲存活，用一条休眠线程换取简单、无 self-join 的所有权模型和稳定的 handoff 可用性；这也避免高频创建/销毁 Scheduler 时反复创建 OS thread。

### Rejected alternatives

- 注册计数归零后由 coordinator 自动销毁服务：最后一次回收可能发生在 coordinator 自身，无法 join 自己。
- 注册计数归零后由任意注销线程同步停止：与并发注册存在 stop/start 竞态，并可能让非 Worker Scheduler 析构承担额外全局阻塞。
- 空闲超时后停止并允许重启：增加时间相关竞态和测试不确定性，收益仅是回收一条休眠线程。
- 每次创建 Scheduler 都重建 Reaper：破坏 D-021 的进程级共享模型并增加线程创建失败面。

### Consequences

- 一旦进程成功启动过 AstraScheduler Worker，通常会保留一条休眠 Reaper coordinator thread，直到显式终结。
- Scheduler 反复创建和销毁不会产生 Reaper thread churn。
- 必须设计独立的进程级 Reaper Service finalization，而不能依赖最后一个 Scheduler 析构。

### Non-goals and deferred risks

- 本决策不规定由谁调用进程级显式终结，也不规定它是否是公开 API。
- 本决策本身不决定显式终结遇到永久 Pending Runtime 的行为；D-027/D-028 后续固定无界 wait 与不伪造完成的 TimedOut。
- 本决策不决定静态析构、`atexit`、动态库卸载或 `fork()` 语义。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 Reaper Service 首次建立后在无 Scheduler 或 Pending Runtime 时保持阻塞空闲，不自动停止或重建，只由后续单独定义的进程级显式终结阶段退出。
- Code or data evidence: D-021 已确认单一进程级 coordinator；其处理最后一个 Runtime 回收时可能成为最后资源释放路径，自动停止会引入 self-join 与并发重启协调问题。

### Traceability

- ADR: [ADR-0012](../../docs/adr/0012-keep-reaper-service-alive-while-idle.md)
- Spec destinations: R-028
- Tickets: Pending
- Tests: Pending

## D-023 — Reaper Finalization 永久关闭新的 Scheduler 注册

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-022 要求 Reaper Service 只在独立的进程级终结阶段退出。若终结过程中仍允许新 Scheduler 注册，coordinator 可能在新 Runtime 已取得 handoff 承诺后退出；若允许终结完成后自动重启，则终结不再是可靠的进程资源边界，并重新引入 D-022 避免的 stop/restart 状态机。必须给 Scheduler 启动与 Reaper Finalization 建立唯一顺序。

### Decision

进程级 Reaper Finalization 是一次不可逆的状态转换。其线性化点必须把 Reaper Service 从可注册状态切换到 `Finalizing`，永久关闭新的 Scheduler Runtime 注册；与之竞态的启动在该点之前成功注册则被纳入本次终结核算，在该点之后则必须在创建任何 Worker 前失败。服务达到 `Finalized` 后，本进程不得重建 Reaper Service 或启动新的 Scheduler Worker。

### Invariants

- 注册关闭与 Scheduler 启动之间必须有唯一线性化顺序，不得出现“启动报告成功但没有可用 Reaper handoff 能力”的 Runtime。
- 在关闭前已成功预注册的 Runtime 必须继续拥有 D-019 保证的 handoff 能力，并被 Finalization 核算。
- 在关闭后尝试启动的 Scheduler 必须失败且不得创建活动 Worker。
- `Finalizing` 与 `Finalized` 都不得重新开放注册；进程内不存在 Reaper restart。
- Finalization 不得通过撤销已注册 Runtime 的 handoff 能力来加速完成。
- Finalization 的等待、Pending Runtime 处理、caller eligibility 与公共 Interface 由后续 D-024 至 D-039 固定。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Scheduler 注册早于 Finalization 线性化点 | 已取得能力 | 启动可继续，并被终结过程核算 |
| Scheduler 注册晚于线性化点 | 注册已关闭 | 在任何 Worker 创建前失败 |
| Reaper Service 已 `Finalizing` | 终结进行中 | 拒绝全部新注册 |
| Reaper Service 已 `Finalized` | 进程级终态 | 不允许重建或启动新 Scheduler |

### Rationale

把 Finalization 定义为永久关闭进程级运行时准入，可以让 coordinator 退出建立在一个有限、不会继续增长的 Runtime 集合上。一次性终态也让动态库卸载、测试进程收尾和资源审计具有明确边界，而不必支持危险且需求不明的全局 restart。

### Rejected alternatives

- Finalization 期间继续接受 Scheduler：终结集合持续增长，coordinator 退出与 handoff 承诺可能竞态。
- Finalized 后按需重建 Reaper Service：需要重新引入进程级 stop/restart 协议，并让旧静态对象或 Runtime token 与新世代混淆。
- 先停止 coordinator、后关闭注册：存在新 Runtime 注册到已失去执行线程的 Reaper Service 的窗口。
- 仅建议调用方自行避免并发启动：无法形成可测试的 Runtime 内部安全保证。

### Consequences

- Reaper Finalization 是进程级 one-shot 操作，通常只能在应用或测试进程的最后阶段执行。
- Finalization 开始后，仍存活但尚未启动的 Scheduler Handle 可能无法进入 `Running`。
- 测试若需要多轮全局初始化，应使用独立进程，而不是依赖 Reaper restart。

### Non-goals and deferred risks

- 本决策本身不决定永久 Pending Runtime 的等待结果；D-027/D-028 后续允许 wait 无界阻塞并让 wait_for 返回 TimedOut。
- 本决策不决定 concurrent Finalization 调用是否共享完成或如何报告结果。
- 本决策不决定是否自动从 `atexit`/静态析构触发，也不规定动态库卸载 API。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 Reaper Finalization 线性化地永久关闭 Scheduler 注册；此前注册的 Runtime 纳入终结核算，此后启动的 Scheduler 在创建 Worker 前失败，且本进程不支持 Reaper restart。
- Code or data evidence: D-019 要求每个 Running Runtime 始终具备 Reaper handoff 能力；D-021/D-022 将该能力集中到持续存活的进程级服务，因此服务终结必须先原子关闭新注册。

### Traceability

- ADR: [ADR-0013](../../docs/adr/0013-reaper-finalization-permanently-closes-registration.md)
- Spec destinations: R-029
- Tickets: Pending
- Tests: Pending

## D-024 — Reaper Finalization 对已纳入 Runtime 请求 Graceful Shutdown

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-023 使 Finalization 线性化点之前注册的 Runtime 构成有限核算集合，但尚未决定它们的关停模式。默认 Immediate 会因进程级基础设施终结而静默取消已接受任务；仅关闭 Reaper 注册而不请求 Runtime 关停，则活动 Runtime 永远不会达到 Join Ready，Finalization 无法完成。还需覆盖已经注册但尚在启动事务中的 Runtime，避免其在全局准入关闭后短暂接受新工作。

### Decision

Reaper Finalization 必须对核算集合中的每个 Runtime 发出 Graceful Shutdown 请求。`Running` Runtime 转入 Graceful Stopping；已经 `Stopping` 的 Runtime 保持当前 Shutdown Mode，Immediate 不得降级；已经 `Stopped` 的 Runtime 不改变任务终态。已注册但尚未发布 `Running` 的启动事务必须观察一个 sticky Graceful request，并在任何用户任务或 External Submission 可运行前回滚启动，或直接进入不开放外部准入的 Graceful Stopping。

### Invariants

- Finalization 本身不得把尚未运行的已接受任务直接终结为 Cancelled；取消只来自已经存在或并发线性化的 Immediate Shutdown。
- Graceful 请求必须继续遵循 D-002/D-003 的 Drain Work Closure 与 External Submission 关闭边界。
- 已处于 Immediate Stopping 的 Runtime 不得降级，已处于 Graceful Stopping 的 Runtime 不得创建第二个关停过程。
- 并发 `shutdown_now()` 可以按 D-012 将 Finalization 发起的 Graceful 单向升级为 Immediate。
- Finalization 前已注册但仍在启动中的 Runtime 不得在未观察终结请求的情况下发布一个可接受 External Submission 的 `Running` 窗口。
- Scheduler Handle 可以在 Finalization 后继续存在，但对应 Runtime 一旦进入 Stopping/Stopped，提交必须遵循既有拒绝规则。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 已注册 + `Running` | Finalization 核算集合 | 请求 Graceful Shutdown |
| 已注册 + Graceful Stopping | 已在 drain | 保持 Graceful，共享现有完成 |
| 已注册 + Immediate Stopping | 已在取消路径 | 保持 Immediate，不降级 |
| 已注册 + `Stopped` | 已完成关停 | 不改写任务终态，仅等待/完成注销核算 |
| 已注册 + 启动事务未完成 | 尚未开放用户执行 | 观察 sticky Graceful，回滚或直接进入关闭准入的 stopping 路径 |

### Rationale

Finalization 是进程级资源终结，而不是取消意图；默认 Graceful 与 Scheduler 析构及 Worker orphan handoff 的既有策略一致。对启动中的 Runtime 使用 sticky request，可以保留 D-023 的线性化顺序，同时消除“全局已 Finalizing、局部却短暂 Running 并接收外部任务”的窗口。

### Rejected alternatives

- 对全部 Runtime 默认 Immediate：把基础设施终结隐式转化为任务取消，可能丢弃已接受工作。
- 只关闭新 Scheduler 注册、不触发已有 Runtime 关停：活动 Runtime 无法达到 Join Ready，Reaper coordinator 无法安全退出。
- 强制把已有 Immediate 降级为 Graceful：违反 D-012 的模式单调性，并可能恢复已取消任务。
- 允许已注册的 Starting Runtime 正常发布 Running 后再异步请求 Graceful：产生可接受新 External Submission 的竞态窗口。

### Consequences

- Finalization 会影响所有关闭前已注册的 Scheduler，即使其 Handle 仍被应用持有。
- 不终结的任务仍可能让某个 Runtime 永久 Pending；D-026 至 D-029 后续固定 split-phase 等待、超时与显式升级策略。
- Scheduler 启动事务需要能观察并处理进程级 sticky Finalization request。

### Non-goals and deferred risks

- 本决策不决定 Finalization API 是否同步等待全部 Runtime 与 Reaper coordinator 退出。
- 本决策不提供全局 `finalize_now()` 或超时后自动升级 Immediate 的策略。
- 本决策不固定 Starting Runtime 选择完整回滚还是进入无外部准入的短暂 Graceful Stopping，只要求不开放用户工作窗口。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 Reaper Finalization 对关闭前纳入核算的全部 Runtime 请求 Graceful Shutdown，保留已有 Immediate 模式，并要求启动中的 Runtime 在开放任何用户工作前观察终结请求。
- Code or data evidence: D-014/D-018 已把析构后备固定为 Graceful；D-012 要求模式只可升级不可降级；D-023 确定了 Finalization 的有限 Runtime 核算集合。

### Traceability

- ADR: [ADR-0014](../../docs/adr/0014-reaper-finalization-requests-graceful-shutdown.md)
- Spec destinations: R-030
- Tickets: Pending
- Tests: Pending

## D-025 — Reaper Finalization 同步等待全部 Runtime 与 coordinator 完成

Status: rejected

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-023/D-024 已定义 Finalization 的有限核算集合与 Graceful 请求，但尚未定义 API 返回时资源是否真正结束。若只发出请求后立即返回，调用方无法判断 Worker、Runtime 注册记录和进程级 coordinator 是否仍在运行，也不能建立可靠的测试收尾或库卸载前置边界；若遇到不终结任务就返回“busy”，进程已永久关闭注册却仍需调用方自行拼接完成等待协议。

### Decision

已否决方案：把 Reaper Finalization 暴露为一个既发起终结、又同步等待全部 Runtime 与 coordinator 完成的单体调用。该方案要求调用方在开始 Finalization 时就接受无界阻塞，无法先进行有界观察并决定继续等待、显式升级或终止进程。

### Invariants

- Finalization 返回时不得仍有 AstraScheduler Worker 或 Reaper coordinator thread 存活。
- 每个纳入 Runtime 必须完成其既有 Shutdown Completion；不得用进程级 `Finalized` 代替或提前满足 Runtime 级完成。
- Finalization 返回前，Reaper Service 不得继续持有 Pending/Join Ready Runtime State 或已注销的 handoff 记录。
- 已停止 Runtime 对应的 Scheduler Handle 可以仍然存活；这不允许 Worker 或 Reaper 注册继续存在，也不意味着 Handle 可再次启动 Runtime。
- 不合作任务可以使调用永久阻塞；不得以超时、强杀、detach 或伪造 `Stopped` 作为隐式后备。
- 并发 `shutdown_now()` 仍可按 D-012 升级单个 Runtime，但 Finalization 自身不得主动升级。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 全部 Runtime 可终结 | 正常 Finalization | 等待全部完成，join coordinator，发布 `Finalized` 后返回 |
| 至少一个 Runtime 永久 Pending | 不合作任务 | 调用可无限期阻塞，服务继续处理其他 Runtime |
| Scheduler Handle 仍存在但 Runtime 已 Stopped | 无活动执行资源 | 不阻止 Finalization 完成；Handle 只能观察终态/按既有规则拒绝操作 |
| 某 Runtime 并发升级 Immediate | 外部显式取消 | 等待升级后的同一 Shutdown Completion |

### Rationale

Finalization 是进程级资源安全边界，只有同步等待才能让“返回”具有单一、可测试含义。允许无限阻塞与 Scheduler 级同步 shutdown、cooperative cancellation 及无强杀边界保持一致；需要有界退出的应用必须在调用 Finalization 前显式对相关 Scheduler 使用 `shutdown_now()` 并确保 Running Task 响应停止请求。

### Rejected alternatives

- split-phase `begin_finalization()` + `wait()`/`wait_for()`：已被项目 owner 选中，见 D-026、D-027、D-028。
- 遇到活动/Pending Runtime 就返回 busy：注册已经永久关闭，调用方还需维护第二套重试/等待协议，且容易遗漏 coordinator join。
- 内置超时后自动 Immediate：未经调用方明确选择就改变任务终态和取消语义。
- 超时后 detach/泄漏：破坏 D-017/D-020 的资源与状态完成保证。

### Consequences

- Finalization 适合应用主控线程、测试 harness 或受控卸载路径，不适合延迟敏感线程。
- API 文档必须突出“可能无限期阻塞”，并提供 Pending Runtime metrics/trace 帮助诊断。
- 安全卸载还要求应用不再持有或调用 Scheduler Handle；Finalization 只保证执行线程与进程级 Reaper 基础设施已结束。

### Non-goals and deferred risks

- 本决策不决定哪些线程具备 Finalization 调用资格。
- 本决策不决定多个并发 Finalization 调用是否共享完成及具体返回类型。
- 本决策不提供带 timeout 的 Finalization 变体。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确否决把 Reaper Finalization 设计成单个同步、可能无限阻塞的调用，改为 begin/wait/wait_for 分阶段协议。
- Code or data evidence: D-008/D-010/D-014 已采用同步、可无限阻塞的资源完成边界；D-020 要求 Runtime 到 Join Ready 后实际 join 才能发布 Stopped；D-023/D-024 定义了 Finalization 的有限集合与关停模式。

### Traceability

- ADR: Pending
- Spec destinations: Excluded — rejected in favor of D-026 through D-028
- Tickets: Pending
- Tests: Pending

## D-026 — begin_finalization 永久关闭注册并异步启动 Graceful Finalization

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-025 的单体同步调用无法让调用方先发起进程级终结，再以有界方式观察进度并选择后续策略。项目 owner 要求把“开始不可逆终结”与“等待真正完成”拆开，同时保留 D-023 的永久准入关闭和 D-024 的 Graceful 默认语义。

### Decision

`begin_finalization()` 必须线性化地永久关闭 Scheduler Runtime 注册入口，对关闭前已纳入核算的 Runtime 启动 D-024 定义的 Graceful Finalization，并在终结请求已被可靠记录、Reaper coordinator 已被通知后立即返回；它不得等待 Runtime 达到 Shutdown Completion、Join Ready、`Stopped` 或进程级 `Finalized`。

### Invariants

- `begin_finalization()` 返回只表示 Finalization 已不可逆地开始，不表示任何 Runtime 或 Reaper Service 已完成。
- 注册入口一旦关闭必须保持关闭；begin 返回后不得恢复注册或重启 Reaper。
- 关闭前已注册的 Runtime 必须收到或能够观察 sticky Graceful request，继续遵循 D-024。
- begin 返回前必须可靠建立后续 `wait()`/`wait_for()` 可观察的同一个 Finalization Completion。
- begin 不得等待 Drain Work Closure、Worker 退出、join 或 coordinator 退出。
- 重复/并发 begin 调用的幂等性、返回类型与控制对象形态由后续 D-030 至 D-032/D-039 固定。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 尚可注册 + 首次 begin | 正常开始 | 关闭注册、记录 Graceful 请求、通知 coordinator 后返回 |
| 核算集合为空 | 没有 Runtime | 仍永久关闭注册；完成可由后台快速达成 |
| 存在活动/Pending Runtime | 终结需继续 | begin 立即返回，后台继续工作 |
| Runtime 已 Immediate Stopping | 已显式升级 | 保持 Immediate，不降级 |

### Rationale

split-phase 开始操作把不可逆的全局状态转换做得短小明确，同时把无界等待移到调用方显式选择的观察方法中。调用方因此可以组织超时日志、升级策略或进程终止，而 Runtime 仍保持单一 Finalization Completion。

### Rejected alternatives

- D-025 的 begin-and-block 单体调用：无法在开始终结后进行有界观察和策略选择。
- begin 仅关闭注册但不启动 Graceful：活动 Runtime 无法主动走向 Join Ready 和完成。
- begin 超时或返回后重新开放注册：违反 D-023 的进程级一次性终结边界。

### Consequences

- 调用方需要在 begin 后显式选择 `wait()`、`wait_for()` 或其他终结策略。
- begin 的低延迟返回不削弱最终资源完成保证；完成含义由 D-027/D-028 统一观察。
- 进程级 Finalization 控制面成为一个深模块：短接口隐藏 Runtime 枚举、模式请求、Reaper 通知与完成聚合。

### Non-goals and deferred risks

- 本决策本身不固定 begin 返回形态；D-030/D-039 后续选择 FinalizationControl。
- 本决策本身不定义显式 Immediate 升级；D-029/D-039 后续固定 `request_immediate()`。
- 本决策本身不定义 caller eligibility；D-033 后续允许任意应用线程调用 begin。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确要求 `begin_finalization()` 永久关闭 Scheduler 注册入口、启动 Graceful Finalization 后立即返回。
- Code or data evidence: D-023 已定义永久关闭注册；D-024 已定义已纳入 Runtime 的 Graceful 请求与模式保留。

### Traceability

- ADR: [ADR-0015](../../docs/adr/0015-finalization-is-split-phase.md)
- Spec destinations: R-031
- Tickets: Pending
- Tests: Pending

## D-027 — Finalization wait 仅在真实完成后返回且可无限阻塞

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

`begin_finalization()` 按 D-026 立即返回，因此需要一个无超时的等待操作表达最终资源安全边界。该操作不能把“请求已发出”“注册已关闭”或“部分 Runtime 已完成”误当作进程级终结完成。

### Decision

Finalization `wait()` 只有在核算集合中的每个 Runtime 都达到 Shutdown Completion 并解除 Reaper 注册、全部 Pending/Join Ready 工作清空、Reaper coordinator 退出并被 join，且进程级 `Finalized` 已发布后才返回。任一 Runtime 无法终结时，`wait()` 可以无限期阻塞。

### Invariants

- `wait()` 返回时不得仍有 AstraScheduler Worker 或 Reaper coordinator thread 存活。
- `wait()` 不得主动改变 Shutdown Mode、自动升级 Immediate、detach 或伪造完成。
- `wait()` 必须观察 D-026 建立的同一个 Finalization Completion，不得创建新的终结世代。
- Scheduler Handle 可以仍然存活，但其 Runtime 必须已 Stopped 且不再注册于 Reaper Service。
- 调用线程资格由 D-034 固定，多个 wait 调用方与唯一 coordinator join 由 D-031/D-037 固定。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Finalization 已完成 | Completion 已发布 | 立即返回 |
| Finalization 进行中且可终结 | 正常等待 | 阻塞到真实完成后返回 |
| 存在永久 Pending Runtime | 不合作任务 | 可以无限期阻塞 |

### Rationale

无超时 `wait()` 提供最强、最简单的完成保证，适合应用最终收尾、测试 harness 和受控卸载前置步骤；是否愿意无界等待由调用方在 split-phase 接口中明确选择。

### Rejected alternatives

- Runtime 尚未完成时提前返回：破坏 Finalization Completion 的资源安全含义。
- wait 内部自动升级或强杀：改变调用方未明确选择的任务终态。
- wait 返回 busy：把等待循环和完成聚合泄漏给所有调用方。

### Consequences

- 调用方可在确认愿意无界等待时使用 `wait()`。
- 需要有界观察的调用方使用 D-028 的 `wait_for()`。
- Pending Runtime metrics/trace 对诊断长期阻塞成为必要能力。

### Non-goals and deferred risks

- 本决策不定义 stop token 可中断 wait 的变体。
- `wait()` 的 Worker 上下文异常规则由 D-034/D-039 固定；本决策仍只负责完成语义。
- concurrent waiters 的公共协调规则由 D-031/D-037 固定，具体唤醒原语仍属于实现选择。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确要求 `wait()` 只有真正完成后才返回，并接受它可能无限期阻塞。
- Code or data evidence: D-008/D-010/D-014 已采用同步、可无限阻塞的资源完成边界；D-020 要求实际 join 后才发布 Stopped。

### Traceability

- ADR: [ADR-0015](../../docs/adr/0015-finalization-is-split-phase.md)
- Spec destinations: R-032
- Tickets: Pending
- Tests: Pending

## D-028 — Finalization wait_for 超时只返回 TimedOut 而后台继续

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

调用方需要在不放弃 Finalization 的情况下有界观察进度，以便记录诊断、决定是否继续等待、显式升级策略或终止整个进程。超时只是观察结果，不能改变已经不可逆关闭的注册入口或伪造资源完成。

### Decision

Finalization `wait_for(timeout)` 必须在真实 Finalization Completion 已达成时返回 `Completed`；若等待时限内未完成则返回 `TimedOut`。`TimedOut` 不发布或伪造 `Finalized`，不恢复 Scheduler 注册，不重启 Reaper，也不停止后台工作；Reaper 与 coordinator 必须继续推进同一次 Finalization，后续 `wait()` 或 `wait_for()` 仍可观察它。

### Invariants

- `TimedOut` 只表示本次有界等待结束，不表示 Finalization 失败、取消或完成。
- 超时后注册入口继续永久关闭，Reaper Service 保持 `Finalizing`，不存在 rollback/restart。
- 超时不得自动升级任何 Runtime 为 Immediate，不得 detach Worker/coordinator，也不得释放仍被使用的 Runtime State。
- 调用方必须在超时后选择继续等待、显式升级已定义的策略，或决定终止整个进程；Runtime 不代替调用方作该策略选择。
- 后续等待必须观察同一个 Finalization Completion；超时不得创建新世代。
- timeout 的时钟、边界竞态和非正时长行为由后续 D-036 固定。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 调用时已经完成 | Completion 已发布 | 立即返回 `Completed` |
| 时限内完成 | 正常有界等待 | 返回 `Completed` |
| 时限到达仍未完成 | 超时观察 | 返回 `TimedOut`，后台继续 |
| `TimedOut` 后再次等待 | 同一终结世代 | 可继续 wait/wait_for 并最终观察 `Completed` |

### Rationale

`wait_for()` 把“有界等待”与“终结策略”分离：Runtime 提供可靠观察，调用方保留数据损失、延迟和进程生存之间的决策权。这比内置超时取消更符合 cooperative cancellation 和模式单调性。

### Rejected alternatives

- 超时即视为完成：会让 Worker/coordinator 仍运行时伪造资源安全边界。
- 超时自动 Immediate：未经明确授权取消已接受任务。
- 超时重新开放注册或重启 Reaper：违反 D-023 的进程级终态。
- 超时停止 coordinator：会遗留 Pending Runtime 失去回收执行能力。

### Consequences

- `wait_for()` 的调用方必须正确处理 `TimedOut`，不能将其当作可安全卸载或销毁全局资源的信号。
- Runtime Metrics/Chrome Trace 应能解释仍在 Finalizing 的 Runtime 与阻塞原因。
- 显式全局升级接口及其作用范围需要单独决策。

### Non-goals and deferred risks

- clock 与边界语义由 D-036 固定；timeout 精度和 stop-token wait 仍不在范围内。
- 结果类型的完整 C++ 名称由 D-039 固定。
- 显式升级操作由 D-029/D-039 固定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确要求 `wait_for(timeout)` 超时返回 `TimedOut` 而不伪造完成；超时后 Reaper/coordinator 继续、注册保持关闭，调用方自行继续等待、显式升级或终止进程。
- Code or data evidence: D-023 已使注册关闭不可逆；D-024 已固定 Graceful 默认；D-020/D-021 保证后台 Reaper 可继续处理 Pending/Join Ready Runtime。

### Traceability

- ADR: [ADR-0015](../../docs/adr/0015-finalization-is-split-phase.md)
- Spec destinations: R-033
- Tickets: Pending
- Tests: Pending

## D-029 — Finalization 显式升级覆盖全部已核算且未完成的 Runtime

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-028 要求 `TimedOut` 后由调用方决定继续等待、显式升级策略或终止进程，但尚未定义“显式升级”的作用域。由于 D-017 允许 Scheduler Handle 在 Runtime State 完成前全部消失，只依赖仍存活的 Handle 分别调用 `shutdown_now()` 无法保证覆盖 Reaper 正在托管的 orphan Runtime，因此需要先决定是否提供进程级升级语义；具体 C++ 接口形态可以随后单独决定。

### Decision

Finalization 控制面提供一个显式的进程级 Immediate 升级操作。调用方触发后，它将同一 Finalization 核算集合中所有尚未达到 Shutdown Completion 的 Runtime 单向请求为 Immediate：Graceful Runtime 按 D-012 升级为 Immediate，已经处于 Immediate 的 Runtime 保持不变，已经完成的 Runtime 不被重写。该操作只在升级请求已被可靠记录并通知相应 Runtime/Reaper coordinator 后返回，不等待 Finalization Completion。

### Invariants

- 升级只能由调用方显式触发；`wait_for()` 返回 `TimedOut` 绝不得隐式触发升级。
- 升级作用于关闭注册入口时已经纳入核算的完整 Runtime 集合，包括已无 Scheduler Handle、仅由 Reaper 托管的 Runtime State。
- 对已注册但仍处于 Starting 竞态的 Runtime，sticky Finalization mode 必须从 Graceful 单向提升为 Immediate；该 Runtime 不得在升级后出现开始执行新用户任务的窗口。
- 已接受但尚未运行的任务按 D-006 完成取消结果；已经运行的任务按 D-007 仅请求协作停止，不得强杀线程。
- 升级操作必须幂等、单调，并复用 D-026 建立的同一个 Finalization Completion；不得创建新的终结世代、恢复注册或重启 Reaper。
- 升级操作返回不表示 Finalization 已完成，也不保证后续等待有界；忽略停止请求的 Running Task 仍可使 Finalization 永久 Pending。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Graceful 且未完成 Runtime | 需要升级 | 单向请求 Immediate |
| 已 Immediate Runtime | 已在更强模式 | 保持 Immediate，幂等处理 |
| 已完成 Runtime | 不再活动 | 不重写终态 |
| orphan Runtime State | 无 Scheduler Handle | 仍由 Finalization 控制面覆盖 |
| Starting Runtime 竞态 | 已注册、尚未开始用户工作 | sticky mode 提升为 Immediate，在启动协议中观察 |

### Rationale

进程级升级能力与进程级 Finalization 核算边界一致，并且不会因 Handle 生命周期结束而丢失控制权。将“发布升级请求”和“等待真正完成”继续分离，可让调用方在多次有界观察之间显式改变策略，同时保留 D-027/D-028 的准确完成语义。

### Rejected alternatives

- 只允许调用方通过仍存活的 Scheduler Handle 分别升级：无法覆盖 D-017 产生的 orphan Runtime State。
- `TimedOut` 自动升级全部 Runtime：把可能导致任务取消的数据损失策略隐藏在观察操作中。
- 每次升级创建新的 Finalization Completion：破坏并发等待者对同一终结世代的观察。
- 对 Running Task 强制终止线程：违反 D-007 的 cooperative cancellation 边界，并可能破坏进程内共享状态。

### Consequences

- 应用主控层在 Finalization 长时间 Pending 时拥有不依赖 Scheduler Handle 的统一升级路径。
- Immediate 仍不是硬实时终止保证；调用方最终仍可能选择终止整个进程。
- 公共 Interface 最终由 D-030/D-039 选择挂载在 FinalizationControl 上，并保持本决策的全局核算语义。

### Non-goals and deferred risks

- 升级函数名称与归属由 D-030/D-039 后续固定为 `FinalizationControl::request_immediate()`。
- 本决策不定义选择性升级单个 Runtime 的公共接口。
- 调用线程资格由 D-033 固定；并发升级的内部同步算法与诊断格式仍属于实现选择。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受进程级显式 Immediate 升级覆盖 Finalization 核算集合内全部尚未完成的 Runtime，包括仅由 Reaper 托管的 Runtime State；升级复用同一完成状态并在请求发布后返回。
- Code or data evidence: D-006/D-007 定义 Immediate 的任务语义；D-012 定义 Graceful 到 Immediate 的单调升级；D-017 允许 Handle 与 Runtime State 解耦；D-024/D-026 定义 Finalization 的核算集合与 sticky 请求。

### Traceability

- ADR: [ADR-0015](../../docs/adr/0015-finalization-is-split-phase.md)
- Spec destinations: R-034
- Tickets: Pending
- Tests: Pending

## D-030 — begin_finalization 返回控制对象承载后续等待与升级

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-026 至 D-029 已确定进程级 Finalization 的开始、等待、超时观察和显式升级语义，但尚未决定这些操作在 C++ Interface 上是彼此独立的全局函数，还是通过 begin 返回的对象组织。全局函数最少，却允许调用方在类型层面无序地先调用 `wait()`；控制对象能把同一次 Finalization Completion 与后续操作显式关联，但必须避免让对象生命周期被误解为 Reaper 生命周期所有权。

### Decision

公共 Interface 以 `[[nodiscard]] FinalizationControl begin_finalization()` 作为唯一入口。begin 完成 D-026 的不可逆状态转换后返回一个关联到进程级唯一 Finalization Completion 的控制对象；D-027 的 `wait()`、D-028 的 `wait_for(timeout)` 和 D-029 的显式 Immediate 升级均作为该对象的操作提供，不再提供可脱离 begin 单独调用的公共全局 `wait`/`wait_for`/升级函数。

### Invariants

- `FinalizationControl` 不允许公共默认构造；调用方只有成功执行 `begin_finalization()` 后才能获得有效控制对象。
- 控制对象是观察和请求同一次进程级 Finalization 的 capability，不拥有 Reaper Service 或 Runtime State 的生命周期。
- 销毁控制对象不得阻塞、取消 Finalization、恢复注册、重启 Reaper 或重置 Finalization Completion；后台必须继续推进。
- 控制对象的方法必须保持 D-026 至 D-029 的既有语义，不得因 Interface 形态改变 begin、等待、超时或升级的线性化边界。
- 控制对象内部必须隐藏进程级 registry、coordinator join ownership、Runtime 核算集合和完成发布机制。
- 控制对象复制规则由 D-031、重复/并发 begin 由 D-032、等待异常与最终 Interface 由 D-034/D-035/D-039 固定。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| begin 后等待 | 正常用法 | 通过返回对象调用 `wait`/`wait_for` |
| begin 后升级 | 长期 Pending | 通过同一对象显式请求进程级 Immediate |
| 控制对象提前销毁 | 调用方放弃观察 | Finalization 与后台 Reaper 不受影响 |
| 试图 begin 前等待 | 无有效对象 | 公共 Interface 在类型层面不提供该调用路径 |

### Rationale

返回控制对象把“开始不可逆终结”与“观察/升级这一次终结”组成一个深 Module Interface：调用顺序更难误用，所有复杂的共享完成和后台协调仍被隐藏。它同时避免把进程全局状态暴露成一组可任意排列的函数，也不会采用会在析构时产生隐式策略的 RAII owner 语义。

### Rejected alternatives

- 全部使用进程级自由函数：表面更少，但不能通过类型表达 begin-before-wait，并扩大任何代码随时操纵全局 Finalization 的误用面。
- `begin_finalization()` 返回 `void`，另设全局 accessor 获取控制对象：增加第二条获取路径和额外状态错误。
- 控制对象析构时自动 wait 或 Immediate：把可能无限阻塞或取消任务的策略藏进析构。

### Consequences

- 典型调用形成 `auto finalization = begin_finalization(); finalization.wait_for(...);` 的显式流程。
- 公共类型需要清楚表明它是 capability/observer，而不是 Reaper 的所有者。
- 单元测试可以围绕内部可注入的 Finalization core 建立 seam；公共不可逆行为仍需进程隔离测试。

### Non-goals and deferred risks

- 控制对象复制/移动与跨线程共享由 D-031 固定。
- 重复或并发 begin 的幂等返回由 D-032 固定。
- timeout clock、结果枚举与方法异常规则由 D-034 至 D-036/D-039 固定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案，因此确认 begin 返回不可默认构造、析构无副作用的控制对象，等待与显式升级只通过该对象提供。
- Code or data evidence: D-026 至 D-029 已固定 Interface 必须承载的四类行为；D-023 固定 Finalization 为进程级一次性生命周期。

### Traceability

- ADR: [ADR-0016](../../docs/adr/0016-finalization-uses-a-control-object.md)
- Spec destinations: R-035, R-036
- Tickets: Pending
- Tests: Pending

## D-031 — FinalizationControl 可复制并支持多线程共享观察

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-030 使 begin 返回控制对象，但尚未决定它是单一 move-only token，还是可以被应用主控、测试 harness 和监督线程共同持有。Finalization Completion 本身是进程级唯一状态；若控制对象只能由一个调用方持有，外部就必须再造共享封装，反而把多等待者协调泄漏到 Module 之外。

### Decision

`FinalizationControl` 是可复制、可移动的轻量共享 capability。所有副本关联同一个进程级 Finalization Completion，并可从多个非 Worker 线程并发调用观察操作；状态改变操作仍按既有线性化规则作用于同一次 Finalization，而不是由每个对象维护独立状态。

### Invariants

- 复制控制对象不得创建新的 Finalization 世代、复制 Runtime 核算集合或增加 Reaper coordinator。
- 所有有效副本必须观察同一个 Finalizing/Finalized 状态、同一个模式升级结果和同一个完成发布。
- 并发 `wait()`、`wait_for()` 与 `request_immediate()` 必须数据竞争安全；其具体允许调用上下文由后续决策约束。
- 销毁任意一个或全部控制对象都不得取消或暂停后台 Finalization，继续遵循 D-030。
- Finalization Completion 发布后，仍存活的全部控制对象必须稳定观察完成，不得因副本创建时间不同而返回冲突结果。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 复制控制对象 | 多观察者 | 共享同一完成状态，不复制后台资源 |
| 多线程同时等待 | 并发观察 | 由同一完成事件协调唤醒 |
| 一个副本请求 Immediate | 显式升级 | 全部副本随后观察同一个升级后过程 |
| 全部副本销毁 | 无前台观察者 | 后台 Finalization 继续 |

### Rationale

Finalization 是进程级共享事实，控制对象应表达对该事实的共享观察能力，而不是唯一所有权。可复制 capability 让调用方可以自然地把监督、日志和最终 join 职责分给不同非 Worker 线程，同时保持 Interface 小而深。

### Rejected alternatives

- move-only 控制对象：迫使多观察者自行包装共享所有权，增加外部协议和误用面。
- 返回进程级单例引用：无法表达有效性与 begin-before-wait 顺序，也容易形成悬空引用或全局访问滥用。
- 每次复制建立独立完成对象：会破坏 D-026/D-027 的唯一 Finalization Completion。

### Consequences

- 实现需要一个进程级共享完成核心，控制对象只持有安全引用或等价 capability。
- 公共文档必须说明“可复制”不等于“拥有或延长 Runtime State 的业务生命周期”。
- 并发等待者中的 coordinator join 所有权由 D-037 固定。

### Non-goals and deferred risks

- 本决策不规定控制对象的字节大小、引用计数方案或是否使用 `shared_ptr`。
- Worker 调用等待方法的行为由 D-034/D-035 固定。
- coordinator join 的公共唯一所有权由 D-037 固定；具体同步原语仍属于实现选择。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐将 FinalizationControl 设计为可复制、线程安全的共享 capability，以支持多个观察者复用唯一完成状态。
- Code or data evidence: D-026/D-027 已定义唯一 Finalization Completion；D-030 已选择由 begin 返回控制对象。

### Traceability

- ADR: [ADR-0016](../../docs/adr/0016-finalization-uses-a-control-object.md)
- Spec destinations: R-036
- Tickets: Pending
- Tests: Pending

## D-032 — 重复与并发 begin_finalization 幂等共享同一终结世代

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

多个应用子系统可能无法可靠判断谁先发起进程级终结。若第二次 begin 报错，调用方需要额外的全局选主；若并发 begin 能建立多个完成状态，则注册关闭、Graceful 请求和 coordinator join 都会失去唯一所有权。

### Decision

首次 `begin_finalization()` 以一个线性化点执行 D-026 的永久注册关闭并建立唯一 Finalization Completion。与其并发或之后发生的重复 begin 是成功的幂等参与：它们不重复终结副作用，并返回关联到同一完成状态的 `FinalizationControl`。在已经 `Finalized` 后调用 begin，必须立即返回一个已完成控制对象，不创建新世代或重启 Reaper。

### Invariants

- 并发 begin 中只能有一次注册关闭线性化和一次初始 sticky Graceful 请求发布。
- 非首个调用最多等待“首次 begin 的状态与控制对象已可靠发布”，不得等待 Runtime drain、Worker join 或 Finalization Completion。
- 所有 begin 返回的控制对象必须满足 D-031，观察同一核算集合与完成状态。
- `Finalizing` 和 `Finalized` 都是 begin 的幂等吸收状态；不得因重复 begin 恢复注册或启动新的 coordinator。
- 若首次 begin 发生时从未建立过 Reaper Service 且核算集合为空，必须永久关闭注册并直接发布已完成状态，不得仅为终结空集合创建 coordinator thread。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 首次 begin | Reaper 可注册 | 关闭注册、发布 Graceful 请求、返回控制对象 |
| 并发 begin | 首次 begin 正在发布 | 短暂协调后返回同一控制状态 |
| Finalizing 后 begin | 终结进行中 | 幂等返回同一进行中状态 |
| Finalized 后 begin | 已完成 | 立即返回已完成状态 |
| 从未创建 Scheduler | 空核算集合 | 不创建 coordinator，直接完成 |

### Rationale

进程级 one-shot 操作采用幂等 begin 可以消除外部选主与 check-then-act 竞态，同时不弱化 D-023 的不可逆性。空集合快速完成避免为了关闭一个尚未启动的 Runtime 域而反向创建待回收线程。

### Rejected alternatives

- 非首次 begin 返回错误：迫使应用自己串行化全局终结，并引入状态检查竞态。
- 每次 begin 返回独立完成对象：会制造多个 join owner 和相互冲突的 Finalized 发布。
- Finalized 后允许 restart：直接违反 D-023。
- 空集合也创建 coordinator 再令其退出：增加无意义的线程创建失败面和资源周转。

### Consequences

- 多个库或应用模块可以安全地请求同一个进程级 Finalization，但仍必须遵守只在进程收尾阶段调用的不可逆契约。
- begin 的实现需要一个一次性初始化/发布协议，而不是简单的无同步全局布尔值。
- 并发等待者如何只 join coordinator 一次由后续决策固定。

### Non-goals and deferred risks

- 本决策不固定 once-state 使用 mutex、atomic 状态机还是其他实现。
- 本决策不定义控制对象等待方法的 caller eligibility。
- 本决策不支持测试中的 reset 或多代 Reaper。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐把重复和并发 begin 设计为共享唯一完成状态的幂等操作，并让空核算集合直接完成。
- Code or data evidence: D-023 已定义进程级 one-shot Finalization；D-026 定义 begin 发布边界；D-030/D-031 定义控制对象及共享观察。

### Traceability

- ADR: [ADR-0016](../../docs/adr/0016-finalization-uses-a-control-object.md)
- Spec destinations: R-037
- Tickets: Pending
- Tests: Pending

## D-033 — 非阻塞 Finalization 命令允许从任意应用线程调用

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

`begin_finalization()` 和 `request_immediate()` 只发布进程级策略，不承担 Runtime drain 或 thread join。若仅因调用线程是 Scheduler Worker 就拒绝这些操作，任务无法在检测到进程级退出条件时安全发起终结；若它们内部等待完成，又会重现 D-009/D-011 避免的 self-wait。

### Decision

`begin_finalization()` 与 `FinalizationControl::request_immediate()` 可以由任意应用线程调用，包括任意已注册 Scheduler 的 Worker。两者都必须保持请求式语义：只执行必要的线性化、可靠记录和通知，不等待 Drain Work Closure、Worker 退出、coordinator 退出或 join。

### Invariants

- Worker 调用 begin 后必须能够继续完成当前任务并离开 Worker loop，不得等待自身所属 Runtime 的 Shutdown Completion。
- Worker 调用 `request_immediate()` 只发布 D-029 的进程级升级请求，不得同步等待升级结果完成。
- 允许 Worker 调用不改变 D-023 的高权限、不可逆性质；调用方架构仍应只把控制对象交给有权触发进程终结的代码。
- 两个命令可以短暂争用内部同步以完成线性化，但不得把“非阻塞”宣称为 lock-free、wait-free 或固定时延保证。
- 同 Scheduler Worker 对 `shutdown()`/`shutdown_now()` 的限制仍由 D-009/D-011 约束；本决策不把同步 Scheduler 关停方法改成 Worker-safe。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 普通应用线程 | begin/升级 | 发布请求后返回 |
| 任意 Scheduler Worker | begin/升级 | 同样发布请求后返回，不等待自身 |
| 多线程并发请求 | 共享 Finalization | 按 D-029/D-032 幂等、单调线性化 |

### Rationale

调用资格应由操作是否会形成 self-wait 决定，而不是笼统禁止 Worker 触达进程控制面。请求式命令从 Worker 调用是安全的，并能支持任务检测致命条件后通知应用进入全局收尾；真正的阻塞观察继续由非 Worker 路径承担。

### Rejected alternatives

- 所有 Finalization 操作都禁止 Worker 调用：不必要地限制安全的请求式控制操作。
- Worker 调用 begin 时静默改为 Immediate：把调用上下文错误地转换成任务取消策略。
- begin 或升级在 Worker 上同步等待：形成当前任务对自身终结的循环依赖。

### Consequences

- WorkerContext 只需在阻塞等待入口执行拒绝检查；请求式入口不因 Worker 身份失败。
- 安全评审必须把 FinalizationControl 视为进程级高权限 capability，避免随普通 Task Context 广泛传播。

### Non-goals and deferred risks

- 本决策不提供语言级权限系统或撤销控制对象的能力。
- 本决策不承诺请求式命令无锁或实时有界。
- 本决策不决定异常规格；正常已初始化路径不得依赖 Runtime drain 才能返回。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐允许任意应用线程发起非阻塞 begin 与显式升级，同时把所有完成等待留给非 Worker。
- Code or data evidence: D-026/D-029 已把两项操作定义为请求发布后返回；D-009/D-011 的拒绝理由是同步 self-wait/self-join，而非 Worker 身份本身。

### Traceability

- ADR: [ADR-0016](../../docs/adr/0016-finalization-uses-a-control-object.md)
- Spec destinations: R-038
- Tickets: Pending
- Tests: Pending

## D-034 — Finalization wait 从任意 Scheduler Worker 调用时抛出 logic_error

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-027 的 `wait()` 必须等待全部已核算 Runtime 完成并 join。Finalization 开始后仍存活的任意 Scheduler Worker 都属于该核算集合；如果它在用户任务中调用 `wait()`，Finalization Completion 必须等待这个 Worker 退出，而 Worker 又等待 Completion，形成确定性的进程级 self-wait。

### Decision

任意 AstraScheduler Worker 调用 `FinalizationControl::wait()` 都必须在开始等待或认领 coordinator join 所有权之前同步抛出 `std::logic_error`。该拒绝不限于“当前 Scheduler”：进程级 Finalization 核算所有注册 Runtime，因此任何 Scheduler Worker 都不具备阻塞等待资格。

### Invariants

- 拒绝检查必须早于 condition wait、Runtime join、coordinator join 或任何 Finalization 状态改变。
- 抛出异常不得取消 Finalization、升级模式、恢复注册或消耗唯一 join 所有权。
- 普通非 Worker 线程继续适用 D-027：只在真实完成后返回，并可无限期阻塞。
- `wait()` 不得因 Worker 调用而静默退化为异步请求或立即成功。
- 已发布 Finalized 后理论上不再存在活动 AstraScheduler Worker；若 Worker 身份标记异常残留，实现仍应优先拒绝而不是掩盖生命周期错误。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 任意 Scheduler Worker | `wait()` | 无副作用抛出 `std::logic_error` |
| 普通应用/监督线程 | `wait()` | 按 D-027 等待真实完成 |
| Reaper coordinator 内部 | 非公共调用路径 | 不通过公共控制对象等待或 join 自己 |

### Rationale

进程级等待与单个 Scheduler 关停不同：另一个 Scheduler 的 Worker 也在同一完成集合中。显式异常让确定性死锁变成可测试的调用错误，并保持 `wait()` 对合法调用者的单一完成含义。

### Rejected alternatives

- 允许 Worker 无限等待：Completion 依赖调用 Worker 先退出，确定性死锁。
- Worker 调用立即返回：会在未完成时伪造 D-027 的资源安全边界。
- Worker 调用自动转成 begin：begin 已经发生，且会让同一方法按上下文改变含义。
- 以 `std::terminate()` 处理：显式方法存在正常错误通道，无需采用析构级 fail-fast。

### Consequences

- `wait()` 不能声明为 `noexcept`。
- WorkerContext 必须能识别“当前线程属于任意 AstraScheduler Runtime”，而不只比较单个 Scheduler id。
- 测试必须覆盖多个 Scheduler 中任一 Worker 调用全局 wait 的拒绝。

### Non-goals and deferred risks

- 本决策不统一修改 `shutdown()`/`shutdown_now()` 的既有错误类型。
- 本决策不提供 Worker-safe coroutine await 版本。
- 本决策不定义异常消息文本。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐让全局 wait 从任意 Scheduler Worker 调用时以 `std::logic_error` 无副作用失败，避免进程级 self-wait。
- Code or data evidence: D-027 要求等待所有 Worker 与 coordinator 完成；D-009/D-011 已确认同步等待自身必须在副作用前拒绝。

### Traceability

- ADR: [ADR-0016](../../docs/adr/0016-finalization-uses-a-control-object.md)
- Spec destinations: R-039
- Tickets: Pending
- Tests: Pending

## D-035 — Finalization wait_for 从任意 Scheduler Worker 调用时同样被拒绝

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

有限 timeout 表面上避免永久 self-wait，但活动 Worker 在调用期间本身阻止 Finalization Completion 达成，因此它不可能在仍执行该调用时真实观察 `Completed`。允许这种调用只会占用调度容量、延长 Graceful drain，并使超时结果被误解为独立于调用者的系统进度。

### Decision

任意 AstraScheduler Worker 调用 `FinalizationControl::wait_for(timeout)` 都必须在读取 timeout 或开始等待之前同步抛出 `std::logic_error`，与 D-034 的 `wait()` 调用资格保持一致。`Completed`/`TimedOut` 结果只适用于合法的非 Worker 调用方。

### Invariants

- 即使 timeout 为零或负值，Worker 调用仍必须抛出 `std::logic_error`；公共 Interface 不把 `wait_for(0)` 兼作 Worker-safe 状态查询。
- 拒绝不得返回 `TimedOut`，因为调用上下文错误与真实期限到达是不同事实。
- 拒绝不得改变 Finalization 模式、Completion 或 coordinator join owner。
- 非 Worker 调用继续适用 D-028 与后续 timeout 线性化规则。
- 若未来需要 Worker-safe 的纯观察，应设计独立、无等待且不参与 join 的查询 Interface。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 任意 Scheduler Worker + 正 timeout | `wait_for` | 无副作用抛出 `std::logic_error` |
| 任意 Scheduler Worker + 非正 timeout | 即时探测意图 | 仍抛出，不复用等待 Interface |
| 普通非 Worker 线程 | `wait_for` | 返回 `Completed` 或 `TimedOut` |

### Rationale

统一禁止 Worker 使用两种等待操作比“无界等待拒绝、有界等待允许”更容易正确理解，也避免一个 Worker 通过很长 timeout 实质阻塞整个 Finalization。错误与超时保持正交，使 `TimedOut` 始终只表达非 Worker 的有界观察结果。

### Rejected alternatives

- 允许有限 wait_for：调用 Worker 自身使 Completed 不可达，并会拖延 Runtime drain。
- 仅允许 `wait_for(0)`：把等待 Interface 偷偷扩展成查询 Interface，增加上下文特例。
- Worker 调用统一返回 TimedOut：混淆错误调用与真实超时。

### Consequences

- `wait_for()` 不能声明为 `noexcept`，且调用方需要区分异常与结果枚举。
- 若 Runtime Metrics 需要 Worker 内查询，应提供不携带等待/join 语义的独立只读通道。
- 测试必须覆盖正、零、负 timeout 的 Worker 拒绝。

### Non-goals and deferred risks

- 本决策不承诺未来一定提供 `is_finalized()`。
- 本决策不定义 Worker-safe coroutine suspension 等待。
- 本决策不决定 `std::logic_error` 的派生项目类型。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐让 wait_for 与 wait 采用相同的非 Worker 调用资格，并保留 TimedOut 的纯超时含义。
- Code or data evidence: D-028 定义 TimedOut 只是合法有界等待的观察结果；D-034 定义进程级等待的 Worker self-wait 风险。

### Traceability

- ADR: [ADR-0016](../../docs/adr/0016-finalization-uses-a-control-object.md)
- Spec destinations: R-040
- Tickets: Pending
- Tests: Pending

## D-036 — wait_for 使用 steady clock 并在线性化边界区分完成与超时

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-028 只固定了 `Completed`/`TimedOut` 含义，尚未定义系统时钟跳变、非正 duration 以及 Completion 与 deadline 同时发生时的结果。若没有统一观察边界，不同实现或平台可能对同一竞态返回不一致结果，测试也无法建立可靠 oracle。

### Decision

合法非 Worker 调用的 `wait_for(timeout)` 必须基于单调的 `std::chrono::steady_clock` 计算 deadline。若 timeout 小于或等于零，则执行一次无副作用即时观察：已发布 Finalization Completion 返回 `Completed`，否则返回 `TimedOut`。对于正 timeout，Completion 发布与 deadline 到达必须在同一同步域内形成唯一观察顺序：先观察到 Completion 则返回 `Completed`，先在线性化观察点确认期限已到且 Completion 尚未发布则返回 `TimedOut`。

### Invariants

- 墙钟校准、时区变化或系统时间回拨不得延长或缩短等待期限。
- `TimedOut` 线性化之后即使 Completion 在方法实际返回前发布，本次调用仍返回 `TimedOut`；后续等待可以观察 `Completed`。
- 非正 timeout 不得触发模式升级、coordinator 停止或新的 Finalization 世代。
- timeout 是有界观察协议，不是实时调度保证；OS 调度和同步收尾可能使实际返回略晚于请求 duration。
- duration 到内部 deadline 的转换必须避免未定义溢出；具体饱和转换算法属于实现选择。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| timeout ≤ 0 + 已完成 | 即时观察 | `Completed` |
| timeout ≤ 0 + 未完成 | 即时观察 | `TimedOut` |
| Completion 先线性化 | 正 timeout | `Completed` |
| deadline 先线性化 | 正 timeout | `TimedOut`，后台继续 |
| wall clock 跳变 | 任意 timeout | 不影响 steady deadline |

### Rationale

steady clock 与单一观察点给 timeout 建立跨平台可测试语义，同时保留并发系统的现实：方法返回瞬间看到的全局状态可能已经晚于本次结果线性化点。把非正 duration 定义为合法即时探测，也避免负值下发生巨大无符号转换或意外长等待。

### Rejected alternatives

- 使用 `system_clock`：墙钟调整会破坏 duration 等待含义。
- deadline 到达后一律优先 TimedOut，即使 Completion 已发布：可能漏掉已完成事实并制造不必要重试。
- Completion 与 deadline 竞态结果未指定：让测试和调用方无法推理。
- 负 timeout 视为无限等待：与 `wait_for` 名称和有界语义冲突。

### Consequences

- 测试需要可控 steady-clock adapter 或内部 seam 来覆盖边界竞态，而公共生产 Interface 仍使用 `std::chrono::duration`。
- 文档必须说明 `TimedOut` 与方法返回时刻的状态可能存在正常竞态。
- 精确 result 类型与公开方法签名由 D-039 固定。

### Non-goals and deferred risks

- 本决策不提供绝对 deadline 的 `wait_until()`。
- 本决策不提供 stop-token 可取消等待。
- 本决策不规定 timeout 的纳秒级精度或调度延迟上限。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐用 steady clock、非正即时观察和唯一竞态顺序固定 wait_for 的跨平台含义。
- Code or data evidence: D-028 明确延期 clock、边界竞态与非正时长行为；D-031 要求多线程共享观察。

### Traceability

- ADR: [ADR-0016](../../docs/adr/0016-finalization-uses-a-control-object.md)
- Spec destinations: R-041
- Tickets: Pending
- Tests: Pending

## D-037 — 唯一等待者 join coordinator 后发布 Finalization Completion

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

Reaper coordinator thread 不能 join 自己，而 D-027 又要求 Finalization Completion 只有在 coordinator 已退出并被 join 后才能发布。多个可复制控制对象可以并发等待；若每个等待者都 join 会产生所有权竞态，若 coordinator 在退出前自报完成则会提前越过真正资源边界。

### Decision

coordinator 在全部 Runtime 核算和 Reaper 工作清空后单调发布 `CoordinatorExited` 并退出，但不得自行发布 Finalization Completion。合法非 Worker 等待者中恰好一个通过原子认领成为 coordinator join owner；它只在观察到 `CoordinatorExited` 后执行唯一一次 join，随后发布进程级 `Finalized` 与 Finalization Completion。其他等待者只观察同一完成事件，不重复 join。

### Invariants

- coordinator 不得 detach 或 join 自己；`CoordinatorExited` 不是 Finalization Completion。
- join owner 只能认领一次，且只能在 coordinator 已停止执行后调用 join。
- `wait()` 必须在需要时承担或等待该 join，之后才能返回。
- `wait_for()` 只有在期限线性化前观察到 `CoordinatorExited` 并成功完成/观察唯一 join 时才可返回 `Completed`；否则按 D-036 返回 `TimedOut`。
- 若没有任何等待者，coordinator 可以退出并保持已退出但未 join 的内部状态；注册仍永久关闭，后续合法等待者负责完成 join 和发布 Finalized。
- 控制对象析构不得隐式认领 join，继续遵循 D-030。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| coordinator 仍在工作 | 等待进行中 | 等待者观察退出或 timeout |
| coordinator 已退出、未 join | 首个合格等待者 | 原子认领并 join |
| 多个并发等待者 | join 竞态 | 一个 join，其余等待 Completion |
| 无等待者 | 后台工作已清空 | 保持 Exited-unjoined，等待未来控制调用 |
| 已 Finalized | 任意合法等待 | 立即观察 Completed，不重复 join |

### Rationale

把“coordinator 退出”和“外部 join 完成”拆成两个单调阶段，是在不增加第二条辅助线程、不 detach 的前提下满足真实完成边界的最小协议。等待者天然是愿意承担完成同步的非 Worker 上下文，因此适合作为唯一 join owner。

### Rejected alternatives

- coordinator 退出前发布 Finalized：线程仍 joinable，资源边界不完整。
- coordinator detach 后自行退出：破坏 D-027 的 join 保证。
- 每个等待者都尝试 join：对同一 thread 并发 join，行为不安全。
- begin 创建第二条 finalizer thread 专门 join：扩大 D-021 的进程级辅助线程拓扑，只为解决低频收尾。
- 控制对象析构自动 join：可能在任意线程和任意作用域产生无界阻塞。

### Consequences

- 应用若只 begin 而从不合法等待，后台 Runtime 可以完成，但进程级 Finalized 不会在 coordinator 被 join 前发布。
- `wait_for()` 的 duration 不是硬实时上限；在期限前认领已经退出的 coordinator 后，OS 调度可能使方法实际返回略晚，继续遵循 D-036。
- 内部状态至少需要区分 Finalizing、CoordinatorExited、JoinClaimed 与 Finalized，具体编码属于实现选择。

### Non-goals and deferred risks

- 本决策不固定 join owner 使用 CAS、mutex 还是 once flag。
- 本决策不新增第二条专用 finalizer thread。
- OS thread join 的不可恢复控制面故障由 D-040 固定为 fail-fast；平台错误分类细节留给实现。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐由一个合法等待者在 coordinator 已退出后唯一 join，并由该路径发布真实 Finalization Completion。
- Code or data evidence: D-021 只允许一条 Reaper coordinator；D-027 要求 coordinator 退出且 join；D-031 允许多等待者共享控制对象。

### Traceability

- ADR: [ADR-0016](../../docs/adr/0016-finalization-uses-a-control-object.md)
- Spec destinations: R-042
- Tickets: Pending
- Tests: Pending

## D-038 — Reaper Finalization 只允许显式触发而不挂接静态析构

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

Finalization 会永久关闭注册，并可能因不合作任务无限等待。若把 begin/wait 隐式挂到 `atexit`、静态对象析构或最后一个 Scheduler 析构，静态销毁顺序可能先释放任务依赖资源，随后 Reaper 又访问它们；也可能在用户不可控制的位置永久挂起或触发 self-join。动态库卸载更要求所有执行线程和代码引用在卸载前真实结束。

### Decision

Reaper Finalization 只能由调用方显式调用 `begin_finalization()` 并通过控制对象观察完成。AstraScheduler 不从 `atexit`、进程级静态析构、最后一个 Scheduler/Handle 析构或空闲超时自动触发 begin、wait 或 Immediate escalation。安全动态库卸载必须由非 Worker 调用方先观察 `Completed`，再确保不再存在会调用库代码的 Scheduler/Task/Finalization 对象后才能卸载。

### Invariants

- 静态析构不得隐式执行可能无限阻塞的 `wait()`。
- 最后一个 Runtime 注销仍只让 Reaper Service 进入空闲，继续遵循 D-022，不等同于 Finalization。
- `TimedOut` 绝不是安全卸载信号；此时 Worker、Runtime 或 coordinator 仍可能执行库代码。
- 调用方若在 TimedOut 后选择终止整个进程，AstraScheduler 不承诺任务清理、用户析构或 trace flush 已完成。
- 测试不得依赖公共 reset/restart 恢复全局状态；不可逆公共行为使用独立子进程隔离。
- Finalization Completion 证明执行基础设施已结束，但不自动销毁仍由应用持有的前台对象；卸载前对象生命周期仍由调用方负责。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 正常应用收尾 | 显式控制 | begin 后 wait/wait_for/升级 |
| 最后 Scheduler 销毁 | Reaper 空闲 | 不自动 Finalize |
| 静态/atexit 阶段 | 隐式收尾 | 不注册自动 begin/wait hook |
| 动态库卸载 | 需要资源安全 | 必须先 Completed 且停止使用公共对象 |
| TimedOut 后进程终止 | 强制退出 | 交给 OS 终止，不伪造库级清理保证 |

### Rationale

不可逆且可能无界的进程控制操作必须由应用在依赖资源仍有效时显式编排。拒绝静态自动终结避免 C++ static destruction order fiasco，也让“继续等待、升级或终止进程”的决策保持在有日志和策略上下文的主控层。

### Rejected alternatives

- `atexit` 自动 wait：可能在依赖已销毁后运行并永久阻塞进程退出。
- 最后一个 Scheduler 析构自动 Finalize：把单 Runtime 生命周期错误地提升为进程级永久关闭，后续无法创建 Scheduler。
- timeout 后自动 detach 并允许卸载：仍执行库代码的线程会跳入已卸载地址。
- 公共 `reset_for_test()`：破坏 D-023 的真实 one-shot 语义并掩盖世代隔离错误。

### Consequences

- 嵌入式/动态库使用者必须在宿主卸载协议中显式加入 Finalization Completion gate。
- 正常测试和 sanitizer 收尾应显式 finalization；全局不可逆场景使用子进程。
- 若应用直接终止进程，Reaper 的进程生命周期存储由 OS 回收，而不是依赖危险的静态析构协议。

### Non-goals and deferred risks

- 本决策不定义 `fork()` 后的 Runtime 语义；默认不承诺支持带活动线程的 fork。
- 本决策不提供跨动态库实例共享 Reaper 的 ABI 协议。
- 本决策不规定应用选择强制进程终止的超时时长。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐让不可逆、可能无界的 Finalization 只由应用显式编排，并禁止静态析构与 atexit 自动触发。
- Code or data evidence: D-022 保持空闲 Reaper 存活；D-023 禁止 restart；D-027/D-028 区分真实完成与超时。

### Traceability

- ADR: [ADR-0017](../../docs/adr/0017-finalization-is-explicit-process-teardown.md)
- Spec destinations: R-043, Open Questions
- Tickets: Pending
- Tests: Pending

## D-039 — Finalization 公共 C++ Interface 固定为控制对象四操作

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

D-030 至 D-038 已确定 Interface 的能力与调用规则，但后续 spec、Tickets 和示例仍需要一套稳定的公共类型与方法名称。若只记录行为而不固定入口，版本 Tickets 可能分别发明不同的全局函数、result 枚举或升级名称，导致文档和实现漂移。

### Decision

公共 C++20 Interface 使用 `astra::FinalizationControl` 与 `astra::FinalizationWaitResult`。`astra::begin_finalization() noexcept` 是唯一创建控制对象的入口；控制对象公开 `wait() const`、duration 泛型的 `wait_for(timeout) const` 和请求式 `request_immediate() const noexcept`。结果枚举至少固定 `Completed` 与 `TimedOut` 两个值；begin 与 wait_for 返回值都标记 `[[nodiscard]]`。

### Invariants

- `FinalizationControl` 无公共默认构造，复制/移动与析构不得抛出，并继续遵循 D-030/D-031 的共享 capability 语义。
- `begin_finalization()` 必须 `noexcept`；需要失败的 Reaper 建立和 handoff 资源准备必须发生在 Worker 启动事务中，空核算集合按 D-032 无资源完成。
- `request_immediate()` 必须 `noexcept`，只发布幂等升级请求；已 Finalized 时为成功 no-op。
- `wait()` 与 `wait_for()` 不标记 `noexcept`，因为 D-034/D-035 的非法 Worker 上下文通过 `std::logic_error` 报告。
- 不提供同义的全局 `wait_finalization()`、`finalize_now()`、control accessor 或 reset Interface。
- duration 模板必须接受标准 `std::chrono::duration<Rep, Period>`，内部按 D-036 转换到 steady deadline。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| begin | 进程级入口 | `[[nodiscard]] FinalizationControl`，`noexcept` |
| wait | 无界完成观察 | `void`，合法非 Worker 才可调用 |
| wait_for | 有界完成观察 | `[[nodiscard]] FinalizationWaitResult` |
| request_immediate | 显式全局升级 | `void`，`noexcept` |

### Rationale

四操作 Interface 精确覆盖开始、无界等待、有界等待与显式升级，没有泄漏 registry、Runtime 枚举或 join 协调。控制对象让 begin-before-wait 在类型层面成立，`request_immediate` 名称明确表达“发布请求而非等待完成”，`[[nodiscard]]` 防止误丢控制与 timeout 结果。

### Rejected alternatives

- 进程级自由函数集合：已由 D-030 拒绝，无法编码 begin-before-wait。
- 把升级命名为 `finalize_now()`：容易被误解为立即完成或强杀 Running Task。
- `bool wait_for(...)`：`false` 无法清楚表达是 timeout、错误还是取消，且扩展性差。
- `wait_until()` 与 stop-token overload 同时首发：当前没有已确认用例，会扩大 Interface。

### Consequences

- 头文件需要包含或前置满足 chrono、result enum 与 logic_error 契约。
- 后续版本可以在不改变 Interface 的前提下替换内部等待、注册与 join 实现。
- ABI 是否稳定、符号可见性与导出宏由各版本构建 Ticket 决定，不改变语义。

### Non-goals and deferred risks

- 本决策不固定类的内存布局、PImpl 形式或二进制 ABI 版本策略。
- 本决策不提供异步 coroutine await、progress callback 或 stop-token wait。
- 本决策不决定枚举底层整数类型。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐冻结由 begin 返回的四操作控制对象 Interface，使用明确 result enum 与 request 命名。
- Code or data evidence: D-026 至 D-029 定义四种行为；D-030 至 D-038 已固定对象、并发、调用上下文、timeout 与 join 规则。

### Traceability

- ADR: [ADR-0016](../../docs/adr/0016-finalization-uses-a-control-object.md)
- Spec destinations: R-034, R-044, R-045, R-046, Open Questions
- Tickets: Pending
- Tests: Pending

## D-040 — Reaper 控制面不可恢复故障必须 fail-fast

Status: accepted

Date: 2026-08-25

Supersedes: None

Superseded by: None

### Context

Reaper Service 持有 orphan Runtime State 并负责唯一 join 与 Finalized 发布；一旦 coordinator 因未捕获异常、所有权不变量破坏或不可恢复的 thread join 错误而停止，进程已经无法兑现 Runtime State 存活和回收承诺。尝试把故障当作普通 Task 异常、伪造 Completion 或重启 Reaper 都会掩盖资源安全破坏。

### Decision

Reaper coordinator 顶层必须拦截所有逃逸异常。对无法在内部证明安全恢复的控制面故障，Runtime 必须先执行不抛异常、尽力而为的诊断记录，然后调用 `std::terminate()`；不得发布 `Stopped`/`Finalized`、detach 线程、泄漏后继续运行或重启 Reaper。用户 Callable 异常继续按 Task result/exception propagation 处理，不属于 Reaper 控制面故障。

### Invariants

- coordinator thread 不得因 C++ 异常越过其线程入口而静默消失。
- join ownership、预注册 handoff 和 Runtime State 引用连续性的不变量失败不得转换成 `TimedOut` 或普通任务失败。
- fail-fast 前不得伪造任何 Runtime Shutdown Completion 或 Finalization Completion。
- 诊断路径本身必须 `noexcept` 且不依赖动态分配成功；诊断失败不阻止 terminate。
- Reaper 不得在故障后 restart，继续遵循 D-023。
- 预期的 Worker 不合作、永久 Pending 和合法 wait timeout 不是内部故障，不触发 fail-fast。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 用户 Callable 抛异常 | 任务域 | 捕获到 Task result，不进入 Reaper fatal path |
| Runtime 永久 Pending | 合法不完成 | 保持后台状态，不 terminate |
| wait_for 到期 | 合法观察 | 返回 TimedOut |
| coordinator 逃逸异常/不变量破坏 | 控制面不可恢复 | 诊断后 `std::terminate()` |

### Rationale

Reaper 是内存与线程生命周期的最后安全网，没有可替代的恢复执行体。继续运行却无法保证 orphan Runtime State 和 join 所有权，比确定性终止更危险；fail-fast 让故障可诊断且不把损坏状态伪装成正常完成。

### Rejected alternatives

- 将异常存入某个 Scheduler Future：Reaper 故障属于进程控制面，可能没有存活 Handle，且无法恢复回收承诺。
- 发布 TimedOut/Failed Finalization 后继续：调用方可能错误卸载仍被线程使用的代码。
- detach 全部剩余线程：破坏 D-017/D-027 的资源安全边界。
- 自动重启 coordinator：旧 join token、Runtime 注册与新世代无法安全重建，并违反 D-023。

### Consequences

- Reaper 实现必须有顶层异常屏障和最小 fatal diagnostic hook。
- 故障注入测试需要在独立子进程验证确定性 terminate，不能在普通单元测试进程内触发。
- Metrics/Trace 可以记录最后已知阶段，但不得依赖其成功才能终止。

### Non-goals and deferred risks

- 本决策不固定日志后端、terminate handler 或 crash dump 集成。
- 本决策不把所有资源耗尽都定义为 fatal；Worker 启动前可报告的失败继续按 D-019 正常返回。
- 本决策不定义平台级进程监督与自动重启。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受本窗口剩余推荐方案；推荐对无法安全恢复的 Reaper 控制面故障 fail-fast，而不伪造完成或尝试重启。
- Code or data evidence: D-017/D-019 要求 Runtime State 所有权连续；D-021 只有一个 coordinator；D-023 禁止 Reaper restart。

### Traceability

- ADR: [ADR-0018](../../docs/adr/0018-reaper-control-plane-fails-fast.md)
- Spec destinations: R-047
- Tickets: Pending
- Tests: Pending

## D-041 — submit 从首个版本起返回自定义 TaskHandle

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

参考设计同时提出 v0.1.0 可先公开 `std::future<T>`，后续再引入自定义 Task Handle。整个项目还计划支持 TaskState、Cancellation、DAG、Coroutine 与更丰富的结果观察；若 `std::future<T>` 先成为公开返回类型，后续要么破坏 `submit()` Interface，要么长期维护两套任务结果抽象。现在需要先冻结稳定的公共任务能力边界，而不把内部早期实现手段误写成公共契约。

### Decision

从 v0.1.0 起，`submit()` 的稳定公共返回类型必须是 AstraScheduler 自定义的 `TaskHandle<T>`；`std::future<T>` 不得成为 AstraScheduler 的公共 `submit()` 返回类型。v0.1.0 内部可以使用 `std::promise`、`std::packaged_task` 或其他标准设施实现结果通道，但后续能力必须演进同一个 `TaskHandle` 公共抽象，而不是替换公共返回类型。

### Invariants

- v0.1.0 的公开 `submit()` 签名不得返回 `std::future<T>`。
- 内部结果实现机制不得泄漏为调用方必须依赖的公共类型。
- 后续 TaskState、Cancellation、DAG 与 Coroutine 集成不得要求用另一类公共结果对象替换 `TaskHandle<T>`。
- 基础 Global Queue Scheduler 与后续 Work-Stealing Scheduler 必须使用同一公共任务 Handle 家族，避免 Benchmark 基线与生产 Scheduler 使用不同调用接口。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| v0.1.0 | Basic Scheduler 公共 Interface | 返回最小可用的 `TaskHandle<T>`；内部实现仍可复用标准库设施 |
| 后续 Runtime 版本 | Cancellation、DAG、Coroutine 等能力 | 扩展同一 `TaskHandle` 抽象，不替换 `submit()` 返回类型 |
| 内部实现 | 结果状态与 Callable 包装 | 可以阶段性使用 `promise`/`packaged_task` 等，不形成公共兼容性承诺 |

### Rationale

`std::future<T>` 适合基础异步结果，但不能自然承载计划中的任务身份、状态、取消和多模块调度能力。先固定项目自己的 `TaskHandle<T>`，可以让 v0.1.0 只实现最小语义，同时保持跨版本 Interface 稳定；内部仍能借用标准库组件控制首版实现成本。

### Rejected alternatives

- v0.1.0 公开返回 `std::future<T>`，后续改成 `TaskHandle<T>`：会造成公开签名破坏或迁移层长期存在。
- 同时公开 `std::future<T>` 与 `TaskHandle<T>` 两套 submit：调用方和各 Runtime 模块需要处理重复且可能分叉的结果语义。
- 等到 Cancellation/DAG 版本才决定公共任务抽象：早期版本会先形成难以撤回的调用习惯和兼容性负担。

### Consequences

- v0.1.0 需要实现并测试自定义 Task 共享状态与 Handle 生命周期，即使内部 Callable 包装仍使用标准库设施。
- 后续版本能够在不替换 `submit()` 返回类型的前提下增加状态、取消和调度集成能力。
- TaskHandle 的复制、`get()` 消费、多等待者、取消、任务标识以及 rejected submission 行为仍需分别确认。

### Non-goals and deferred risks

- 本决策不定义 `TaskHandle<T>` 是否可复制。
- 本决策不定义 `get()` 是否单次消费、异常是否可重复观察或等待者数量。
- 本决策不定义取消方法、TaskState 查询、TaskId 暴露或 rejected submission 的表示。
- 本决策不固定内部共享状态使用 `shared_ptr`、intrusive refcount 或其他所有权实现。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受从 v0.1.0 起由公开 `submit()` 返回自定义 `TaskHandle<T>`，不把 `std::future<T>` 作为稳定公共返回类型；标准库结果设施仅可作为内部实现手段。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 640–744、2007–2080 行提出 Future 到 TaskHandle 的阶段性演进；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-042 — TaskHandle 是可复制的共享任务 capability

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-041 已固定 `submit()` 从首版返回自定义 `TaskHandle<T>`，但尚未决定它是单一 move-only consumer，还是可供监督、取消、DAG 与 Coroutine 等模块共同持有的共享 capability。若 Handle 是 move-only，多观察者需要在 Runtime 外再包装共享所有权；若允许复制但每份复制形成状态快照，则任务终态和控制操作可能分叉。

### Decision

`TaskHandle<T>` 必须可复制、可移动。每个有效副本必须表示同一个 Task Identity 并共享同一个任务完成状态；复制 Handle 不得复制、重新提交或重新执行任务。

### Invariants

- 复制构造或复制赋值不得创建新的 Task Identity、Task Control Block 或调度工作。
- 任意有效副本观察到的任务身份和完成发布必须来自同一个共享任务状态。
- 一个 Handle 副本的销毁不得使其他有效副本失效。
- 后续定义的状态、结果或控制操作必须作用于同一个底层任务，而不得由每个副本维护彼此独立的任务状态。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| v0.1.0 | 基础结果等待与异常观察 | 多个 Handle 副本关联同一任务完成状态 |
| 后续版本 | Cancellation、DAG、Coroutine 与监督能力 | 在同一 Task Identity 上扩展观察和控制 |
| Handle copy | 复制公共 capability | 不产生新的提交或执行 |
| Handle move | 转移一个 capability 实例 | 不改变底层任务身份或状态 |

### Rationale

AstraScheduler 的 Task Handle 不只是单次结果容器，而是贯穿多个 Runtime 模块的任务 capability。把共享语义内建到公共类型，能避免调用方使用额外 `shared_ptr` 包装并形成非标准外部协议，也与 Task Control Block 独立存活的既有取消和完成模型一致。

### Rejected alternatives

- move-only `TaskHandle<T>`：减少引用计数，但多观察者、监督线程和跨模块控制需要额外的外部共享封装。
- 复制 Handle 时复制状态快照：副本会对状态和完成产生不一致观察，也无法正确表达同一任务的取消竞态。
- 复制 Handle 时重新提交任务：违反 Handle 表示任务身份的领域含义，并可能重复执行有副作用的 Callable。

### Consequences

- Task Control Block 需要引用计数或等价共享生命周期机制。
- Handle 复制成本进入 API 性能模型，但不应成为 Worker 调度热路径的必需操作；需要 Benchmark 单独测量。
- 结果消费、多个线程并发调用、全部 Handle 消失后的任务行为和取消仲裁仍需单独决定。

### Non-goals and deferred risks

- 本决策不定义多个线程同时调用同一或不同 Handle 副本的方法是否安全。
- 本决策不定义 `get()` 返回值类别、读取次数或 move-only result 的提取方式。
- 本决策不定义最后一个 Handle 销毁是否影响尚未完成的任务。
- 本决策不固定引用计数实现、对象尺寸或内存分配策略。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 `TaskHandle<T>` 可复制、可移动，并要求全部副本表示同一个任务和同一个共享完成状态。
- Code or data evidence: D-041 已固定 TaskHandle 为跨版本稳定公共任务抽象；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-043 — 最后 TaskHandle 销毁不隐式取消任务

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-042 允许多个 `TaskHandle<T>` 副本共享一个任务，但尚未决定最后一个副本销毁是否构成隐式取消。若引用计数归零自动取消，忽略 `submit()` 返回值、临时 Handle、DAG 内部节点或 Coroutine resume task 可能仅因没有外部观察者而改变执行结果；该竞态还会把对象生命周期意外提升为 Cancellation Policy。

### Decision

最后一个 `TaskHandle<T>` 销毁只放弃调用方对任务的观察与控制 capability，不得隐式取消任务、发布 stop request 或改变任务状态。已经被 Scheduler 接受的 Unobserved Task 必须继续由 Runtime 持有执行责任并推进到正常终态，除非显式取消或 Scheduler Shutdown 按各自已确认规则改变其策略。

### Invariants

- Task Handle 引用计数归零不得成为任务取消或 stop request 的触发条件。
- Runtime 不得因任务失去全部外部 Handle 而从队列、DAG execution state 或其他调度结构中静默丢弃它。
- Unobserved Task 与有 Handle 的同类任务必须遵守相同的启动、执行、异常捕获和 Shutdown 规则。
- Runtime 对已接受任务的执行责任必须持续到该任务进入一个真实终态。
- 显式 Task Cancellation 与 Scheduler Graceful/Immediate Shutdown 仍可按各自规则影响 Unobserved Task，不因无 Handle 而获得特殊豁免。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 最后 Handle 销毁 + Waiting/Ready | 已接受但未运行 | 任务仍保留在 Runtime 核算中并可正常执行 |
| 最后 Handle 销毁 + Running | 正在执行 | 不发布隐式 stop request，Callable 继续 |
| fire-and-forget submit | 返回 Handle 被立即丢弃 | 已接受任务仍执行到终态 |
| 显式取消或 Shutdown | 独立控制路径 | 继续使用对应取消和关停规则 |

### Rationale

提交被接受后，执行责任属于 Runtime，而不是由结果观察者的引用计数决定。将 Handle 销毁与取消分离，可以让生命周期竞态不改变业务副作用，并保持“已接受任务不能静默丢弃”的已有不变量；需要取消的调用方使用显式能力表达意图。

### Rejected alternatives

- 最后 Handle 销毁自动请求取消：临时 Handle 和忽略返回值会产生隐式且时序敏感的任务取消。
- 未开始任务自动取消、Running Task 继续：仍让相同 Handle 操作根据调度竞态改变业务结果。
- 无 Handle 后直接丢弃任务或异常：会破坏已接受任务的终态与 Shutdown Completion 核算。

### Consequences

- Runtime 必须独立于外部 Handle 引用持有任务控制状态，直到终态发布。
- 调用方丢弃全部 Handle 后无法再直接读取结果或异常；未观察异常的 Metrics/Trace 与诊断策略需单独决定。
- `submit()` 返回值可以被用于 fire-and-forget，但是否标记 `[[nodiscard]]` 仍需作为公共 API 选择确认。

### Non-goals and deferred risks

- 本决策不定义未观察 Task 异常的日志、Metrics 或 terminate 策略。
- 本决策不定义 TaskHandle 方法的并发调用保证。
- 本决策不定义显式 Task Cancellation Interface 或取消成功条件。
- 本决策不固定任务终态后无观察者时结果存储的释放时机。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受最后一个 `TaskHandle<T>` 析构没有取消副作用，已接受任务继续由 Runtime 推进到真实终态。
- Code or data evidence: D-002/D-003/D-006 已要求已接受任务进入可观察终态而不是静默丢弃；D-041/D-042 已固定共享 TaskHandle 公共抽象。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-044 — Terminal Outcome 共享且可重复观察

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-042 允许多个 `TaskHandle<T>` 副本表示同一个任务。若任务结果采用单消费者模型，第一个观察者会使其他副本失去结果，监督、DAG successor、Coroutine 与普通调用方将争抢唯一消费权；而项目仍需支持 move-only value，因此也不能未经设计就假设每次观察都按值复制 `T`。

### Decision

Task 进入终态后必须发布一个共享、不可变的 Terminal Outcome，恰好表示 Value、Exception 或 Cancelled 三类完成事实之一。任意有效 `TaskHandle<T>` 副本可以重复观察同一个 Terminal Outcome；一次观察不得消费、清除或改写该 Outcome，也不得使其他副本失效。

### Invariants

- 每个已终结 Task 必须只有一个不可逆的 Terminal Outcome。
- Terminal Outcome 一经发布不得从 Value、Exception 或 Cancelled 改成另一类别。
- 所有有效 Handle 副本必须观察同一个 Outcome 身份和内容，而不是每副本持有独立快照。
- 观察操作不得转移底层 value 所有权、清除 exception 或改变 TaskState，除非后续另行定义名称不同的显式提取操作。
- 只要仍存在有效 Handle，保存 Terminal Outcome 的共享状态就必须继续存活。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Value Outcome | Callable 正常返回 | 保存一个不可变成功结果供重复观察 |
| Exception Outcome | Callable 抛出 | 保存同一个失败事实供重复观察 |
| Cancelled Outcome | 任务按取消规则终结 | 保存同一个取消事实供重复观察 |
| 多个 Handle 副本 | 先后观察 | 不竞争唯一消费权，观察顺序不改变 Outcome |

### Rationale

可复制 Task Handle 更适合作为共享完成事实的 capability，而不是多个对象争抢的单消费 token。不可变 Outcome 让状态、异常和取消都保持稳定，并为 DAG、多观察者与 Coroutine 建立统一结果核心；move-only value 的提取需求可以通过后续显式 API 解决，而不污染基础观察语义。

### Rejected alternatives

- 第一个 `get()` 消费 Outcome：后续 Handle 副本会因调用顺序不同而获得不同能力，削弱共享 capability 语义。
- 每个 Handle 复制一份结果：不适用于 move-only value，并增加内存与异常状态分叉。
- Value 可重复、Exception/Cancelled 单次消费：不同终态采用不一致观察协议，调用方难以组合。

### Consequences

- 共享任务状态需要保留 Terminal Outcome，直到最后一个需要它的 Handle 或内部依赖释放。
- `get()` 不能未经额外约束就为任意 move-only `T` 重复按值返回；公开访问形式需要单独决定。
- 同一异常或取消事实可以被多个观察者看到；具体 C++ 异常类型和方法签名仍需确认。

### Non-goals and deferred risks

- 本决策不决定 Value Outcome 通过 `const T&`、Result View、共享所有权对象或其他形式公开。
- 本决策不决定是否提供显式单次 `take()` 以及其成功条件。
- 本决策不定义多个线程同时观察 Terminal Outcome 的方法级并发保证。
- 本决策不定义 Terminal Outcome 的内存布局、variant 实现或分配策略。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 Task 的 Terminal Outcome 是共享、不可变且可重复观察的，任何 Handle 的观察都不消费该 Outcome。
- Code or data evidence: D-006 已固定 Cancelled 是可观察终态；D-042 已固定多个 Handle 共享同一任务完成状态。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-045 — Callable 异常保存并按原始动态类型重复重抛

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-044 已把 Exception 定义为共享 Terminal Outcome 的一种，但尚未决定调用方通过项目错误码、包装异常还是原始异常观察失败。若异常越过 Worker thread entry，线程会终止并可能触发 `std::terminate()`；若统一压缩为项目错误类型，调用方会失去原始动态类型和异常信息。

### Decision

Worker 必须捕获用户 Callable 逃逸出的任何 C++ 异常，并以 `std::exception_ptr` 保存为该 Task 的 Exception Outcome。后续调用已定义的任务值获取操作时，必须通过保存的 `exception_ptr` 重新抛出原始异常；重复观察必须重复抛出同一个异常事实，异常不得越过 Worker entry 或被当作 Reaper 控制面故障。

### Invariants

- 用户 Callable 的异常不得从 Worker loop 或 OS thread entry 逃逸。
- 捕获必须覆盖 `std::exception` 派生类型和其他可由 `std::current_exception()` 保存的 C++ 异常。
- Runtime 不得仅为统一错误类型而丢失原始 exception dynamic type。
- Exception Outcome 一经发布必须继续遵循 D-044 的不可变和可重复观察语义。
- 一个任务抛出异常不得直接使同一 Worker 停止为其他任务提供调度能力。
- 用户 Callable 异常不得进入 D-040 的 Reaper fatal path。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Callable 正常返回 | Value Outcome | 不适用异常重抛 |
| Callable 抛出 `std::exception` 派生对象 | Exception Outcome | 保存并按原始动态类型重抛 |
| Callable 抛出非 `std::exception` 对象 | Exception Outcome | 同样通过 `exception_ptr` 保存和重抛 |
| 多个 Handle 先后获取值 | 同一失败任务 | 每次都观察并重抛同一个 Exception Outcome |

### Rationale

`std::exception_ptr` 是 C++20 中跨线程保留任意 C++ 异常的标准机制。保存并重抛原始异常既保护 Worker entry，又让调用方保留现有异常分类和诊断信息；与 D-044 的不可变 Outcome 结合后，多观察者不会争抢或清除异常。

### Rejected alternatives

- 允许异常逃出 Worker：可能终止线程或进程，并破坏 Worker Group 容量与 Runtime 生命周期。
- 把全部异常包装成统一 `task_failed`：隐藏原始动态类型，调用方需要额外的嵌套异常协议。
- 只保存字符串消息或错误码：丢失异常类型、嵌套信息和非 `std::exception` 失败。
- Exception Outcome 只能重抛一次：与 D-044 的共享可重复观察冲突。

### Consequences

- Task Control Block 需要持有 `std::exception_ptr` 直到 Exception Outcome 不再需要。
- 获取任务值的操作不是 `noexcept`，调用方可以捕获 Callable 的原始异常类型。
- `wait()`、状态查询和非抛出式 Outcome inspection 是否传播异常仍需独立确认。
- Unobserved Task 的异常不会因没有 Handle 而自动被重抛；诊断策略仍需决定。

### Non-goals and deferred risks

- 本决策不决定值获取方法的名称、成功返回类型或 move-only value 访问形式。
- 本决策不决定 `wait()` 是否抛出任务异常。
- 本决策不定义 Cancelled Outcome 的 C++ 异常或返回表示。
- 本决策不定义未观察异常的日志、Metrics、Trace 或 process policy。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 Callable 异常由 Worker 捕获为 `std::exception_ptr`，在获取任务值时重复重抛原始异常，且异常不得逃出 Worker。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 682–713 行要求任务异常传播而不退出 Worker；D-040 已将用户 Callable 异常排除出 Reaper fatal path。仓库当前没有实现代码。

### Traceability

- ADR: None
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-046 — TaskHandle get 返回共享 const 引用且不提供 take

Status: superseded

Date: 2026-08-26

Supersedes: None

Superseded by: D-076

### Context

D-042 使 TaskHandle 可复制，D-044 又要求 Terminal Outcome 不被观察操作消费；与此同时，项目希望允许 move-only value。按值返回无法同时支持重复观察和任意 move-only `T`，单次 move-out 则会引入消费权竞态并改变其他 Handle 后续看到的内容。

### Decision

对于非 `void` 的 Value Outcome，`TaskHandle<T>::get() const` 必须返回共享结果对象的 `const T&`，不得复制或移动底层 value；`TaskHandle<void>::get() const` 返回 `void`。基础 TaskHandle Interface 不得提供消费式 `take()`。返回引用的有效期持续到持有该 Terminal Outcome 的最后一个有效 Handle 或等价 owning view 被销毁。

### Invariants

- `get()` 观察 Value Outcome 时不得改变 Terminal Outcome 或底层 `T` 的所有权。
- copyable 和 move-only `T` 必须使用同一个共享 const-reference 访问模型，不得根据 copyability 静默切换消费语义。
- 多个 Handle 先后调用 `get()` 必须引用同一个已存储 value 对象。
- Exception Outcome 上的 `get()` 必须继续遵循 D-045 重抛原始异常；Cancelled Outcome 的行为由后续独立决策固定。
- 从 `get()` 获得的引用不得被文档描述为独立拥有结果生命周期。
- 公共基础 Interface 不得包含会把 Terminal Outcome 改为 ResultTaken 或清空 value 的 `take()`。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| `TaskHandle<T>` + Value | `T` 非 void | `get() const` 返回 `const T&` |
| `TaskHandle<void>` + Value | Callable 正常无值完成 | `get() const` 返回 `void` |
| copyable `T` | 共享观察 | 不因为可复制而每次返回一份副本 |
| move-only `T` | 共享观察 | 可通过 const reference 使用，但不从 Outcome 转移所有权 |
| 需要可转移共享资源 | Callable 设计 | 可由任务显式返回 `std::shared_ptr<U>` 等 shareable value |

### Rationale

共享 const reference 是同时保留 TaskHandle 可复制、Outcome 不可变和 move-only result 支持的最小一致模型，也与 `std::shared_future<T>` 的结果观察方式相近。拒绝基础 `take()` 避免引入新的可变终态和先到先得竞态；真正需要共享资源所有权的任务可以把共享所有权编码进其返回类型。

### Rejected alternatives

- `get()` 每次按值返回 `T`：无法支持任意 move-only result，并会给大型值带来隐式复制。
- 第一次 `get()` move-out：把调用顺序变成消费权竞争，违反 D-044。
- 根据 `T` 是否可复制选择不同 `get()` 语义：模板实例之间行为不一致，泛型代码难以推理。
- 基础 Interface 同时提供 `take()`：需要新增 ResultTaken 状态、唯一消费仲裁和并发失败协议，当前没有已确认用例支撑。

### Consequences

- 调用方必须保证使用返回引用期间至少有一个 owning Handle 或未来定义的 owning view 保持有效。
- 从临时 Handle 调用 `get()` 并把引用保存到完整表达式之外可能形成悬空引用，需要文档、示例和静态分析突出。
- 对需要转移独占资源所有权的任务，推荐把可共享所有权作为返回值类型的一部分，而不是消费 Terminal Outcome。
- `get()` 是否等待、Worker caller eligibility 和方法并发保证仍需独立决定。

### Non-goals and deferred risks

- 本决策不决定 Callable 返回引用类型 `T&` 或 `const T&` 是否受支持。
- 本决策不定义未来 owning Result View 的类型或必要性。
- 本决策不定义 `get()` 在未完成 Task 上的阻塞规则。
- 本决策不定义 Cancelled Outcome 的 `get()` 表示。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受 `TaskHandle<T>::get() const` 对成功结果返回共享 `const T&`，`TaskHandle<void>::get()` 返回 `void`，基础 API 不提供消费式 `take()`。
- Code or data evidence: D-042 固定共享 Handle，D-044 固定非消费式 Terminal Outcome；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-047 — 非 Worker get 使用语义最强的 Unbounded Wait

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-046 已固定 Value Outcome 的返回形式，但尚未决定任务未完成时 `get()` 是返回 not-ready、隐式 timeout 还是同步等待。项目 owner 进一步要求把这种基础同步接口准确称为 Unbounded Wait，以区别于普通的“阻塞一段时间”和后续有界或可取消等待能力。

### Decision

普通非 AstraScheduler Worker 调用 `TaskHandle<T>::get()` 时，若 Task 尚未发布 Terminal Outcome，`get()` 必须执行 Unbounded Wait；只有真实 Terminal Outcome 发布后才结束等待，并按 D-046 返回 Value、按 D-045 传播 Exception，或按后续 Cancelled 规则报告取消。该等待没有 timeout、不会自动取消任务，是 TaskHandle 最基础且完成语义最强的同步结果接口。

### Invariants

- 非 Worker `get()` 不得因等待时长、虚假唤醒或中间 TaskState 提前返回。
- `get()` 不得返回默认值、空引用、not-ready 哨兵或伪造 Terminal Outcome。
- Unbounded Wait 本身不得请求 Task Cancellation、发布 stop request 或改变 Scheduler Shutdown Mode。
- Task 永远无法终结时，非 Worker `get()` 可以永久阻塞。
- 调用时 Terminal Outcome 已发布的 `get()` 必须直接观察该 Outcome，不引入额外等待世代。
- 有界、可取消或异步等待若被提供，必须作为语义不同的独立 Interface 定义，不能削弱本规则的 `get()` 完成边界。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 非 Worker + 未完成 Task | `get()` | Unbounded Wait 到真实 Terminal Outcome |
| 非 Worker + 已完成 Task | `get()` | 立即观察现有 Terminal Outcome |
| 不终结 Task | `get()` | 可以永久阻塞 |
| AstraScheduler Worker | `get()` | 等待/帮助策略由独立决策固定 |

### Rationale

把 `get()` 定义为最强同步接口，使调用方在返回时不需要再判断“是否真的完成”，也与 D-044 的稳定 Terminal Outcome 一致。使用 Unbounded Wait 这一术语明确表达它没有时间边界，而不是实现遗漏 timeout；有界控制由名称和结果都不同的 API 承担。

### Rejected alternatives

- 未完成时返回 not-ready：把轮询和完成判断泄漏给每个调用方，并改变 `get()` 的值获取直觉。
- 内置固定 timeout：没有适用于所有任务的统一时长，还会让同一方法产生不完整结果分支。
- timeout 或长时间等待后自动取消：把观察接口变成 Cancellation Policy。
- 以默认值或空引用表示未完成：无法与合法值区分，并可能产生悬空或未定义行为。

### Consequences

- 非 Worker 调用方只有在愿意接受无界等待时才使用 `get()`。
- 诊断永久等待需要 TaskState、Metrics 与 Trace 支持，但这些能力不改变 `get()` 语义。
- 后续需要分别决定 `wait()`/`wait_for()`、stop-token wait、Coroutine await 及 Worker helping 策略。

### Non-goals and deferred risks

- 本决策不定义 AstraScheduler Worker 调用 `get()` 的行为。
- 本决策不确认任何具体的有界或可取消 TaskHandle 等待 Interface。
- 本决策不定义 Cancelled Outcome 的 C++ 表示。
- 本决策不提供实时返回保证或死锁检测。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 接受非 Worker `get()` 同步等待真实 Terminal Outcome，但要求正式称为 Unbounded Wait，并明确它只作为最基础、语义最强的同步接口。
- Code or data evidence: D-044 至 D-046 已固定 Terminal Outcome、异常传播和成功值访问；仓库当前没有实现代码。

### Traceability

- ADR: None
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-048 — 同 Runtime Worker get 采用 Helping Wait

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-047 只定义普通非 Worker 的 Unbounded Wait。若同 Scheduler Worker 在执行 Parent Task 时提交 Child Task 并直接阻塞等待，Parent 会占住 Worker；当所有 Worker 都如此或 Scheduler 只有一个 Worker时，已入队 Child 无人执行，形成线程饥饿死锁。完全拒绝 Worker 等待可以避免死锁，但会排除自然的 fork-join 调用模式。

### Decision

同 Scheduler Worker 对该 Runtime 中另一个尚未完成的 Task 调用 `get()` 时，必须进入 Helping Wait，而不得把该 OS Worker 专用于普通条件等待或立即以 caller-context 错误拒绝。当前 Callable 保留在调用栈上，Worker 通过同 Runtime 的正常调度路径继续执行 Eligible Task，直到目标 Task 发布 Terminal Outcome 后恢复当前 Callable。

### Invariants

- Helping Wait 不得创建、attach 或 detach 补偿线程。
- Worker 在存在 Eligible Task 时不得仅为等待目标结果而保持阻塞或休眠。
- v0.1.0 必须通过 Global Injection Queue 选择帮助工作；后续版本必须使用其正常 local/global/steal 调度协议，而不是建立只服务等待者的第二套任务队列。
- 执行帮助任务时，WorkerContext 必须切换到被帮助 Task 的真实 Task Identity 和执行上下文，并在该任务退出后恢复外层等待 Task 的上下文。
- Helping Wait 只能在目标 Terminal Outcome 真实发布后按 `get()` 语义恢复外层 Callable，不得伪造完成。
- 没有 Eligible Task 时，Worker 可以使用正常 idle/wakeup 协议等待新工作或目标完成，但不得 busy-spin。
- Helping Wait 不保证目标一定终结；依赖环、不合作任务或永不恢复的异步依赖仍可使等待永久持续。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 同 Runtime Worker + 目标已完成 | `get()` | 直接观察 Terminal Outcome，不进入帮助循环 |
| 同 Runtime Worker + 目标未完成 | `get()` | 进入 Helping Wait 并执行 Eligible Task |
| v0.1.0 | Global Queue Scheduler | 从 Global Injection Queue 帮助执行 |
| 后续 Work-Stealing Runtime | Local/Global/Steal Scheduler | 使用正常调度选择顺序帮助执行 |
| 非 Worker | `get()` | 继续按 D-047 执行 Unbounded Wait |

### Rationale

Helping Wait 让同步 TaskHandle API 与 Work-Stealing/Fork-Join Runtime 的进度模型兼容，尤其避免单 Worker 中 Parent 等 Child 的确定性饥饿。复用正常调度路径可保持 Priority、Metrics 与 Trace 的单一执行模型，而不是用额外补偿线程隐藏阻塞。

### Rejected alternatives

- 同 Runtime Worker 普通阻塞：所有 Worker 都等待队列中任务时会耗尽执行容量。
- 未完成时抛 `std::logic_error`：安全但排除常见 fork-join，并迫使所有依赖改写为 DAG 或 Coroutine。
- 临时创建补偿 Worker：改变固定 Worker Group、线程上限、Metrics 与 Shutdown 核算，并引入线程创建失败路径。
- 只搜索目标任务并内联执行：目标可能有未完成依赖，也会绕过正常 scheduling policy 和队列所有权。

### Consequences

- Worker loop 和 Task execution boundary 必须支持可重入的嵌套调度与 WorkerContext 保存/恢复。
- Trace 需要表达嵌套 Task 执行，Metrics 不能把外层等待时间误算为外层 Task 的 CPU execution time。
- Direct self-wait、间接依赖环、跨 Runtime Worker 等待、Helping Wait 嵌套深度和 Shutdown 交互需要继续决定。
- 压力测试需要覆盖单 Worker、多层 fork-join、异常、取消和大量同时 Helping Wait。

### Non-goals and deferred risks

- 本决策不定义等待当前 Task 自身 Handle 的行为。
- 本决策不提供一般依赖环检测。
- 本决策不定义其他 Scheduler Worker 等待目标 Runtime Task 的行为。
- 本决策不固定帮助循环每次执行的任务数量、victim 顺序或 backoff 常数。
- 本决策不决定 Priority/Deadline 是否允许等待目标获得临时优先级提升。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受同 Scheduler Worker 对另一个未完成任务调用 `get()` 时采用 Helping Wait，而不是阻塞 OS Worker 或立即拒绝。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 244–279、420–515、1794–1825 行包含 Work Stealing 与 Fork-Join 目标；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-049 — Direct Self-Wait 在副作用前抛出 logic_error

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-048 的 Helping Wait 能通过执行其他 Eligible Task 解决 Worker 容量饥饿，但无法推进当前 Task 自己的 Terminal Outcome：该 Outcome 只有在当前 Callable 返回或抛出后才能发布，而 Direct Self-Wait 又阻止 Callable 结束。若继续帮助，只会无限执行无关工作并隐藏确定性的逻辑错误。

### Decision

当前正在执行的 Task 通过表示同一 Task Identity 的任意 `TaskHandle` 副本调用 `get()` 时，Runtime 必须在进入 Helping Wait、条件等待、执行帮助任务或改变任何 Task/Runtime 状态之前，同步抛出 `std::logic_error`。

### Invariants

- Direct Self-Wait 检测必须比较当前 Task Identity 与目标 Handle Task Identity，而不是比较 Handle 对象地址。
- 拒绝必须发生在 Helping Wait 嵌套层级、队列、TaskState、Terminal Outcome、Cancellation State 和 outstanding-work 发生变化之前。
- Direct Self-Wait 不得返回默认值、Cancelled、TimedOut 或伪造的 Terminal Outcome。
- 若 Callable 不捕获该 `std::logic_error`，异常必须按 D-045 成为当前 Task 的 Exception Outcome，不得逃出 Worker。
- 其他 Handle 副本或 Runtime 状态不得因本次错误调用而失效。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 当前 Task + 自身任一 Handle 副本 | `get()` | 无副作用抛出 `std::logic_error` |
| 同 Runtime Worker + 另一个 Task | `get()` | 继续按 D-048 Helping Wait |
| 非 Worker | `get()` | 继续按 D-047 Unbounded Wait |
| 间接依赖环 | A waits B, B waits A | 不由本决策检测或解决 |

### Rationale

Direct Self-Wait 是通过当前 WorkerContext 和 Task Identity 即可确定的不可满足依赖。立即抛出逻辑错误把确定性 hang 转为可测试的任务失败，同时不要求维护完整 wait-for graph，也不改变其他合法 Helping Wait 的 fork-join 能力。

### Rejected alternatives

- 继续 Helping Wait：当前 Task 的 Outcome 仍不可达，只会隐藏错误并执行无关任务。
- 永久阻塞当前 Worker：制造确定性死锁并浪费调度容量。
- 自动把当前 Task 标记 Cancelled：改变 Callable 已经 Running 的终态并违反协作取消边界。
- 调用 `std::terminate()`：显式 `get()` 存在正常异常通道，无需进程级 fail-fast。

### Consequences

- WorkerContext 必须保存当前 Task Identity，且嵌套 Helping Wait 切换时正确更新与恢复。
- TaskHandle `get()` 需要在 Worker 路径的最前端执行 identity check。
- 测试必须覆盖原始 Handle、复制 Handle 和多层 Helping Wait 中的 Direct Self-Wait。

### Non-goals and deferred risks

- 本决策不检测两个或多个 Task 构成的间接等待环。
- 本决策不定义未来 `wait()`、`wait_for()` 或 Coroutine await 对 self-wait 的行为。
- 本决策不固定 `std::logic_error::what()` 文本或项目派生异常类型。
- 本决策不定义跨 Runtime TaskHandle 等待。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受当前 Task 调用自身 Handle 的 `get()` 时，在任何副作用前抛出 `std::logic_error`。
- Code or data evidence: D-042 确保任意 Handle 副本共享同一 Task Identity；D-045 确保未捕获异常成为 Exception Outcome；D-048 定义 Helping Wait。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-050 — Runtime 不保证检测 Indirect Wait Cycle

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-049 可以用当前 Task Identity 常数时间检测 Direct Self-Wait，但 A waits B、B waits C、C waits A 一类间接环需要 Runtime 维护动态 wait-for graph，并在并发插入每条等待边前执行可达性检查。该协议会增加所有 Worker `get()` 的同步、Task Control Block 生命周期和异常/取消清理成本，还需定义检测后由哪个 Task 失败。

### Decision

AstraScheduler Runtime 不保证检测任意动态 Indirect Wait Cycle，也不为基础 TaskHandle 等待维护完整 wait-for graph。D-049 的 Direct Self-Wait 仍必须检测；未被检测的间接环可以使相关 Task 永久处于 Helping Wait。需要在执行前验证无环的显式依赖必须使用 TaskGraph/DAG 的 cycle detection，而不是依赖 TaskHandle 动态等待。

### Invariants

- Runtime 不得宣称 Helping Wait 能保证打破或完成 Indirect Wait Cycle。
- 未检测的间接环不得被伪造为 Cancelled、Failed 或 Completed；没有独立取消/关停介入时，它可以永久不终结。
- D-049 的常数范围 Direct Self-Wait 检测不得因本决策被移除。
- TaskGraph/DAG 的显式依赖必须使用其独立 cycle-detection 契约，不得把本决策解释为 DAG 也允许环。
- Metrics/Trace 可以暴露长时间等待链和 Task Identity，但不得把诊断启发式结果当作强制失败语义。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| A waits A | Direct Self-Wait | 按 D-049 抛出 `std::logic_error` |
| A waits B, B waits A | Indirect Wait Cycle | 不保证检测，可以永久 Helping Wait |
| 更长动态 TaskHandle 环 | Indirect Wait Cycle | 同样不保证检测 |
| TaskGraph/DAG dependency cycle | 显式图结构 | 由 DAG 独立执行 cycle detection |
| opt-in diagnostic tooling | Metrics/Trace/调试能力 | 可以提示疑似等待环，但不改变基础 Runtime 语义 |

### Rationale

通用 wait-for graph 会把低频逻辑错误的全局图维护成本施加到每次 Worker `get()`，并引入新的锁、内存和所有权复杂度。项目已有 TaskGraph/DAG 作为显式依赖与无环验证的正式抽象；TaskHandle 等待保留动态能力，同时明确接受间接环可能不终结的代价。

### Rejected alternatives

- 所有 TaskHandle 等待都维护动态 wait-for graph：增加持续同步成本、可达性算法和边清理协议。
- 检测到环后任意选择一个 Task 失败：结果依赖插边线性化顺序，还需定义已发生副作用的回滚和异常传播。
- Helping Wait 自动执行环中 Task 直到完成：环中每个 Callable 正在等待，执行更多无关任务不能发布其 Outcome。
- 把疑似长等待自动取消：超时不是环证明，会把诊断策略变成任务数据损失策略。

### Consequences

- 用户以 TaskHandle 构造动态等待关系时负责避免间接环。
- 需要结构化依赖和执行前环检查的工作流使用 TaskGraph/DAG。
- Metrics 与 Chrome Trace 应提供 Task Identity、wait target 和等待时长，帮助诊断永久 Helping Wait；具体字段后续决定。
- Cancellation/Shutdown 仍可按各自规则影响环中的 Task，但不构成一般 cycle detection。

### Non-goals and deferred risks

- 本决策不定义 DAG cycle detection 的时间点、算法或错误类型。
- 本决策不定义 opt-in debug wait-for graph。
- 本决策不定义跨 Runtime Indirect Wait Cycle 的额外处理。
- 本决策不提供 deadlock timeout 或自动 Cancellation。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 接受推荐方案：Runtime 不保证检测间接 TaskHandle wait cycle，只检测 Direct Self-Wait；显式依赖环交由 DAG cycle detection。
- Code or data evidence: D-048 定义 Helping Wait，D-049 定义 Direct Self-Wait；`AstraScheduler_项目整体设计.md` 第 799–919 行规划 DAG 与环检测。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-051 — 跨 Runtime 等待只帮助源 Runtime

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Scheduler A 的 Worker 可能在 Task A 中获取 Scheduler B 的 TaskHandle 结果。若把它当普通非 Worker 执行 Unbounded Wait，会占住 A 的执行容量；若让 A Worker 直接执行 B 的任务，则会破坏 WorkerContext、Local Queue、Metrics、Priority、Cancellation 与 Shutdown 的 Runtime 归属。

### Decision

一个 AstraScheduler Worker 等待另一 Runtime 中尚未完成的 Task 时，必须进入 Cross-Runtime Helping Wait：它继续通过正常调度路径执行自己源 Runtime 的 Eligible Task，同时只观察目标 Runtime 的 Terminal Outcome。源 Worker 不得执行、窃取、内联或认领目标 Runtime 的任何任务；目标 Task 必须继续由目标 Runtime 自己的进度能力推进。

### Invariants

- 跨 Runtime 等待不得把源 Worker 注册为目标 Runtime Worker，或改变它所属的 Worker Group。
- 源 Worker 执行的所有帮助任务必须来自源 Runtime，并使用源 Runtime 的正常调度与 WorkerContext 协议。
- 源 Worker 不得从目标 Runtime 的 Global Queue、Local Queue、DAG ready set 或 Coroutine resume queue 获取任务。
- 目标 Terminal Outcome 发布后，源 Worker 必须恢复原始等待 Task，并按该 Outcome 的 `get()` 语义继续。
- 源 Runtime 暂无 Eligible Task 时，可以通过正常 idle/wakeup 协议等待源工作或目标完成通知，不得 busy-spin。
- Cross-Runtime Helping Wait 不创建补偿线程，也不转移两个 Runtime 的任务、Metrics 或 Shutdown ownership。
- 跨 Runtime Indirect Wait Cycle 继续适用 D-050，不保证检测或自动打破。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Runtime A Worker waits Runtime B Task | 目标未完成 | 只帮助 A，观察 B Outcome |
| Runtime A Worker waits Runtime B Task | 目标已完成 | 直接观察 Outcome，不进入帮助循环 |
| Runtime A Worker waits Runtime A Task | 同 Runtime | 按 D-048 Helping Wait |
| 普通非 Worker waits any Task | 无源 Runtime | 按 D-047 Unbounded Wait |
| A 与 B 形成等待环 | 跨 Runtime Indirect Wait Cycle | 不保证检测，可永久不终结 |

### Rationale

Worker 归属是 Local Queue 单 owner、thread-local context、Metrics 和生命周期核算的基础，不能为了等待另一个 Runtime 而临时跨域执行任务。帮助源 Runtime 可以避免源 Worker 容量被同步等待耗尽，同时让目标 Runtime 保持自己的调度和控制策略。

### Rejected alternatives

- 源 Worker 普通阻塞等待目标：大量跨 Runtime 等待会耗尽源 Worker Group。
- 源 Worker 帮助目标 Runtime：破坏队列 owner、Runtime identity、任务归属和关停核算。
- 为跨 Runtime 等待创建补偿线程：改变固定 Worker 拓扑并增加线程创建与 Finalization 复杂度。
- 一律拒绝跨 Runtime `get()`：限制安全的组合场景，且源 Runtime Helping 可以提供明确进度模型。

### Consequences

- 等待实现需要组合源 Runtime work notification 与目标 Task completion notification，具体同步原语属于实现选择。
- Trace 必须保留外层 Task、源 Runtime 和远端 wait target 的身份，避免把帮助执行误归属到目标 Runtime。
- 跨 Runtime 调用方仍负责避免等待环；Runtime 不提供全局 wait-for graph。
- Shutdown/Finalization 中源或目标 Runtime 进入 Stopping 时的帮助边界仍需独立决定。

### Non-goals and deferred risks

- 本决策不定义跨 Runtime 等待环检测或 timeout。
- 本决策不允许 cross-runtime work stealing。
- 本决策不固定通知聚合使用 condition variable、atomic wait 或其他原语。
- 本决策不决定源/目标 Runtime Shutdown 时是否继续 Helping Wait。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确接受其他 Scheduler 的 Worker等待目标 Task 时只帮助自己的源 Runtime，不执行目标 Runtime 的任务。
- Code or data evidence: D-048 已固定同 Runtime Helping Wait，D-050 已排除一般间接环检测；D-009/D-011 已采用 caller-relative Runtime identity。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-052 — 单 Task Cancellation 按启动状态线性化分类

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

TaskHandle 计划承载单任务 Cancellation，但“取消”不能同时表示强杀 Running Callable 和移除尚未开始的任务。D-006/D-007 已为 Immediate Shutdown 建立了按 Task 是否进入 `Running` 分类的安全边界；单 Task Cancellation 需要决定是否复用同一模型，并固定请求与 Task start 的竞态结果。

### Decision

对单个已接受 Task 发出 Task Cancellation Request 时，请求必须与该 Task 进入 `Running` 的转换形成唯一线性化顺序。取消先胜出时，尚未 Running 的 Task 必须恰好一次发布 `Cancelled` Terminal Outcome、唤醒等待者且永不执行 Callable；Task start 先胜出时，请求只能向 Running Task 发布 cooperative stop request，不得强制终止 Worker/Callable或立即伪造 `Cancelled`。Task 已有 Terminal Outcome 时，请求必须为成功、无副作用的幂等 no-op。

### Invariants

- Task start 与未运行取消之间必须只有一个赢家，不得同时发布 `Cancelled` 并执行 Callable。
- 未运行取消必须完成共享 Terminal Outcome 并唤醒全部等待者，不得只从队列或 DAG ready set 删除任务。
- Running Task 的 stop request 不等于 Terminal Outcome，不得立即把 TaskState 改为 `Cancelled`。
- Runtime 不得用强制线程取消、异步异常或进程终止实现单 Task Cancellation。
- 重复或并发 Cancellation Request 不得创建第二个 Terminal Outcome、重复执行 Callable 或反复改变已终结状态。
- 已 Value、Exception 或 Cancelled 的 Task 不得因后来的 Cancellation Request 改写历史 Outcome。
- Cancellation Request 本身不是完成等待；不合作 Running Task 可以永久不终结。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 已接受、尚未 Running | Cancellation wins | `Cancelled` Outcome，Callable 不执行 |
| Ready → Running 先线性化 | Start wins | Running Task 收到 cooperative stop request |
| 已 Running | Cancellation Request | 只发布 stop request，等待 Callable 协作 |
| 已 Terminal | 重复/迟到请求 | 成功、无副作用 no-op |
| Suspended Coroutine | 已开始后挂起 | frame 与 awaiter 取消由独立决策固定 |

### Rationale

按启动状态分类能同时保证未运行任务真正停止和 Running C++ Callable 的 RAII 安全，并与 Scheduler Immediate Shutdown 使用同一 Task Control Block 竞态协议。唯一线性化点消除“报告取消但仍执行”的双重结果，幂等终态也让多个 Handle 副本可以安全表达相同取消意图。

### Rejected alternatives

- 所有状态都立即标记 `Cancelled`：Running Callable 仍可能执行并产生副作用，Outcome 与事实不一致。
- Running Task 强制终止线程：跳过栈展开和 RAII，可能破坏进程内共享状态。
- 取消只设置 stop request，包括尚未运行 Task：未开始任务仍可能被 Worker 取出执行，取消意图缺乏确定效果。
- 已终结 Task 返回错误并改变状态：迫使调用方先做有竞态的状态检查，且破坏 Terminal Outcome 不可变性。

### Consequences

- Task Control Block 需要统一实现 start-vs-cancel 线性化、stop state 和 Terminal Outcome 发布。
- TaskHandle 多副本发出的并发取消请求共享同一个 Task Cancellation 状态。
- Running Task 响应 stop request 后的 Terminal Outcome、公开方法名称/返回类型、Suspended Coroutine 和 DAG 传播仍需继续决定。
- 测试必须覆盖大规模 start/cancel 竞态、重复取消、迟到取消和不合作 Callable。

### Non-goals and deferred risks

- 本决策不固定公开方法名为 `cancel()` 或 `request_cancel()`。
- 本决策不定义取消方法返回 `void`、`bool` 或枚举。
- 本决策不决定 stop-aware Running Callable 返回后的最终 Outcome。
- 本决策不定义 Suspended Coroutine cancellation 或 DAG descendant propagation。
- 本决策不定义 Cancellation Request 的调用线程权限和方法级并发保证。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 预先授权接受当前窗口的推荐方案；推荐将单 Task Cancellation 按是否已 Running 分类，并让请求与 Task start 在唯一线性化顺序中竞争。
- Code or data evidence: D-006/D-007 已固定 Immediate Shutdown 对未运行和 Running Task 的对应语义；D-044 固定 Terminal Outcome 不可变。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0021](../../docs/adr/0021-task-cancellation-is-state-classified-and-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-053 — Task Cancellation 命令在请求可靠发布后立即返回

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-052 已定义取消请求按 Task 启动状态分类，但尚未区分“发布取消意图”和“等待取消真正完成”。Running Task 可以忽略 cooperative stop request；若取消方法同步等待 Terminal Outcome，调用方可能把一个请求操作误解为有界完成，并在 Worker 上形成 self-wait。

### Decision

Task Cancellation 公共命令必须是 request-only 操作：它只等待 D-052 所需的取消/启动线性化、请求状态可靠记录和必要通知完成，然后立即返回；不得等待 Task 发布 Terminal Outcome、Callable 退出或 Worker 执行其他工作。请求返回不表示 Task 已取消或已终结。

### Invariants

- 请求方法不得因为 Running Task 不合作而阻塞到该 Task 终结。
- 返回前，取消意图必须已可靠记录，使后续 Task start、Running Callable 或 Runtime 控制路径能够观察对应结果。
- 请求不得隐式等待、join Worker、执行 Helping Wait 或创建新的完成世代。
- 重复请求必须复用同一个 Task Cancellation State，不得重复执行 Callable 或发布第二个 Outcome。
- 调用方必须通过 Terminal Outcome 或后续观察 Interface 判断最终结果，不能把请求返回当作完成证明。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 尚未 Running | request-only 命令 | 完成取消线性化与 Outcome 发布所需最小工作后返回 |
| Running | request-only 命令 | 发布 cooperative stop request 后返回，不等 Callable |
| Terminal | 重复/迟到请求 | 无副作用立即返回 |

### Rationale

请求与完成观察分离能保持 API 名称、时延和 cooperative cancellation 事实一致，也让 Worker 安全地发出取消意图而不等待自己或其他 Task。真正需要等待的调用方使用 TaskHandle 的 Outcome/等待能力。

### Rejected alternatives

- cancel 同步等待 Terminal Outcome：不合作 Running Task 可使控制命令无限阻塞，并可能形成 Worker self-wait。
- 请求返回即伪造 Cancelled：Running Callable 可能继续产生副作用，完成事实不真实。
- 内置 timeout 后返回失败：把等待策略混入基础请求，并仍需定义后台取消状态。

### Consequences

- Cancellation API 的完成边界短于 Task Terminal Outcome。
- 调用方若需要“请求后等待”，需要显式组合 request 与 `get()`/等待 Interface。
- 精确方法签名由 D-054 固定，调用资格与并发规则由 D-055 固定。

### Non-goals and deferred risks

- 本决策不固定方法名、返回类型或异常规格。
- 本决策不承诺 lock-free、wait-free 或固定响应时延。
- 本决策不决定 Running Task 的最终 Outcome。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐把 Task Cancellation 设计为请求可靠发布后返回、不会等待 Task 完成的 request-only 命令。
- Code or data evidence: D-052 已区分请求与真实终态；D-007 已确认 Running Task 只能 cooperative stop。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0021](../../docs/adr/0021-task-cancellation-is-state-classified-and-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-054 — Task Cancellation 公共签名固定为 request_cancel

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

`bool cancel()` 容易被解释为“Task 最终已 Cancelled”，但 D-052/D-053 允许 Running Task 只收到 stop request，并且请求返回时可能尚未终结。返回 bool 或细分枚举也只是某个线性化时刻的快照，无法保证随后 Terminal Outcome。

### Decision

有效 `TaskHandle<T>` 的公共取消命令必须命名为 `void request_cancel() const noexcept`。基础 Interface 不得提供语义同义的 `cancel()`、返回 `bool`/枚举的取消 overload，或把该命令命名为暗示同步完成的 `cancel_now()`。

### Invariants

- `request_cancel()` 的 `void` 返回只表达命令已按 D-053 完成发布，不编码 Task 最终 Outcome。
- 方法必须为 `const`，因为 Handle capability 本身不因请求而指向另一个 Task。
- 方法必须为 `noexcept`；运行期请求不得依赖动态分配、线程创建或向调用方传播异常。
- 已 Terminal Task 上调用必须为成功、无副作用 no-op。
- 文档不得把 `request_cancel()` 返回描述成“Task 已经停止”。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 任意 `TaskHandle<T>` | 公共命令 | `void request_cancel() const noexcept` |
| 尚未运行、Running、Terminal | 状态差异 | 由 D-052 分类，签名保持一致 |
| 需要完成观察 | 调用方流程 | 另行调用 Outcome/等待 Interface |

### Rationale

`request_` 命名准确表达 cooperative cancellation 的意图与完成边界；`void` 避免制造会立即过期的成功快照，`noexcept` 让取消可安全用于错误路径和 Worker 控制流。最终状态仍由唯一 Terminal Outcome 提供权威答案。

### Rejected alternatives

- `bool cancel()`：无法同时清楚表达 pre-start cancel、Running stop request 和竞态后的最终 Outcome。
- `CancelResult` 枚举：提供更多瞬时细节但仍不是完成证明，并扩大首版 API。
- `cancel_now()`：暗示同步或强制终止，与 cooperative 模型冲突。
- 方法抛异常报告迟到请求：Terminal no-op 是合法幂等清理路径。

### Consequences

- 调用方不会通过返回值判断取消是否最终生效。
- Metrics/Trace 可以记录请求落在哪个 Task 阶段，但不改变公共返回类型。
- moved-from/invalid Handle 的成员调用契约仍需单独决定。

### Non-goals and deferred risks

- 本决策不定义无效 Handle 的行为。
- 本决策不提供带 reason、deadline 或 propagation policy 的 overload。
- 本决策不定义 Coroutine 或 DAG 的传播范围。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐固定 `void TaskHandle<T>::request_cancel() const noexcept`，不提供含义模糊的 bool/enum `cancel()`。
- Code or data evidence: D-052/D-053 已固定状态分类与 request-only 边界。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0021](../../docs/adr/0021-task-cancellation-is-state-classified-and-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-055 — request_cancel 可由任意应用线程并发调用

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

TaskHandle 可复制并会被监督线程、Worker 与其他模块共同持有。若取消只能由非 Worker 或单一 owner 调用，外部需要额外串行化；而 request-only 命令不等待 Task/Worker 完成，不具有同步 shutdown 的 self-wait 风险。

### Decision

任意应用线程都可以在有效 `TaskHandle<T>` 上调用 `request_cancel()`，包括目标 Task 所属 Runtime 的 Worker和其他 Runtime 的 Worker。不同 Handle 副本上的并发 `request_cancel()` 必须数据竞争安全、幂等地作用于同一个 Task Cancellation State，并按 D-052 形成一个有效 start/cancel 线性化结果。

### Invariants

- Worker 调用 `request_cancel()` 不得等待自身、目标 Task 或任何 Runtime Shutdown Completion。
- 并发请求不得发布多个 Terminal Outcome、重复 stop transition 或重新提交任务。
- 一个请求与 Task start 的相对顺序必须对所有 Handle 副本一致可观察。
- 允许并发调用不代表对同一个 Handle 对象同时执行赋值、移动或析构是合法的；对象生命周期同步仍由调用方负责。
- 调用资格不得因目标 Task 与调用 Worker 属于同一或不同 Runtime 而改变。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| 普通应用线程 | `request_cancel()` | 发布请求后返回 |
| 同 Runtime Worker | `request_cancel()` | 同样 request-only，不 self-wait |
| 其他 Runtime Worker | `request_cancel()` | 同样作用于目标 Task State，不跨域执行任务 |
| 多副本并发调用 | 同一 Task | 数据竞争安全、幂等共享结果 |

### Rationale

取消请求是对共享 Task State 的短控制命令，而不是完成等待；因此没有理由因 Worker 身份限制调用。明确跨副本并发安全能让监督、timeout 和业务控制自然组合，同时保留 C++ 对同一对象生命周期并发修改的常规约束。

### Rejected alternatives

- 只允许创建 Handle 的线程取消：无法支持 Handle 共享和监督线程。
- 禁止 Worker 取消：把安全的 request-only 命令与有 self-wait 风险的同步等待混为一谈。
- 要求调用方外部加锁：泄漏 Task Control Block 的线性化协议并增加 check-then-act 竞态。

### Consequences

- Task Cancellation State 需要线程安全的幂等请求协议。
- 压力测试必须覆盖多个线程、同/跨 Runtime Worker 与 Task start 并发。
- TaskHandle 其他方法的并发保证仍需分别确认。

### Non-goals and deferred risks

- 本决策不使同一 Handle 对象的并发 move/assignment/destruction 成为合法用法。
- 本决策不承诺 request_cancel lock-free 或实时有界。
- 本决策不定义请求 reason 或调用者权限系统。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐允许任意应用线程通过不同 TaskHandle 副本并发、幂等地请求取消。
- Code or data evidence: D-042 固定 Handle 可复制共享同一 Task State，D-053 固定 request-only 边界。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0021](../../docs/adr/0021-task-cancellation-is-state-classified-and-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-056 — stop request 不覆盖 Running Callable 的正常返回

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-052 规定 Running Task 收到 cooperative stop request，但尚未决定 Callable 响应请求后返回时发布 Value 还是 Cancelled。若 Runtime 只在 Callable 返回后采样 `stop_requested()` 并强制标记 Cancelled，请求可能在业务副作用已经提交或有效结果已经生成后到达，从而丢弃真实成功事实；若任何提前退出都视为 Value，又无法显式表达协作取消。

### Decision

Running Callable 收到 stop request 后若正常返回，Task 必须发布 Value Outcome，即使返回时 stop 仍为 requested。普通用户异常继续发布 Exception Outcome；只有 Callable 通过后续定义的显式 cooperative cancellation signal 退出时，Task 才发布 Cancelled Outcome。Runtime 不得仅根据完成时的 stop-requested bit 覆盖 Callable 的正常返回或异常。

### Invariants

- stop request 是协作意图，不是 Running Task 的 Terminal Outcome。
- Callable 正常返回必须保留其 value/void 成功事实，不得被迟到请求改写为 Cancelled。
- 普通异常必须继续遵循 D-045，不能因 stop 已 requested 而改成 Cancelled。
- Cancelled Outcome 必须来自未运行取消胜出或 Runtime 识别的显式 cooperative cancellation signal。
- Task 终态分类不得依赖 Worker 在 Callable 返回后对一个可能竞态变化的 stop bit 进行最终采样。
- Terminal Outcome 一经发布继续遵循 D-044，不得被后来的 Cancellation Request 改写。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Running + stop requested + normal value return | Callable 完成 | Value Outcome |
| Running + stop requested + normal void return | Callable 完成 | Value/void Outcome |
| Running + stop requested + ordinary exception | Callable 失败 | Exception Outcome |
| Running + explicit cancellation signal | 协作退出 | Cancelled Outcome |
| 未 Running cancel wins | 任务未开始 | 继续按 D-052 直接 Cancelled |

### Rationale

只有 Callable 知道自己是否在安全取消点放弃了业务结果。正常返回表达工作已经成功完成，Runtime 不能用迟到的外部请求覆盖它；显式 cancellation signal 则让 stop-aware Callable 清楚区分“提前取消”和“收到请求但仍完成”。

### Rejected alternatives

- 返回时 stop requested 一律 Cancelled：可能丢弃已经提交的有效结果并制造时序相关终态。
- Running Task 永远不能是 Cancelled：无法表达 Callable 在安全点主动放弃工作。
- 任何 stop-aware 提前返回自动推断 Cancelled：Runtime 无法从普通返回值判断业务是否完成。
- stop requested 时普通异常改成 Cancelled：掩盖真实失败和诊断信息。

### Consequences

- stop-aware Callable 需要显式选择正常完成还是发出 cancellation signal。
- 公共 cancellation signal 的类型和 helper Interface 需要后续决定。
- Metrics 可以分别统计 stop requested、cooperative cancellation 和 completed-after-stop。
- Cancellation 请求与正常返回竞态不会导致结果被追溯改写。

### Non-goals and deferred risks

- 本决策不固定 cooperative cancellation signal 使用异常、返回类型或 context method。
- 本决策不定义 Suspended Coroutine 的 cancellation signal。
- 本决策不定义 DAG successor 对上游 Cancelled 的处理。
- 本决策不决定 Cancelled Outcome 的 `get()` 表示。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐让 stop request 只表示意图，Running Callable 正常返回保持 Value，普通异常保持 Exception，只有显式协作取消信号形成 Cancelled。
- Code or data evidence: D-007/D-052 已确认 Running stop request 不等于终态，D-044 固定 Terminal Outcome 不可变。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0021](../../docs/adr/0021-task-cancellation-is-state-classified-and-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-057 — get 以 task_cancelled 重复报告 Cancelled Outcome

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-044 固定 Cancelled 是可重复观察的 Terminal Outcome，D-046/D-047 又使 `get()` 成为同步值获取接口。Cancelled 没有 `const T&` 可返回；若用默认值、空引用或 bool 表达，会与合法 Value 混淆，也不能保留 `get()` 只在真实 Outcome 后结束的强语义。

### Decision

AstraScheduler 公共 Interface 必须提供 `astra::task_cancelled` 异常类型。`TaskHandle<T>::get() const` 和 `TaskHandle<void>::get() const` 观察到 Cancelled Outcome 时必须抛出 `astra::task_cancelled`；由于 Outcome 可重复观察，每次 `get()` 都必须独立抛出同一取消类别，不得消费或改变 Outcome。

### Invariants

- `task_cancelled` 必须是可由调用方按类型捕获的公开 C++ 异常，并派生自 `std::exception`。
- Cancelled `get()` 不得返回默认构造 `T`、空引用、false 或 not-ready 状态。
- 抛出 `task_cancelled` 不得改变 TaskState、Terminal Outcome 或其他 Handle 的观察能力。
- Value/Exception Outcome 不得被映射为 `task_cancelled`；分别继续遵循 D-046/D-045。
- `task_cancelled::what()` 的稳定文本和类 ABI 布局不属于当前语义承诺。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| `TaskHandle<T>` + Cancelled | `get() const` | 抛出 `astra::task_cancelled` |
| `TaskHandle<void>` + Cancelled | `get() const` | 同样抛出 `astra::task_cancelled` |
| 多个 Handle 副本 | 重复观察 | 每次调用都报告同一取消类别 |
| Value | `get()` | 返回共享 const value |
| Exception | `get()` | 重抛原始异常 |

### Rationale

专用异常让强同步 `get()` 在没有值时保持类型明确，并与 C++ Future 的异常传播风格一致。取消与普通失败具有不同公共类型，调用方可以分别处理；重复抛出保持 Terminal Outcome 的共享非消费语义。

### Rejected alternatives

- 返回默认值或空引用：无法区分合法值并可能产生未定义行为。
- `get()` 返回 `expected`/variant：改变 D-046 已选的共享引用 Interface，并把所有成功调用变复杂。
- 把取消包装成普通 `runtime_error`：调用方不能可靠区分取消与失败。
- Cancelled 只允许观察一次：违反 D-044。

### Consequences

- `get()` 的三种结果为共享 Value reference、原始 Exception 重抛或 `task_cancelled`。
- 调用方可以用异常边界分别处理业务失败和取消。
- 非抛出式 Outcome inspection 是否提供仍需独立决定。

### Non-goals and deferred risks

- 本决策不固定 `task_cancelled::what()` 文本、构造函数集合或 ABI。
- 本决策不决定 wait/wait_for 是否抛出取消。
- 本决策不定义 Coroutine await 的取消呈现。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐让 `get()` 在 Cancelled Outcome 上重复抛出公开 `astra::task_cancelled`。
- Code or data evidence: D-044 固定可重复 Outcome，D-046/D-047 固定 `get()` 的成功值与完成边界。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0021](../../docs/adr/0021-task-cancellation-is-state-classified-and-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-058 — 逃出 Callable 的 task_cancelled 形成 Cancelled Outcome

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-056 要求 Running Callable 通过显式 cooperative cancellation signal 区分正常完成与取消；D-057 已提供公开 `astra::task_cancelled`。若该类型逃出 Callable 后仍按 D-045 归为普通 Exception，则取消无法沿 `child.get()` 自然传播，也需要第二个内部信号类型和重复异常协议。

### Decision

`astra::task_cancelled` 未被 Callable 捕获并逃出 Task execution boundary 时，Runtime 必须把它识别为 Cancellation Signal并发布 Cancelled Terminal Outcome，不得保存为 Exception Outcome。Callable 可以显式抛出该类型表达协作取消；从被取消 child 的 `get()` 传播出的同一类型也遵循本规则。若 Callable 捕获该异常并随后正常返回，则按 D-056 发布 Value Outcome。

### Invariants

- Task execution boundary 必须在通用 D-045 异常捕获之前识别 `astra::task_cancelled` 类别。
- 逃出的 Cancellation Signal 不得进入 Exception Outcome 或 Reaper fatal path。
- 一个 Task 的 Cancelled Outcome 仍必须恰好发布一次并唤醒所有等待者。
- Callable 捕获信号后继续执行时，Runtime 不得仅因曾抛出该异常而强制取消 Task。
- Cancellation Signal 可以在没有先前 stop request 时显式自取消；stop-requested bit 不是识别该信号的前置条件。
- 取消沿同步 `get()` 传播不等于 DAG descendant propagation，后者需独立规则。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| Callable 显式 `throw task_cancelled{}` | 逃出 boundary | Cancelled Outcome |
| child `get()` 抛 task_cancelled | parent 不捕获 | parent 也发布 Cancelled Outcome |
| Callable 捕获 task_cancelled 后正常返回 | 已恢复 | Value Outcome |
| 普通异常逃出 | Callable failure | 继续按 D-045 Exception Outcome |

### Rationale

同一公共类型既能观察 Cancelled Outcome，也能作为明确的协作退出信号，形成最小而一致的异常协议。允许无预先 stop request 的显式自取消让 Callable 可以基于内部条件放弃工作；只有越过 execution boundary 的信号才决定终态，捕获后恢复仍由 Callable 掌控。

### Rejected alternatives

- task_cancelled 仍作为普通 Exception：取消与失败在父任务中失去传播语义，并需要额外内部信号。
- 只允许 Runtime 内部抛私有信号：用户 Callable 无法明确表达安全取消点。
- 只有 stop requested 时才接受信号：在检查与抛出之间引入竞态，也禁止业务条件触发的自取消。
- 一旦抛出过信号即强制 Cancelled：C++ catch/recovery 将失去意义。

### Consequences

- `task_cancelled` 是控制流异常，代码审查和文档需要区分它与普通业务失败。
- 未捕获的 child cancellation 会通过同步等待自然取消 parent；调用方可 catch 后选择 fallback Value。
- Coroutine cancellation 和 DAG propagation 需要决定是否复用相同信号。

### Non-goals and deferred risks

- 本决策不提供 `throw_if_cancelled()` helper；其 Interface 后续决定。
- 本决策不定义跨语言或无异常构建模式。
- 本决策不定义 DAG 静态依赖传播。
- 本决策不固定异常对象是否携带 reason。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐让逃出 Callable 的 `astra::task_cancelled` 成为 Cancelled Outcome，而不是普通 Exception，并允许同步等待自然传播取消。
- Code or data evidence: D-045 固定普通异常捕获，D-056 要求显式 cancellation signal，D-057 固定公开取消异常。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0021](../../docs/adr/0021-task-cancellation-is-state-classified-and-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-059 — submit 仅在普通调用不可行时注入 stop_token

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

stop-aware Callable 需要获得 Task 自己的 `std::stop_token`。新增 `submit_with_context()` 会分裂提交 Interface，TLS `this_task` 会隐藏依赖并扩大调用上下文；自动注入 token 则必须处理普通 Callable、overload set 与 generic lambda 同时可调用时的歧义。

### Decision

`submit(F, Args...)` 必须先判断 `F` 是否可按 `std::invoke(F, Args...)` 调用；若可以，Runtime 必须选择普通调用且不得注入 token。仅当普通形式不可调用、而 `std::invoke(F, std::stop_token, Args...)` 可调用时，Runtime 才把该 Task 的 stop token 作为第一个参数注入。若两种形式都不可调用，`submit` 必须在编译期不可用或产生清晰约束诊断。

### Invariants

- 选择必须完全由编译期 invocability 决定，不得在运行期猜测 Callable 意图。
- 两种形式都可调用时必须稳定优先无 token 形式，generic/variadic Callable 不得意外收到隐藏参数。
- 注入只发生在第一个 Callable 参数位置，不得搜索或填充任意位置的 `std::stop_token`。
- 注入的 token 必须属于目标 Task 的 Cancellation State，并在 Task 可能进入 `Running` 前建立。
- `TaskHandle<T>` 的 `T` 必须由实际被选择的 invocation result 推导。
- 调用方显式把 `std::stop_token` 放入 `Args...` 且普通形式可调用时，该 token 是普通用户参数，Runtime 不得再注入第二个 token。
- v0.1.0 与后续调度版本必须保持相同选择规则。

### Scope and variants

| Variant | `F(Args...)` | `F(stop_token, Args...)` | Selected form |
|---|---:|---:|---|
| 普通 Callable | yes | no/yes | ordinary, no injection |
| 明确 stop-aware Callable | no | yes | inject Task stop token first |
| generic Callable 两者均可 | yes | yes | ordinary, no injection |
| 两者都不可调用 | no | no | compile-time rejection |

### Rationale

fallback-only 注入保留设计文档中自然的 stop-token Callable 写法，同时给 overload/generic Callable 一个稳定、不惊讶的优先级。Task token 作为标准 `std::stop_token` 进入用户代码，不需要暴露内部 TaskContext 或依赖 thread-local 全局访问。

### Rejected alternatives

- 总是优先 token 形式：generic lambda 和宽泛 overload 会在升级 Runtime 后静默改变被调用签名。
- 只支持显式 `submit_with_token()`：分裂 submit 家族并让通用包装代码增加分支。
- 使用 TLS `this_task::stop_token()` 作为唯一入口：隐藏依赖，非 Task 上下文行为还需额外协议。
- 在参数列表任意位置匹配 token：重载决议和诊断不可预测。

### Consequences

- stop-aware Callable 通过要求首参数 `std::stop_token` 且不提供无-token 可调用形式显式选择注入。
- submit constraints 和 result type traits 需要覆盖两个 invocation form。
- 文档必须说明 generic Callable 默认不收到 token；需要 token 时使用明确签名 wrapper。
- `throw_if_cancelled(token)` helper 仍需独立决定。

### Non-goals and deferred risks

- 本决策不提供通用 TaskContext 参数注入。
- 本决策不定义 Callable 参数存储、decay 或 reference_wrapper 规则。
- 本决策不定义无异常构建模式。
- 本决策不决定 Coroutine promise 如何获得 stop token。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐让 submit 优先普通 invocation，仅在其不可行且 stop-token 形式可行时注入 Task stop token。
- Code or data evidence: `AstraScheduler_项目整体设计.md` 第 747–799 行展示 stop-token Callable；D-052 至 D-058 已建立单 Task Cancellation State 与 Signal。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0021](../../docs/adr/0021-task-cancellation-is-state-classified-and-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-060 — throw_if_stop_requested 提供显式协作取消点

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-059 让 stop-aware Callable 获得标准 `std::stop_token`，D-058 又允许通过 `task_cancelled` 明确发出 Cancellation Signal。若每个 Callable 手写 token 检查和 throw，代码容易遗漏或使用不一致异常；TLS helper 则会隐藏 token 来源和调用上下文。

### Decision

公共 Interface 必须提供 `void astra::throw_if_stop_requested(std::stop_token token)`。该函数在单一观察点发现 `token.stop_requested()` 为 true 时必须抛出 `astra::task_cancelled`，否则立即返回；它不得读取 TLS TaskContext、请求取消、等待 Task 或改变任何 Runtime 状态。

### Invariants

- helper 的行为必须只取决于传入 token 在本次观察点的 `stop_requested()` 结果。
- 返回只表示本次检查没有观察到请求，不保证随后不会收到请求。
- 抛出的类型必须与 D-057/D-058 使用的 `astra::task_cancelled` 完全相同。
- helper 不得隐式获取当前 Task token或验证 token 属于哪个 Runtime。
- helper 不得分配 Runtime 资源、创建线程或修改 Task Cancellation State。
- 函数不能标记 `noexcept`，因为 requested 分支按契约抛出。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| token 未 requested | helper call | 立即返回 |
| token 已 requested | helper call | 抛出 `astra::task_cancelled` |
| request 与检查竞态 | 单次观察点 | 由唯一实际观察结果决定；调用方可在下一安全点重试 |
| 非 Task token | 标准 stop_token | 同样按传入 token 状态处理 |

### Rationale

显式 token 参数使依赖可见并易于单元测试，函数名直接说明可能抛出以及判断条件。它把重复样板集中起来，但不制造比 `std::stop_token` 更强的时序或所有权承诺。

### Rejected alternatives

- `this_task::cancellation_point()` TLS helper：隐藏依赖，并需定义非 Task 线程和嵌套 Helping Wait 上下文。
- helper 返回 bool：与 `stop_requested()` 重复，不能直接形成 D-058 的 Cancellation Signal。
- helper 自动调用 `request_cancel()`：把观察当前请求与产生新请求混为一谈。
- 在请求到达后保证后续所有检查都抛出以外的额外同步：标准 stop_token 已提供单调 requested 事实，无需新协议。

### Consequences

- stop-aware Callable 可以在每个资源安全点用统一方式转入 Cancelled Outcome。
- helper 可独立于 Scheduler 用普通 `std::stop_source` 测试。
- Coroutine cancellation point 是否复用此函数需在 Coroutine 设计中确认。

### Non-goals and deferred risks

- 本决策不提供 TLS/current-task token accessor。
- 本决策不规定检查频率或最大取消响应延迟。
- 本决策不定义异步回调、stop_callback 或 cancellation reason。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐提供显式 `throw_if_stop_requested(stop_token)` helper，在观察到请求时抛出 `task_cancelled`。
- Code or data evidence: D-057/D-058 固定取消异常与 Signal，D-059 固定 Task token 注入。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0021](../../docs/adr/0021-task-cancellation-is-state-classified-and-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-061 — TaskHandle wait 只同步 Terminal Outcome 而不传播结果

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

`get()` 同时承担完成同步和 Value/Exception/Cancelled 传播，但监督、批量协调和状态采样经常只需要知道 Task 是否已经终结。若纯等待也重抛任务异常，调用方必须在不关心结果时仍建立异常处理；若只等待队列为空或中间状态，又会弱化 Terminal Outcome 边界。

### Decision

有效 `TaskHandle<T>` 必须提供 `void wait() const`。合法调用只在目标 Task 已发布真实 Terminal Outcome 后返回；Value、Exception 和 Cancelled 三类 Outcome 都使 `wait()` 正常返回，`wait()` 不返回 value、不重抛 Callable 异常、不抛 `task_cancelled`，也不消费或改变 Outcome。

### Invariants

- `wait()` 不得以 Ready、Running、Suspended、队列为空或 stop requested 代替 Terminal Outcome。
- Task 已 Terminal 时，`wait()` 必须立即正常返回，不重复发布完成。
- `wait()` 返回后，后续任意 Handle 的 `get()` 仍必须完整观察原 Terminal Outcome。
- `wait()` 不得因 Outcome 类别不同而改变返回类型或普通完成路径。
- `wait()` 本身不得请求取消、改变 TaskState 或创建新的 completion generation。
- 方法不标记 `noexcept`，因为 Direct Self-Wait caller error 由 D-062 报告。

### Scope and variants

| Terminal Outcome | `wait()` result | Later `get()` |
|---|---|---|
| Value | normal return | shared value reference |
| Exception | normal return | rethrow original exception |
| Cancelled | normal return | throw `task_cancelled` |

### Rationale

等待完成与读取结果分离符合 C++ Future 风格，也让监控代码无需为业务异常建立空 catch。Terminal Outcome 仍是唯一完成事实，纯等待不会损坏共享非消费式结果。

### Rejected alternatives

- `wait()` 重抛 Exception/Cancelled：与 `get()` 重复并使纯同步难以组合。
- `wait()` 返回 TaskState：把状态查询与同步绑定，且状态可能扩大。
- `wait()` 在 stop requested 时提前返回：stop request 不是终态。
- 不提供 `wait()`：迫使所有只关心完成的代码调用可能抛异常的 `get()`。

### Consequences

- TaskHandle 同时提供最强结果接口 `get()` 和纯同步接口 `wait()`。
- caller-relative 等待进度与 Direct Self-Wait 由 D-062 固定。
- 有界 `wait_for()` 仍需独立定义。

### Non-goals and deferred risks

- 本决策不定义 `wait()` 的非 Worker/Worker 执行策略。
- 本决策不提供 stop-token 可取消 wait。
- 本决策不定义状态查询 API。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐提供只同步真实 Terminal Outcome、从不传播任务 Outcome 的 `void wait() const`。
- Code or data evidence: D-044 固定 Terminal Outcome，D-045/D-057 固定 `get()` 的异常与取消传播。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-062 — TaskHandle wait 复用 caller-relative Helping Wait

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-061 定义了 `wait()` 的 Outcome 边界，但若 Worker 上的 `wait()` 使用普通条件等待，仍会产生与 `get()` 相同的 Worker starvation；若 `wait()` 和 `get()` 对同一 caller 使用不同进度模型，调用方只因是否读取结果就获得不同死锁行为。

### Decision

TaskHandle `wait()` 必须复用 `get()` 的 caller-relative 等待策略：普通非 Worker 对未完成 Task 执行 Unbounded Wait；同 Runtime Worker 执行 D-048 Helping Wait；其他 Runtime Worker 执行 D-051 Cross-Runtime Helping Wait。当前 Task 等待自己的 Handle 时必须按 D-049 在任何等待或帮助副作用前抛出 `std::logic_error`。

### Invariants

- `wait()` 与 `get()` 对相同 caller/target 组合必须使用相同进度能力。
- Worker `wait()` 不得退化为专用 OS-thread 条件等待或创建补偿线程。
- Cross-Runtime `wait()` 不得执行目标 Runtime 的任务。
- Direct Self-Wait 检测必须比较 Task Identity，并且拒绝不得改变 Outcome、队列或 Helping depth。
- Indirect Wait Cycle 继续遵循 D-050，不保证检测。
- `wait()` 完成后按 D-061 正常返回，不读取或传播 Outcome。

### Scope and variants

| Caller | Target | `wait()` progress model |
|---|---|---|
| 非 Worker | 任意 Task | Unbounded Wait |
| Worker A | 同 Runtime 其他 Task | Helping Wait in A |
| Worker A | Runtime B Task | Cross-Runtime Helping Wait in A |
| 当前 Task | 自身 Handle | `std::logic_error` before side effects |

### Rationale

`wait()` 和 `get()` 的差异只应是完成后是否传播结果，而不应改变 Scheduler 进度和死锁风险。复用既有 Helping Wait 协议也避免维护第二套 Worker wait loop。

### Rejected alternatives

- Worker `wait()` 普通阻塞、`get()` Helping：纯等待更容易耗尽 Worker，行为不一致。
- 所有 Worker `wait()` 拒绝：只因不读取结果就失去已确认的 fork-join 能力。
- Worker `wait()` 帮助目标 Runtime：破坏 D-051 的 Worker 归属。

### Consequences

- Helping Wait implementation 可以由 `wait()`/`get()` 共享内部深模块。
- Direct self、跨 Runtime 和间接环测试必须同时覆盖两种 API。
- 有界等待的 Worker 进度模型仍需独立决定。

### Non-goals and deferred risks

- 本决策不定义 `wait_for()` 或 stop-token wait。
- 本决策不固定 Helping Wait 的公平性、batch size 或嵌套上限。
- 本决策不改变 Scheduler Shutdown API 的 Worker 拒绝规则。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐让 TaskHandle wait 复用 get 的非 Worker Unbounded Wait、同/跨 Runtime Helping Wait 与 Direct Self-Wait 规则。
- Code or data evidence: D-047 至 D-051 已固定 get 的 caller-relative进度模型，D-061 固定 wait 的 Outcome 语义。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-063 — TaskHandle wait_for 返回 Completed 或 TimedOut 且不传播 Outcome

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

`wait()` 提供最强的无界完成同步，但监督循环、UI 主循环和受响应时限约束的调用方需要在不改变 Task 的前提下有界观察完成。若用 `bool` 表示结果会弱化含义；若 timeout 自动请求取消，则一次观察操作会隐式改变执行策略。

### Decision

公共 Interface 必须提供 `enum class TaskWaitResult { Completed, TimedOut };`，并在有效 `TaskHandle<T>` 上提供：

```cpp
template<class Rep, class Period>
[[nodiscard]] TaskWaitResult
wait_for(std::chrono::duration<Rep, Period> timeout) const;
```

目标 Task 的真实 Terminal Outcome 在本次等待中先被观察到时返回 `Completed`，等待时限先到时返回 `TimedOut`。Value、Exception 与 Cancelled 都统一构成 `Completed`；`wait_for()` 不返回 value、不传播 Callable 异常、不抛 `task_cancelled`，也不消费或改变 Outcome。`TimedOut` 只结束本次观察，不请求取消、不伪造终态，后续 `wait()`、`wait_for()` 或 `get()` 仍观察同一个 Task。

### Invariants

- `Completed` 只表示 Terminal Outcome 已真实发布，不表示 Task 成功返回 Value。
- `TimedOut` 不得改变 TaskState、Cancellation Request、队列归属或 Terminal Outcome。
- 返回 `TimedOut` 后 Task 必须继续遵循原调度与取消规则，允许随后变成 Value、Exception 或 Cancelled。
- `wait_for()` 返回 `Completed` 后，后续 `get()` 必须重复观察原 Outcome。
- `TaskWaitResult` 不与进程级 `FinalizationWaitResult` 混用，二者属于不同完成域。
- 方法不标记 `noexcept`，因为 D-065 的 Direct Self-Wait caller error 可以抛出。

### Scope and variants

| Observed event | Result | Task side effect |
|---|---|---|
| Terminal Outcome first | `Completed` | none |
| deadline first | `TimedOut` | none; Task continues |
| Exception/Cancelled Outcome | `Completed` | no propagation |

### Rationale

显式枚举保留结果含义和未来诊断扩展空间，同时将“观察时限”与“取消策略”解耦。它与 D-061 的纯同步语义一致，只把无界等待缩短为一次有界观察。

### Rejected alternatives

- `bool wait_for(...)`：`false` 容易与无效 Handle、取消或调用错误混淆。
- timeout 自动 `request_cancel()`：把只读同步变成策略变更，产生竞态和意外取消。
- timeout 构造 Timeout Terminal Outcome：伪造并覆盖任务之后的真实结果。
- `std::future_status`：把标准 Future 的 deferred 等无关状态引入自定义 Task 模型。

### Consequences

- 调用方可以轮询完成进度而不接触业务 Outcome。
- 调用方必须把 `Completed` 后的 Value/Exception/Cancelled 区分留给 `get()` 或未来的 Outcome inspection。
- duration 时钟与边界线性化由 D-064 固定；Worker 进度模型由 D-065/D-066 固定。

### Non-goals and deferred risks

- 本决策不提供 `wait_until()`、stop-token overload 或 Coroutine await。
- 本决策不定义 moved-from/invalid Handle 的错误行为。
- 本决策不定义非抛出式 Outcome inspection。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐以 `TaskWaitResult::Completed/TimedOut` 提供纯观察式 TaskHandle 有界等待，超时不取消也不伪造完成。
- Code or data evidence: D-044 固定 Terminal Outcome，D-061 固定纯同步等待边界。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-064 — TaskHandle wait_for 使用 steady clock 和唯一完成—期限顺序

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

相对 duration 可能遇到系统墙钟调整，Terminal Outcome 发布又可能与 deadline 同时发生。若时钟来源和竞态优先级未定义，相同场景可能跨平台返回冲突结果，零或负 duration 也无法形成稳定测试。

### Decision

`TaskHandle::wait_for(timeout)` 必须基于单调的 `std::chrono::steady_clock` 计算 deadline。timeout 小于或等于零时执行一次无副作用即时观察：Terminal Outcome 已发布则返回 `Completed`，否则返回 `TimedOut`。对于正 timeout，Terminal Outcome 发布与 deadline 到达必须在同一同步域内形成唯一观察顺序：先观察到 Outcome 则返回 `Completed`；先在线性化观察点确认期限已到且 Outcome 尚未发布则返回 `TimedOut`。

### Invariants

- duration 转换、deadline 计算和加法必须避免溢出；超大正 duration 饱和到可表示的最晚 steady deadline，不得环绕为已超时。
- 已 Terminal 的 Task 对任意 duration 都返回 `Completed`。
- 非正 duration 不阻塞、不帮助执行任务，也不请求取消。
- 一旦本次调用线性化为 `TimedOut`，即使 Outcome 在方法实际返回前发布，本次仍返回 `TimedOut`。
- 一旦本次调用观察 Outcome 并线性化为 `Completed`，不得因随后读取到 deadline 而改写结果。

### Scope and variants

| timeout / race | Result rule |
|---|---|
| `timeout <= 0`, already Terminal | `Completed` |
| `timeout <= 0`, incomplete | `TimedOut` |
| Outcome observed before deadline | `Completed` |
| deadline linearized while incomplete | `TimedOut` |
| very large positive duration | saturating steady deadline |

### Rationale

steady clock 不受系统时间回拨或校时影响；唯一线性化顺序让竞态成为可测试契约，而不是实现偶然。非正 duration 作为即时观察符合标准库等待惯例，但仍保留 D-065 的 caller validity 检查。

### Rejected alternatives

- `system_clock` deadline：墙钟调整会改变相对等待长度。
- deadline 到达后一律返回 `TimedOut`：可能忽略已经发布并被观察到的完成。
- 完成与 deadline 竞态未指定：调用方和测试无法推理。
- 负 duration 视为无界等待：违背 `wait_for` 的有界含义。

### Consequences

- 内部共享完成原语必须支持 Outcome publication 与 timed observation 的一致顺序。
- 测试需要可控时钟或同步 seam 覆盖边界竞态与 duration 饱和。
- 正 duration 的实际回程延迟边界由 D-066 说明。

### Non-goals and deferred risks

- 本决策不承诺硬实时 deadline。
- 本决策不增加绝对时间 `wait_until()`。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 Task 等待复用单调时钟、非正即时观察及唯一完成—期限线性化规则。
- Code or data evidence: D-036 已为 Finalization 有界等待建立同一类时钟与边界契约；D-063 定义 TaskWaitResult。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-065 — TaskHandle wait_for 复用 caller-relative 进度并优先拒绝 Direct Self-Wait

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

若 Worker 的有界等待退化为普通条件等待，短 timeout 仍可能在高并发和嵌套 fork-join 中反复闲置执行资源；若 `wait_for(0)` 绕过 Direct Self-Wait 检查，同一 Interface 又会兼具状态查询和非法等待两种上下文语义。

### Decision

正 duration 的 `TaskHandle::wait_for()` 必须复用 D-062 的 caller-relative 进度模型，区别只是达到 deadline 时允许返回 `TimedOut`：普通非 Worker 有界阻塞观察；同 Runtime Worker 在自己的 Runtime 执行 Helping Wait；其他 Runtime Worker在自己的源 Runtime 执行 Cross-Runtime Helping Wait。当前 Task 对自身 Handle 调用 `wait_for()` 时，必须在读取 duration、即时观察、等待或帮助副作用前抛出 `std::logic_error`，即使 duration 为零或负值。

### Invariants

- 相同 caller/target 对 `get()`、`wait()` 和正 duration `wait_for()` 使用同一 Runtime 归属与进度能力。
- Worker 不得通过 `wait_for()` 执行目标外部 Runtime 的 Task 或创建补偿线程。
- timeout 到达前的帮助只走源 Runtime 当时合法的正常调度路径。
- 非正 duration 在通过 Direct Self-Wait 检查后执行 D-064 的即时观察，不进入 Helping Wait。
- Direct Self-Wait 拒绝不得返回 `TimedOut`，不得改变队列、Outcome 或 Helping depth。
- Indirect Wait Cycle 继续不保证检测；有界等待可使调用方在 deadline 后退出该动态等待边。

### Scope and variants

| Caller | Target | Positive-duration progress |
|---|---|---|
| 非 Worker | 任意 Task | bounded blocking observation |
| Worker A | same Runtime other Task | Helping Wait in A until Outcome/deadline |
| Worker A | Runtime B Task | Cross-Runtime Helping Wait in A until Outcome/deadline |
| 当前 Task | self Handle, any duration | `std::logic_error` before side effects |

### Rationale

有界性不应以牺牲 Worker 进度为代价。统一进度协议让三个同步 API 的差异只落在结果读取和是否具有 deadline；把零 duration 保留为同一等待 API 而非 Worker-safe query，也避免 caller error 被伪装成 timeout。

### Rejected alternatives

- Worker `wait_for()` 普通阻塞：可在 timeout 窗口内耗尽 Worker Group。
- Worker `wait_for()` 一律拒绝：破坏已经允许的 Task 内有界组合场景。
- `wait_for(0)` 跳过 self-check：将调用错误悄悄变成状态探测。
- Cross-Runtime Worker 帮助目标 Runtime：破坏 WorkerContext 和 Runtime 隔离。

### Consequences

- `get()`、`wait()` 与 `wait_for()` 可以共享同一个参数化内部 wait loop。
- 测试必须覆盖同 Runtime、跨 Runtime、零/负 duration 与 Direct Self-Wait。
- Worker 帮助执行的非抢占特性可能造成 deadline 回程延迟，见 D-066。

### Non-goals and deferred risks

- 本决策不固定 Helping Wait 的公平性、batch size 或嵌套上限。
- 本决策不提供通用 wait-for graph。
- 本决策不改变 Scheduler Shutdown API 的 Worker 调用限制。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 TaskHandle 有界等待延续 caller-relative Helping Wait，并对所有 duration 优先拒绝 Direct Self-Wait。
- Code or data evidence: D-048 至 D-051、D-062 已固定无界等待的进度模型；D-064 固定非正 duration 行为。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-066 — Worker wait_for 的 deadline 是观察边界而非硬返回上限

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Helping Wait 会在等待期间执行普通用户 Callable，而 C++ Task 执行是非抢占的。Worker 可能在 deadline 前合法选中一个 Eligible Task，但该 Callable 在 deadline 后才返回；任何声称方法在 deadline 处强制返回的契约都需要抢占、额外线程或禁止帮助执行。

### Decision

`TaskHandle::wait_for(timeout)` 的 deadline 是下一次可进行完成/超时判定的观察边界，不是硬实时的方法返回上限。非 Worker 可能因 OS 调度在 deadline 后稍晚返回；Worker 在 deadline 前开始执行的每个 helped Callable 运行期间不被抢占，必须在该 Callable 返回或挂起并交还调度权后重新检查目标 Outcome 与 deadline，因此实际返回时间可能超过 timeout，且没有独立于 helped Callable 行为的有限上界。

### Invariants

- Worker 不得为了满足 timeout 强杀、栈展开、迁移或 detach 正在帮助执行的 Callable。
- 每次准备选择下一个 helped Task 前必须检查目标 Outcome 与 deadline；deadline 已线性化到达后不得再启动新的 helped Task。
- helped Callable 交还控制后，必须按 D-064 的唯一顺序重新判断 Outcome 与 deadline。
- `TimedOut` 仍只表示本次观察结束，不对超时期间执行的其他 Task 产生取消。
- Runtime Metrics 和 Trace 必须能区分 nominal timeout 与因 helped Callable/OS scheduling 造成的 return overshoot。

### Scope and variants

| Caller condition | Possible overshoot |
|---|---|
| 非 Worker blocking wait | OS scheduling / wake-up delay |
| Worker between helped Tasks | only scheduler/check overhead |
| Worker inside helped Callable | unbounded until Callable yields/returns |

### Rationale

项目优先保留 cooperative、非抢占执行模型和 Worker 进度。把 timeout 定义为硬返回 SLA 会与这两个基础约束冲突；明确 observation deadline 可避免调用方把 `wait_for` 错当作实时隔离机制。

### Rejected alternatives

- deadline 强制抢占 helped Callable：标准 C++ 无安全通用机制，可能破坏资源不变量。
- Worker 有界等待完全不帮助：重新引入 Worker starvation。
- 为每次有界等待创建补偿线程：线程数量不可控并改变成本模型。
- 隐藏 overshoot：让延迟故障无法解释且测试形成错误假设。

### Consequences

- 需要严格 wall-clock 响应上限的代码不能在 Scheduler Worker 上用普通非抢占 Callable 实现该保证。
- 长任务应主动分段、检查 stop token，或在 Coroutine 设计中通过 suspension point 交还调度权。
- Benchmark 需要分别测量 requested timeout、completion observation latency 和 return overshoot。

### Non-goals and deferred risks

- 本决策不规定单个 Callable 的最大执行时间。
- 本决策不引入时间片抢占或 watchdog kill。
- Coroutine suspension 后的 timed wait 精度由 Coroutine 设计另行固定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐诚实定义 Worker timed Helping Wait 的 deadline 为观察边界，因为 helped Callable 非抢占且可能造成无界回程超时。
- Code or data evidence: D-007/D-052 已拒绝强杀 Running Task，D-048/D-065 要求 Worker 帮助执行。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-067 — TaskHandle 具有显式空状态且 move 转移 Handle 关联

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-042 要求 `TaskHandle<T>` 可复制、可移动，并把 move 描述为转移 capability 实例，但尚未规定默认构造与 moved-from 对象是否关联 Task。若每个 Handle 永远非空，容器、延迟初始化和 move assignment 需要额外 `optional`；若空状态不可查询，调用方只能通过可能抛异常的操作试探。

### Decision

`TaskHandle<T>` 必须支持不关联任何 Task 的显式空状态：默认构造产生空 Handle；从有效 Handle move construction/assignment 会把 Task 关联转移到目标，并使源 Handle 变为空；复制空 Handle 仍为空。公共 `[[nodiscard]] bool valid() const noexcept` 在 Handle 当前关联一个 Task Control Block 时返回 `true`，为空时返回 `false`。

### Invariants

- Handle 空状态不是 Task Identity、TaskState 或 Terminal Outcome，不得创建伪任务。
- 从有效 Handle move 后，目标表示原 Task Identity，源不再表示该 Task；底层 Task 与其他有效副本不受影响。
- self move-assignment 必须保持对象可析构且不产生新的 Task；是否保持原关联由实现遵循常规 self-move 安全契约，但不得影响底层任务执行。
- 默认构造、复制/移动空 Handle、`valid()` 和销毁空 Handle 不得分配、阻塞、提交任务或请求取消。
- `valid()` 只观察 Handle 关联，不表示 Task 是否完成、可运行或已取消。

### Scope and variants

| Operation | Source | Result |
|---|---|---|
| default construction | none | invalid/empty Handle |
| copy | valid | both associate same Task |
| move | valid | destination associates Task; source empty |
| copy/move | empty | destination empty |
| `valid()` | any | association only |

### Rationale

显式空状态符合 C++ 资源 Handle 的常规值语义，并让数组、容器、交换和延迟赋值无需额外包装。单独的 `valid()` 避免把 Handle 是否存在混入 Task 的执行生命周期。

### Rejected alternatives

- 禁止默认构造且 move 后仍保持原关联：move 与 copy 无行为差异，并使通用容器使用不便。
- 用 `TaskState::Invalid` 表示空 Handle：把 Handle 状态污染进 Task 生命周期。
- 空 Handle 隐式表示已取消 Task：伪造 Task Identity 与 Terminal Outcome。
- 仅通过捕获异常判断有效性：控制流和诊断成本不必要。

### Consequences

- 所有需要 Task 的成员函数必须定义空 Handle 行为，见 D-068。
- same-object move/assignment 与方法调用的并发安全仍由后续并发契约固定。
- Handle 的实现通常包含 nullable shared-state pointer 或等价表示。

### Non-goals and deferred risks

- 本决策不固定 Handle 对象尺寸、small-object optimization 或引用计数机制。
- 本决策不提供从原始 Task Control Block 构造 Handle 的公共入口。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 TaskHandle 采用可显式查询的空状态，move 转移关联并使源为空。
- Code or data evidence: D-042 已确认 Handle 可复制、可移动且 move 转移一个 capability 实例。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-068 — 空 TaskHandle 的需任务操作拒绝而 request_cancel 为 no-op

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-067 引入空 Handle，但 `get()`、等待、状态查询和取消的行为不能依赖空指针崩溃或未定义行为。与此同时，D-054 已将 `request_cancel()` 固定为 `noexcept`，不能通过异常报告没有关联 Task。

### Decision

对空 `TaskHandle<T>` 调用 `get()`、`wait()`、`wait_for(...)`、后续确认的 `state()` 或 `id()`，必须在读取任务状态、等待、Helping 或其他副作用前同步抛出 `std::logic_error`。对空 Handle 调用 `request_cancel() const noexcept` 必须为无副作用 no-op；`valid()` 仍按 D-067 返回 `false`。空 Handle 的析构、复制、移动和赋值保持普通值操作。

### Invariants

- 空 Handle 操作不得解引用空共享状态、访问已释放 Task Control Block 或产生数据竞争。
- 拒绝路径不得提交/执行 Task、进入等待、改变取消状态或发出 Metrics 中的 Task 事件。
- 空 Handle 的 `wait_for()` 即使 duration 为零或负值也先抛出，不返回 `TimedOut`。
- 空 Handle 的 `request_cancel()` 不得影响任何 Task，也不得构造全局取消请求。
- `std::logic_error::what()` 文本和是否使用项目派生 caller-error 类型不作为首版稳定 ABI。

### Scope and variants

| Operation on empty Handle | Behaviour |
|---|---|
| `valid()` | `false`, no throw |
| `request_cancel()` | no-op, no throw |
| `get()` / `wait()` / `wait_for()` | throw `std::logic_error` before effects |
| future `state()` / `id()` | throw `std::logic_error` before effects |
| destruction / assignment | ordinary safe value operation |

### Rationale

对读取和等待操作显式抛错能尽早暴露错误的对象流转，而不是把空 Handle 伪装成取消或超时。`request_cancel()` 选择 no-op 是保留已批准 `noexcept` 命令和析构式清理代码可组合性的代价；调用方若需要区分是否有关联，应先使用 `valid()`。

### Rejected alternatives

- 所有空 Handle 操作都是 undefined behaviour：诊断性差，容易成为远距离内存错误。
- `request_cancel()` 抛异常：与 D-054 的 `noexcept` 冲突。
- `request_cancel()` 调用 `std::terminate()`：把可安全忽略的无目标命令升级为进程故障。
- 空 Handle 等待直接视为 Completed/Cancelled：伪造不存在的 Terminal Outcome。

### Consequences

- 公共文档必须突出 `request_cancel()` 对空 Handle 的特殊 no-op。
- 测试需覆盖默认构造、move 后源对象和空 Handle 每个操作。
- `state()` 可以只表示真实 Task 生命周期，而无需增加 `Invalid` 枚举值。

### Non-goals and deferred risks

- 本决策不定义用户违反 same-object 并发修改规则时的行为。
- 本决策不固定异常消息或 source location。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐空 Handle 的任务观察操作抛 `logic_error`，而已固定 `noexcept` 的取消请求安全 no-op。
- Code or data evidence: D-054 固定 `request_cancel() const noexcept`；D-063/D-065 已区分 timeout 与 caller error。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-069 — 公共 TaskState 使用七态稳定生命周期而隐藏内部瞬态

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

总设计早期草案公开 `Created/Waiting/Ready/Running/Suspended/Completed/Cancelled/Failed`，但 `Created` 发生在 Task 被成功接受并返回 Handle 之前，`Completed` 又无法区分“任意终态”与“成功 Value”。若把队列 claim、取消竞争或 Outcome publication 的内部步骤继续加入公共枚举，后续 Chase-Lev、DAG 和 Coroutine 实现会被首版 ABI 锁死。

### Decision

稳定公共生命周期枚举固定为：

```cpp
enum class TaskState {
    Waiting,
    Ready,
    Running,
    Suspended,
    Succeeded,
    Failed,
    Cancelled
};
```

`Waiting` 表示 Task 已接受但因已建模的依赖、时间或外部就绪条件尚不 Eligible；`Ready` 表示已 Eligible、等待被某个 Worker 开始或恢复；`Running` 表示普通 Callable 正在执行，或 Coroutine 的某个 resume segment 正在执行；`Suspended` 表示已经开始的 Coroutine/异步 Task 保留执行状态但当前不 Eligible；其余三态是与 Terminal Outcome 一一对应的终态。创建、admission、enqueue publication、queue claim、start/cancel arbitration、Outcome publishing 等内部瞬态不得加入稳定公共 `TaskState`。

### Invariants

- 成功 `submit()` 返回的普通独立 Task 最早可观察为 `Ready`；失败 admission 不创建可观察 TaskState。
- `Created` 不是公共状态；尚未成功接受的对象没有公共 Task Identity/Handle。
- `Completed` 不是枚举值；成功终态必须称为 `Succeeded`，任意完成的总称仍是 Terminal Outcome。
- `Succeeded`、`Failed`、`Cancelled` 都是单调终态，一旦发布不得转回非终态或彼此改写。
- Cancellation Request/stop requested 不是独立 TaskState；Running Task 收到请求后仍为 `Running`，直到真实 Outcome 发布。
- 内部可以拥有更多同步状态，但不得直接向公共 Interface 泄漏或依赖其数值布局。

### Scope and variants

| Public state | Meaning | Typical producers |
|---|---|---|
| `Waiting` | accepted, not yet Eligible | DAG dependency / timer / async readiness |
| `Ready` | Eligible for start or resume | submit / dependency completion / wake-up |
| `Running` | execution segment active | Worker start/resume |
| `Suspended` | started, frame/state retained, not Eligible | Coroutine/async suspension |
| `Succeeded` | Value Outcome | normal return |
| `Failed` | Exception Outcome | uncaught ordinary exception |
| `Cancelled` | Cancelled Outcome | pre-start cancel / Cancellation Signal |

### Rationale

七态模型覆盖 Callable、DAG 与 Coroutine 的跨版本可观察差异，同时把调度算法瞬态藏在深模块内。用 `Succeeded` 对齐 Value Outcome，避免 `Completed` 同时被理解为成功和任意终结。

### Rejected alternatives

- 保留公共 `Created`：成功 Handle 不需要观察 admission 前内部构造状态。
- 使用单一 `Pending`：无法区分依赖未满足、已 Eligible 和 Coroutine suspension，削弱 Metrics 与 DAG 推理。
- 使用 `Completed` 作为 Value 终态：与 Terminal Outcome 的总完成概念冲突。
- 暴露 Enqueued/Claimed/Publishing/CancelRequested：绑定队列和同步实现，扩大状态竞态面。
- 只公开 `Incomplete/Complete`：无法支持项目明确要求的 DAG、Coroutine 与运行时诊断价值。

### Consequences

- Task Control Block 需要把更细内部状态投影为七个公共状态。
- Coroutine 的具体 `Running/Ready/Suspended` 转换和 Suspended cancellation 仍需在 Coroutine 设计中固定。
- Metrics/Trace 可以使用相同公共词汇，但允许内部事件具有更细粒度名称。

### Non-goals and deferred risks

- 本决策不固定枚举底层类型或数值 ABI。
- 本决策不定义优先级、deadline miss 或 stop requested 的附加查询。
- 本决策不决定 Task 是否可被多次恢复或迁移 Worker。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐公共 TaskState 使用 Waiting/Ready/Running/Suspended/Succeeded/Failed/Cancelled，并隐藏实现瞬态。
- Code or data evidence: 总设计早期状态机包含 Created/Completed；D-044 已固定 Value/Exception/Cancelled Terminal Outcome。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0022](../../docs/adr/0022-public-task-state-hides-scheduler-transients.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-070 — TaskHandle state 返回非阻塞线性化快照

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

公共 TaskState 若只是非原子诊断字段，会出现数据竞争；若 `state()` 等待状态稳定或完成，它又会与 `wait()` 重叠。任何状态快照也可能在返回后立即过时，因此 Interface 必须明确它是瞬时观察而不是后续控制条件。

### Decision

有效 `TaskHandle<T>` 必须提供 `[[nodiscard]] TaskState state() const`。对有效 Handle 的调用必须非阻塞、无 Task/Runtime 副作用，并返回 D-069 七态之一；返回值必须能线性化到调用区间内目标 Task 实际持有该公共状态的某一时刻。多个线程可通过同一未被并发 reassociate 的 Handle 对象或不同 Handle 副本并发调用 `state()`，且不得产生数据竞争。空 Handle 按 D-068 抛出 `std::logic_error`，因此方法不声明 `noexcept`。

### Invariants

- `state()` 不得等待状态改变、帮助执行、请求取消、提升优先级或修改 deadline。
- 返回的非终态可以在方法返回后立即过时；调用方不得依赖“先查 state 再操作”获得原子 check-then-act。
- 对有效 Handle，正常状态读取本身不抛出用户 Callable 异常或 `task_cancelled`。
- `state()` 与 `get()`、`wait()`、`wait_for()`、`request_cancel()` 并发时必须数据竞争安全。
- 对同一 Handle 对象并发 move/assignment 与调用成员函数不由本决策保证；不同副本共享底层 Task 状态仍安全。

### Scope and variants

| Handle / task condition | `state()` behaviour |
|---|---|
| valid + nonterminal | immediate linearizable snapshot |
| valid + terminal | immutable terminal state |
| empty | `std::logic_error` before task access |
| concurrent transition | old or new state, whichever linearizes during call |

### Rationale

非阻塞快照适合诊断、UI 和 Metrics 展示，而同步与结果传播仍由专用方法承担。线性化保证比“best effort”字段更可测试，同时明确返回后可过时，避免引导有竞态的控制逻辑。

### Rejected alternatives

- `state()` 阻塞到 Terminal：与 `wait()` 重复且名称误导。
- 返回非原子缓存：产生数据竞争或观察不存在的组合状态。
- `std::optional<TaskState>` 表示空 Handle：把 Handle validity 混进每次真实 Task 查询；已有 `valid()` 与 D-068 caller error 足够。
- 让 `state()` 自动推进 Scheduler：只读诊断会产生不可见执行副作用。

### Consequences

- 状态投影需要原子或等价同步机制，但不要求每个内部瞬态都原子公开。
- 测试必须用允许 old/new 两种线性化结果的方式验证竞态，不能要求返回后状态仍保持。
- 终态与 Outcome 的 publication 顺序由 D-071 固定。

### Non-goals and deferred risks

- 本决策不提供订阅式 state change notification。
- 本决策不提供 compare-and-act 或 conditional cancellation Interface。
- 本决策不固定内存序实现细节。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 `state()` 是可并发调用的非阻塞线性化快照，空 Handle 仍显式报错。
- Code or data evidence: D-042 固定共享状态，D-068 固定空 Handle 行为，D-069 固定公共状态集合。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0022](../../docs/adr/0022-public-task-state-hides-scheduler-transients.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-071 — Terminal TaskState 与 Terminal Outcome 原子一致发布

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

TaskState 终态和 Terminal Outcome 若独立发布，`state()` 可能先返回 `Succeeded/Failed/Cancelled`，而 `get()`/`wait()` 仍看不到完成；反方向则可能让完成等待者醒来后继续观察 `Running`。这种裂缝会破坏公共完成语义和内存可见性。

### Decision

Terminal Outcome 内容必须先完整构造，再通过同一个不可逆 completion publication 与对应公共终态一致发布：Value 映射 `Succeeded`，Exception 映射 `Failed`，Cancelled 映射 `Cancelled`。任何 `state()` 调用若返回终态，必须已经以 acquire 或等价同步观察到完整 Terminal Outcome；此后同一或其他有效 Handle 的 `wait()` 必须立即返回，`wait_for()` 必须返回 `Completed`，`get()` 必须无额外等待地观察对应 Outcome。等待者通知只能发生在 completion publication 之后。

### Invariants

- 一个 Task 恰好发布一次 Terminal Outcome 和一个对应终态，两者不得分叉。
- 不得先公开终态再构造 value/exception/cancellation payload。
- 不得先唤醒 completion waiter 再发布终态与 Outcome。
- `state()` 可以在线性化于 publication 之前返回最后一个非终态；这不代表 publication 之后状态回退。
- publication 之后所有未来状态观察必须返回同一终态。
- Running Task 的 stop request 不参与终态 publication，继续按 D-056 由真实退出方式决定映射。

### Scope and variants

| Terminal Outcome | Public terminal TaskState | `get()` |
|---|---|---|
| Value | `Succeeded` | return shared value / void |
| Exception | `Failed` | rethrow stored exception |
| Cancelled | `Cancelled` | throw `task_cancelled` |

### Rationale

把终态和 Outcome 视为同一次 publication 的两个投影，可为状态查询、等待与结果读取建立单一 happens-before 边界。调用方无需处理“状态已完成但结果尚未准备”的中间事实。

### Rejected alternatives

- 两个独立原子字段按最终一致性更新：短暂矛盾仍会泄漏到公共 API。
- `state()` 返回终态但允许 `get()` 再等待：弱化终态定义并使组合逻辑复杂。
- 先通知再发布 payload：等待者可能读取未构造数据。
- stop requested 直接公开 `Cancelled`：与 D-052/D-056 的 cooperative 结果语义冲突。

### Consequences

- 内部 completion state 应成为 Outcome、终态映射和 waiter notification 的唯一协调点。
- TSan、竞态压力测试和受控 publication seam 必须验证三种 Outcome 的可见性。
- 公共 `TaskState` 可以可靠用于判断“此刻已经 terminal”，但非终态快照仍不能用于 check-then-act。

### Non-goals and deferred risks

- 本决策不固定 mutex、atomic、futex 或 condition variable 实现。
- 本决策不提供对 Outcome payload 的非抛出式指针/variant 访问。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 TaskState 终态与 Terminal Outcome 通过同一 completion publication 原子一致地对外可见。
- Code or data evidence: D-044 固定 Outcome 不可变且可重复观察；D-061/D-063 固定等待仅以真实 Outcome 为完成。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0022](../../docs/adr/0022-public-task-state-hides-scheduler-transients.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-072 — TaskHandle 共享状态操作支持并发而对象 reassociation 需外部同步

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

TaskHandle 是 copyable shared capability，典型用法会由多个监督者等待、读取状态或请求取消。若只有“不同副本安全”而同一 Handle 的 const 方法不安全，接口难以在共享对象中使用；反之，要求实现把同一 Handle 对象的 move/赋值也与成员调用自动同步，会给每个轻量 Handle 增加不必要成本并偏离 C++ 普通对象的数据竞争规则。

### Decision

在某个 `TaskHandle<T>` 对象本身没有被并发 move、copy/move assignment、swap 或析构的前提下，任意线程必须能够通过同一个 Handle 对象或关联同一 Task 的不同副本，并发调用 `valid()`、`get()`、`wait()`、`wait_for()`、`state()` 与 `request_cancel()`，且不得产生 Runtime 内部数据竞争。对同一个 Handle 对象并发执行会改变或终结其 Task 关联的操作，与任何其他成员访问之间需要调用方外部同步；不同 Handle 副本的生命周期操作彼此独立。

### Invariants

- 并发 const 观察不得消费 Outcome、使其他调用失效或返回彼此矛盾的 Task Identity。
- 并发 `request_cancel()` 继续按 D-055 幂等共享同一请求状态。
- 一个副本的销毁或 reassociation 不得使其他有效副本的 Task Control Block 悬空。
- 同一对象上的 assignment/move/destruction 与成员调用未同步竞态不属于 Runtime 保证，不要求 Handle 内置对象级 mutex。
- 底层共享状态的线程安全不自动使用户存储的 `T` 的任意成员操作线程安全；Runtime 只保证完整构造 publication 和共享 `const T&` 的稳定身份。

### Scope and variants

| Concurrent operations | Guarantee |
|---|---|
| const/query/wait/cancel on stable same Handle | supported |
| operations through different Handle copies | supported |
| destroy/reassign one copy vs use another copy | supported |
| reassign/move/destroy same Handle object vs access it | caller synchronization required |
| user mutation/interior mutation of returned `T` | governed by `T`, not Runtime |

### Rationale

该边界与共享控制块和标准 C++ 对象并发模型一致：Runtime 负责跨副本共享状态，调用方负责同一小型 Handle 对象的结构性修改。它支持常见监督与多消费者场景，而不强迫 Handle 自身携带锁。

### Rejected alternatives

- 仅不同副本可并发：同一个共享 Handle 变量的 const 使用产生意外限制。
- 同一对象所有 assignment/move 也自动并发安全：增加对象级同步和尺寸，收益有限。
- 任何并发 `get()` 都不支持：与不可变可重复 Terminal Outcome 冲突。
- Runtime 保证返回 `T` 的所有 const 操作线程安全：无法替用户类型约束 interior mutability。

### Consequences

- Task Control Block、Outcome、waiter 与 cancellation state 必须是共享线程安全结构。
- API 文档需要区分“共享底层状态线程安全”和“Handle 对象 reassociation 线程安全”。
- 竞态测试覆盖同对象 const 调用、不同副本销毁与请求取消。

### Non-goals and deferred risks

- 本决策不使任意用户类型 `T` 成为线程安全类型。
- 本决策不规定引用计数算法或 lock-free 要求。
- 本决策不保证无锁、wait-free 或固定延迟。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐共享 TaskHandle 的稳定对象操作可并发，只有同一 Handle 的 reassociation/lifetime mutation 需要外部同步。
- Code or data evidence: D-042/D-044 固定共享 Handle 与不可变 Outcome，D-055 固定并发取消。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-073 — 多等待者共享单次 completion publication 且不得丢失完成

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

多个线程可以同时调用 `get()`、`wait()` 与 `wait_for()`。若 completion 使用单消费者通知或只唤醒一个 waiter，其他无界等待者可能在 Task 已终结后永久阻塞；若承诺确定唤醒顺序，又会把 OS 调度与内部 waiter 容器变成公共公平性契约。

### Decision

同一 Task 的所有同步等待者必须观察 D-071 的同一次 completion publication。Terminal Outcome 发布后，所有已经阻塞且没有合法 timeout 的 `get()`/`wait()` 调用都必须被可靠唤醒并具备继续完成的资格；新的等待者必须立即观察已发布完成而不得依赖历史通知。并发 `wait_for()` 分别按自己的 deadline 与同一 publication 线性化。Runtime 不承诺等待者唤醒顺序、公平次序、同时返回或从唤醒到实际运行的最大调度延迟。

### Invariants

- completion notification 不得采用只允许一个观察者消费完成的协议。
- Terminal Outcome publication 发生在 waiter notification 之前，继续遵循 D-071。
- waiter 在注册与 publication 竞态中不得丢失唤醒；注册前、注册中或注册后完成都必须可观察。
- 一个等待者返回、超时、抛 caller error 或被线程终止不得消费其他等待者的完成资格。
- `wait_for()` 的 `TimedOut` 只移除/结束该次等待，不影响其他 waiter 或 Task。
- 永久未终结 Task 仍可让无界等待者永久阻塞；本决策不伪造进度。

### Scope and variants

| Waiter | Completion behaviour |
|---|---|
| registered before publication | reliably notified/eligible to finish |
| races with publication | no lost wake-up |
| starts after publication | immediate observation |
| timed waiter deadline first | only that call returns `TimedOut` |
| multiple Worker Helping Waits | each observes same completion; scheduling order unspecified |

### Rationale

Completion 是不可变共享事实，不是一次性消息。可靠多等待者语义是 copyable TaskHandle 的必要结果；拒绝公平性和延迟承诺则保留高效条件变量、atomic wait、futex 或平台原语的实现空间。

### Rejected alternatives

- notify-one/单消费者完成：其他合法等待者可能永久阻塞。
- 每个 Handle 副本独立 completion：复制时形成状态分叉并增加竞态。
- FIFO waiter 公平性：绑定内部数据结构和 OS 调度，成本高且不能真正保证运行顺序。
- 完成时同步等待所有 waiter 返回：让 Task 完成路径受任意等待线程拖累。

### Consequences

- completion primitive 必须正确处理 register-before-check/check-before-register 竞态。
- 压力测试需覆盖完成前后大量混合 `get/wait/wait_for` waiter。
- Metrics 可以统计 waiter 和唤醒延迟，但数值不是功能正确性的固定 SLA。

### Non-goals and deferred risks

- 本决策不规定 condition variable、semaphore、atomic wait 或自定义 waiter list。
- 本决策不保证 waiter priority inheritance。
- Coroutine awaiter 的无阻塞注册协议另行设计。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐所有等待者共享单次 completion publication、无丢失唤醒，但不承诺顺序或返回延迟。
- Code or data evidence: D-042/D-044 固定共享不可变完成，D-071 固定 publication 顺序。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-074 — submit 编译期拒绝裸引用结果

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Callable 可以返回 `T&`、`const T&` 或 `T&&`，但 Task Runtime 无法从引用本身证明被引用对象在异步执行、等待和所有 Handle 生命周期内仍存活。若 `TaskHandle<T&>` 又通过共享 `const T&` 访问，外部对象生命周期与 Result ownership 会被误解为 Runtime 保证。

### Decision

按 D-059 选定 Callable invocation form 后，若 `std::invoke_result_t` 是任意 lvalue/rvalue reference 类型，`submit()` 必须在编译期拒绝且不形成 Task。需要显式返回外部引用语义的 Callable 必须把引用包装为值类型，例如 `std::reference_wrapper<T>`；该包装只显式表达非 owning 引用，引用目标生命周期仍由调用方负责。

### Invariants

- `T&`、`const T&`、`volatile T&` 与 `T&&` 结果统一拒绝，不因引用限定不同改变规则。
- 拒绝发生在 Task admission、Task Identity 分配和队列 publication 之前。
- 诊断应通过 concepts/`static_assert` 指向“reference result unsupported”，而不是在深层共享状态模板中失败。
- `std::reference_wrapper<T>`、指针、智能指针等对象返回类型继续作为普通 Value Outcome 存储。
- 包装引用不把目标生命周期转移给 Runtime，除非返回类型本身编码 owning ownership。

### Scope and variants

| Callable result | Submission |
|---|---|
| object value | subject to D-075 |
| `void` | supported |
| raw lvalue/rvalue reference | compile-time rejected |
| `std::reference_wrapper<T>` | supported as value; non-owning by type |
| `shared_ptr<T>` / owning wrapper | supported as value; ownership by type |

### Rationale

异步裸引用结果极易把被引用对象的生命周期错误归因给 TaskHandle。编译期拒绝使 ownership 明确；有真实引用需求时，`reference_wrapper` 或指针让风险在类型上可见，而不是隐藏在 `TaskHandle<T&>` 特化中。

### Rejected alternatives

- 支持 `TaskHandle<T&>` 并完全信任调用方：API 外形暗示 Runtime 管理结果，悬垂风险高。
- 自动复制引用目标：改变 Callable 返回语义，且可能无法复制或代价巨大。
- 自动转成 `reference_wrapper`：隐藏 non-owning 选择，诊断性差。
- 运行时检测悬垂：标准 C++ 无法可靠判断任意引用生命周期。

### Consequences

- 泛型 submit 约束需要在 invocation result deduction 后进行。
- 明确引用返回需求需要少量包装代码。
- TaskHandle 不需要引用类型偏特化，减少共享 Outcome 复杂度。

### Non-goals and deferred risks

- 本决策不验证指针或 `reference_wrapper` 目标生命周期。
- 本决策不限制 Callable 捕获引用；捕获生命周期仍由调用方负责。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐编译期拒绝裸引用 Task 结果，并要求用 reference_wrapper 或 owning wrapper 显式表达引用/所有权。
- Code or data evidence: D-044/D-046 建立 Runtime-owned shared Outcome；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-075 — submit 结果类型保留 void 或归一化为可移动对象值

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

拒绝引用结果后，还需固定顶层 cv、move-only result 与完全 immovable result 的边界。若首版无约束地承诺任意对象结果，type erasure 和 shared Outcome 可能无法把 invocation expression 安全构造成稳定存储；若要求 CopyConstructible，又会背离已确认的 move-only 结果目标。

### Decision

`submit()` 的原始结果 `R` 取自 D-059 选定 invocation form 的 `std::invoke_result_t`。`R` 为 `void` 时返回 `TaskHandle<void>`；`R` 为非引用对象时，公共结果类型为 `T = std::remove_cv_t<R>`，并要求 `T` 满足可析构且可从 invocation result 移动构造的约束，返回 `TaskHandle<T>`。不要求 `T` CopyConstructible；move-only object result 必须受支持。完全不可移动/不可复制的 object result 首版编译期拒绝。

### Invariants

- 结果约束在 admission 前编译期验证，不得以运行时 rejection 表示。
- 顶层 `const`/`volatile` 不进入 `TaskHandle<T>` 的 `T`，底层成员 cv 保持类型自身定义。
- move-only `T` 的 Value Outcome 仍按 D-076 通过共享 `const T&` 重复观察，不被 `get()` move-out。
- result construction 抛出的异常按 D-045 成为 Exception Outcome，不逃出 Worker。
- 完全 immovable result 的拒绝诊断必须与裸引用拒绝区分。

### Scope and variants

| Raw result `R` | Public handle |
|---|---|
| `void` | `TaskHandle<void>` |
| copyable object `T` | `TaskHandle<remove_cv_t<T>>` |
| move-only object `T` | supported, shared const observation |
| immovable object | compile-time rejected in first stable API |
| reference | rejected by D-074 |

### Rationale

支持 move-only 值覆盖现代 C++ 所有权类型，而显式拒绝完全 immovable 结果让首版 type erasure 和 Outcome construction 具有可实现、可诊断的边界。移除顶层 cv 避免生成低价值的 `TaskHandle<const T>` 变体。

### Rejected alternatives

- 要求 CopyConstructible：排除 `unique_ptr` 等核心现代 C++ 结果。
- 承诺任意 immovable prvalue：对 type erasure 和间接构造提出更强实现约束，当前没有必要用例。
- 保留顶层 cv：扩大模板实例集合而不增加结果保护能力，`get()` 本就返回 const reference。
- 对不合规结果运行时失败：类型问题应在编译期报告。

### Consequences

- submit concepts 和诊断必须覆盖普通及 stop-token invocation 两种 form。
- 测试类型矩阵至少包括 void、copyable、move-only、immovable、cv object 与 reference。
- 未来若要支持 immovable result，需新增决策并验证直接原位构造路径，不应静默放宽 ABI/requirements。

### Non-goals and deferred risks

- 本决策不限制结果对象大小或是否单独分配。
- 本决策不规定 allocator-aware result construction。
- 本决策不支持数组和函数值；它们不是可存储对象结果。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐结果归一化为 void 或去顶层 cv 的可移动对象，支持 move-only 而首版拒绝完全 immovable 结果。
- Code or data evidence: D-059 固定 invocation selection，D-074 拒绝引用，D-044 固定 shared Outcome。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-076 — get 仅允许左值 Handle 并以 Handle 本身持有结果生命周期

Status: accepted

Date: 2026-08-26

Supersedes: D-046

Superseded by: None

### Context

D-046 选择共享 `const T&`，但允许在临时 Handle 上调用未加 ref qualifier 的 `get() const`。`scheduler.submit(...).get()` 返回的引用会在完整表达式结束、临时 Handle 析构且没有其他副本时立即悬垂；仅靠文档提示无法防止这个常见错误。由于 Handle 本身已 copyable 且持有 Outcome，单独引入 owning Result View 又会重复所有权抽象。

### Decision

非 void 与 void 的 `TaskHandle::get()` 都必须只允许在左值 Handle 上调用；公共签名使用 `get() const &`，并使 rvalue overload deleted/不可调用。非 void Value Outcome 返回共享结果对象的 `const T&`，void Value Outcome 返回 `void`；Exception/Cancelled 继续按 D-045/D-057 传播。基础稳定 Interface 不提供消费式 `take()` 或单独 owning Result View；调用方通过保留/复制有效 TaskHandle 持有 Outcome 生命周期。

### Invariants

- `std::move(handle).get()` 与 `scheduler.submit(...).get()` 必须在编译期拒绝，不得返回潜在悬垂引用。
- lvalue `get()` 不复制、不移动、不消费 Value Outcome；所有调用返回同一存储对象的 `const T&`。
- 返回引用只在至少一个关联该 Outcome 的有效 Handle 继续存活期间有效。
- `TaskHandle<void>::get() const &` 同样采用 lvalue-only 规则，使泛型代码具有统一调用资格。
- 不得引入 `ResultTaken` 状态或 Handle 间消费仲裁。
- 需要转移独占资源时，Callable 应返回 `shared_ptr` 或其他自身可共享的 value 类型。

### Scope and variants

| Call form | Behaviour |
|---|---|
| `handle.get()` on lvalue | observe/rethrow shared Outcome |
| `std::as_const(handle).get()` | observe/rethrow shared Outcome |
| `std::move(handle).get()` | compile-time rejected |
| temporary Handle `.get()` | compile-time rejected |
| need longer lifetime | retain/copy Handle |

### Rationale

ref-qualified API 在编译期消除最直接的悬垂路径。TaskHandle 已经是共享 owning capability，因此复制 Handle 比引入第二个 Result View 类型更浅、更一致；继续拒绝 `take()` 则保留不可变、可重复 Outcome。

### Rejected alternatives

- 保留 `get() const` 并仅文档警告：临时 Handle 是自然表达式，错误容易发生。
- rvalue `get()` 按值返回：根据 value category 改变语义，且无法统一支持 move-only 与重复观察。
- 新增 owning `ResultView<T>`：与 copyable TaskHandle 重复持有同一共享状态，扩大 API。
- `take()`：重新引入消费权竞态和可变终态。
- 仅非 void 限制左值、void 允许临时：泛型调用资格不一致。

### Consequences

- 示例必须先保存 submit 返回的 Handle，再调用 `get()`；只等待临时任务可显式保存 Handle 或使用更高层 structured concurrency。
- D-046 被本决策 supersede；共享 const reference 与无 take 的核心选择保留，但签名和生命周期所有者更精确。
- 编译测试必须覆盖临时/rvalue rejection 与 move-only result。

### Non-goals and deferred risks

- 本决策不阻止调用方在最后一个 Handle 销毁后继续保存已取得的引用；这仍是普通 C++ 生命周期错误。
- 本决策不提供按值复制便捷方法。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐以 lvalue-ref-qualified get 在编译期阻止临时 Handle 悬垂，并用 TaskHandle 本身承担结果 ownership。
- Code or data evidence: D-042 固定 Handle 可复制，D-044 固定共享 Outcome；D-046 已识别临时 Handle 悬垂风险但未消除。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-077 — 稳定 TaskHandle 不提供第二套非抛出式 Outcome View

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

在 `state()`、`wait()`、`wait_for()` 与 `get()` 已固定后，还可以增加 `try_get()`、`exception()`、`outcome()` 或 variant-like `OutcomeView<T>`。但 move-only value 需要引用式 variant alternative，异常需要决定 `exception_ptr` 暴露，空/未完成又会引入额外状态；这会形成与现有 API 平行的第二套结果协议。

### Decision

计划内稳定 `TaskHandle` Interface 不提供 `try_get()`、`exception()`、`outcome()` 或独立 `OutcomeView/ResultView`。调用方用非阻塞 `state()` 观察 Waiting/Ready/Running/Suspended/Succeeded/Failed/Cancelled，用 `wait()`/`wait_for()` 同步完成，并仅通过左值 `get()` 获取 Value 或传播 Exception/Cancelled。需要保留结果生命周期时复制 TaskHandle。

### Invariants

- `state()` 返回终态后，D-071 保证 `get()` 不再等待；但调用方仍须按 `get()` 契约处理异常。
- Runtime 不得通过 Metrics/Trace 内部视图反向扩展未批准的公共 Outcome API。
- 不提供 `try_get()` 不允许 `get()` 改成返回 optional 或错误码；其最强传播语义保持不变。
- TaskHandle 复制继续是唯一公共结果 ownership view，不另建共享结果所有者类型。
- 未来新增 Outcome View 必须有独立用例和决策，不得作为实现泄漏自动公开。

### Scope and variants

| Need | Stable API |
|---|---|
| nonblocking lifecycle/category | `state()` |
| completion-only synchronization | `wait()` / `wait_for()` |
| Value / Exception / Cancelled propagation | lvalue `get()` |
| retain result lifetime | copy TaskHandle |
| inspect stored `exception_ptr` without throw | not public |

### Rationale

现有四个操作已形成正交的小接口：状态、同步、结果、控制。第二套 variant/pointer 结果视图会增加 lifetime、空状态和模板复杂度，却没有当前确认的消费者；保持单一 `get()` 也使异常传播测试与 Coroutine await 复用更直接。

### Rejected alternatives

- `optional<reference_wrapper<const T>> try_get()`：无法表达 Failed/Cancelled，仍需其他通道。
- `variant<reference_wrapper<const T>, exception_ptr, cancelled>`：暴露第二套 Outcome 类型和异常处理风格。
- `exception()` 单独方法：鼓励先查 state 再取异常，并扩大共享状态 ABI。
- owning Result View：与 copyable TaskHandle 重复所有权。

### Consequences

- 不愿使用异常的调用方可以通过 `state()` 识别 `Failed/Cancelled`，但无法公开读取原始 exception object 而不触发 rethrow。
- Coroutine `await_resume()` 应复用 `get()` 的 Outcome 传播，而不是建立另一套结果类型。
- 若未来生态明确需要 expected-style interop，可在独立版本决策中增加适配器，而不是改变基础 Handle。

### Non-goals and deferred risks

- 本决策不禁止应用在 `get()` 外层把异常转换为 `std::expected`。
- 本决策不限制内部 Metrics/Trace 读取终态类别。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐以 state/wait/get/Handle copy 覆盖观察与 ownership，不公开平行的非抛出式 Outcome View。
- Code or data evidence: D-069 至 D-071 固定 state，D-076 固定 get 与 Handle ownership。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-078 — SchedulerOptions 用默认 64 的正数限制 Helping Wait 嵌套深度

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

普通 C++ Callable 在 Helping Wait 中保留外层栈帧；被帮助执行的 Task 若再次等待，会形成真实 C++ 调用栈嵌套。无界 fork-join 链可能在任务逻辑仍合法时耗尽 Worker stack，而完全禁止嵌套又会让单 Worker Runtime 的常见多层组合失败。

### Decision

`SchedulerOptions` 必须提供 `std::size_t max_helping_depth = 64`。每个 Worker 单独追踪当前 Helping Wait 嵌套深度；从普通 Task execution context 首次进入 Helping Wait 计为深度 1，每次被帮助执行的 Task 再进入 Helping Wait 递增，离开对应 wait scope 后递减。配置值必须大于 0，值为 0 时 Scheduler 启动事务在创建 Worker 前以配置错误失败。

### Invariants

- depth 属于 Worker execution context，不属于 TaskHandle、目标 Task 或 Runtime 全局计数。
- 同/跨 Runtime Helping Wait 在源 Worker 上使用同一个 depth 计数。
- 非 Worker 的 blocking wait 不计入 Helping depth。
- 已 Terminal 的即时观察以及非正 duration `wait_for()` 不进入 Helping Wait、不增加 depth。
- Direct Self-Wait 在任何 depth 变化前按 D-049/D-065 拒绝。
- 默认 64 是递归防护阈值，不是字节级 stack safety 或最大依赖图深度保证；用户增大它需承担更高 stack 风险。

### Scope and variants

| Situation | Depth effect |
|---|---|
| outer Task first helps | 0 → 1 |
| helped Task nests wait | N → N+1 if allowed |
| nested wait leaves | N → N-1 |
| cross-Runtime target | counted on source Worker |
| non-Worker wait / immediate probe | none |

### Rationale

64 为常见 fork-join 层级保留充足空间，同时给意外递归链一个确定的进程内故障边界。把阈值放在 SchedulerOptions 中允许小栈线程和特殊 workload 下调，也避免把任意单一数字伪装成所有平台的安全栈深度。

### Rejected alternatives

- 完全无上限：深链最终可能以不可恢复 stack overflow 终止进程。
- 固定不可配置上限：无法适应 Worker stack 与 workload 差异。
- `0` 表示无限制：把最危险模式编码成看似普通数值，且削弱配置验证。
- `0` 表示禁用 Helping：单 Worker fork-join 会退化为死锁/拒绝，语义变化过大。
- 按 DAG 深度计数：动态 TaskHandle wait 不一定属于显式 DAG。

### Consequences

- WorkerContext 需要维护异常安全的 depth guard。
- Metrics/Trace 应记录当前/最大 observed helping depth 与 limit exceeded 事件。
- 超限行为由 D-079 固定；深依赖优先使用 DAG 或 Coroutine，减少同步栈嵌套。

### Non-goals and deferred risks

- 本决策不估算每个 Callable 的栈字节数，也不能阻止单个 Task 自身 stack overflow。
- 本决策不检测 Indirect Wait Cycle。
- 本决策不规定操作系统 Worker stack size。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 SchedulerOptions 以默认 64 的正数阈值限制每 Worker Helping Wait 栈嵌套。
- Code or data evidence: D-048/D-065 要求普通 Callable 通过嵌套 Helping Wait 保留外层栈。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-079 — Helping depth 超限在启动下一层帮助前抛出专用异常

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-078 设定阈值后，超限可以阻塞 Worker、返回 timeout、取消目标或抛出错误。阻塞可能让单 Worker链永久停滞；伪造 `TimedOut/Cancelled` 会改变目标 Task；进程 terminate 又不给当前 Callable 恢复或降级机会。

### Decision

当 Worker 对未完成的非自身 Task 调用 `get()`、`wait()` 或正 duration `wait_for()`，且进入下一层 Helping Wait 会使 depth 超过 `max_helping_depth` 时，必须在启动任何新的 helped Task、阻塞或改变目标状态前同步抛出公开 `astra::helping_depth_exceeded : std::runtime_error`。异常可由当前 Callable 捕获；若逃出 Callable，按 D-045 成为当前 Task 的 Exception/Failed Outcome，不影响目标 Task 继续执行。

### Invariants

- 只有“需要进入新一层 Helping”才检查超限；目标已经 Terminal 时仍正常观察 Outcome。
- 检查顺序为：Handle validity、Direct Self-Wait、即时 Terminal/非正 probe、随后 Helping depth admission。
- 超限不得返回 `TimedOut`、发布 Cancelled、请求 stop 或改变目标优先级。
- 抛出时 depth 计数保持调用前值，不得因异常泄漏 increment。
- 专用异常类型是稳定源代码 API；其 `what()` 文本不作为 ABI/测试契约。
- 用户捕获后可以选择改用异步组合、返回错误或继续执行其他逻辑。

### Scope and variants

| Operation | At limit + incomplete target |
|---|---|
| Worker `get()` | throw `helping_depth_exceeded` |
| Worker `wait()` | throw `helping_depth_exceeded` |
| Worker positive `wait_for()` | throw `helping_depth_exceeded` |
| non-positive `wait_for()` | immediate probe, no depth entry |
| non-Worker wait | not applicable |

### Rationale

专用可捕获异常在不伪造目标 Task 事实的前提下阻止更深 C++ 栈递归，并能自然沿当前 Task 的既有异常边界传播。相比 generic logic_error，它清楚表示运行时资源/配置边界，而非 Direct Self-Wait 这类必然逻辑错误。

### Rejected alternatives

- 达到上限后普通阻塞：可能耗尽全部 Worker 并死锁。
- 返回 `TimedOut`：无 timeout 的 get/wait 没有该结果，且混淆资源上限。
- 自动取消目标或当前 Task：越权改变业务策略。
- `std::terminate()`：把可诊断、可恢复的嵌套过深升级为进程故障。
- 继续帮助并只打日志：无法兑现深度上限。

### Consequences

- `get()`/`wait()` 除 caller `logic_error` 外还可能抛出 Runtime 的 depth exception；业务 Outcome 异常仍保持原类型。
- 单元测试需用很小的配置阈值确定性触发，并验证目标 Task 未被取消。
- API 文档应推荐显式 DAG/Coroutine 处理深依赖，而不是简单调高阈值。

### Non-goals and deferred risks

- 本决策不把 depth exception 转换为 Cancelled。
- 本决策不保证在阈值以内绝不 stack overflow。
- 本决策不替代 Indirect Wait Cycle 诊断。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐超出 Helping depth 时在副作用前抛专用 runtime_error，目标 Task 保持不变。
- Code or data evidence: D-045 固定 Worker exception boundary，D-078 固定 depth 计数与阈值。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-080 — Helping Wait 始终服从源 Runtime 的 Shutdown eligibility

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Worker 在 Task 内等待期间仍通过源 Runtime 调度工作。若 Shutdown 后 Helping loop 绕过正常 admission/eligibility 继续启动任务，Immediate 会错误执行本应取消的工作，Graceful 也可能执行不属于 Drain Work Closure 的提交。Cross-Runtime 等待还可能把目标 Runtime 的模式错误套到源 Worker。

### Decision

Helping Wait 每次选择 helped Task 时必须使用源 Runtime 当前的正常 eligibility 与调度路径。源 Runtime 为 Running 时按正常策略帮助；处于 Graceful Stopping 时只允许执行 Drain Work Closure 内仍 Eligible 的 Task；一旦源 Runtime 进入 Immediate Stopping，不得再启动任何尚未 Running 的 Task，Helping loop 只能阻塞/观察目标 completion 与源模式变化，直至当前外层 Running Callable 能继续。目标 Task 所属 Runtime 的 Shutdown Mode 只通过目标自身的执行、取消和 Terminal Outcome 影响等待，不授予源 Worker执行目标 Runtime 工作的权限。

### Invariants

- Helping Wait 不得绕过 External/Internal Submission closure、Immediate pre-start cancellation 或 DAG eligibility。
- 源 Graceful 可以继续帮助合法 Internal Submission 形成的 Drain Work Closure。
- 源 Immediate 后，队列中的未运行 Task 按 D-006 取消；Helping Worker 不得先 claim 执行再规避取消。
- Cross-Runtime 等待始终只帮助源 Runtime；目标 Graceful/Immediate 不改变该归属。
- 源 Immediate + 外部目标未完成时，外层 Running Task/Runtime 可以永久 Pending；不得 detach、强杀或伪造目标完成。
- Shutdown 模式检查必须发生在每次新 helped Task selection 前；已经 Running 的 helped Callable 仍按 cooperative 边界完成。

### Scope and variants

| Source mode | Helping action |
|---|---|
| Running | normal eligible scheduling order |
| Graceful Stopping | only Drain Work Closure eligible tasks |
| Immediate Stopping | start none; observe/block for target |
| target Runtime changes mode | source ownership unchanged; observe target Outcome |

### Rationale

Helping Wait 是正常 Worker 调度路径的嵌套使用，而不是绕过生命周期规则的特殊 executor。让源 Runtime 独立决定可执行集合，能同时保持 Shutdown 单调性、Runtime 隔离和 cross-Runtime ownership。

### Rejected alternatives

- Helping loop 忽略 Shutdown 直到目标完成：Immediate 后仍可能启动已取消任务。
- 源 Immediate 时强制取消外部目标：跨 Runtime 越权。
- Worker 转去执行目标 Runtime 队列：破坏 D-051 的隔离、Metrics 和 Local Queue ownership。
- 源 Immediate 时终止外层 Callable：违反 cooperative cancellation 与 RAII。

### Consequences

- wait loop 需要同时被 target completion、source work availability 和 source shutdown mode change 唤醒。
- Immediate Shutdown 仍可能被正在 cross-Runtime wait 的不合作 Running Task 无限拖延。
- 测试必须覆盖 wait 与 Graceful→Immediate 升级、跨 Runtime 四种模式组合。

### Non-goals and deferred risks

- 本决策不改变 Shutdown API 从 Worker 调用时的拒绝规则。
- 本决策不保证 cross-Runtime wait cycle 检测。
- 本决策不固定 wake primitive 或调度 batch size。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 Helping Wait 始终复用源 Runtime 的正常 Shutdown eligibility，Immediate 后不再启动新任务。
- Code or data evidence: D-001/D-002 固定版本调度基线，D-005 至 D-007 固定 Graceful/Immediate 工作边界，D-051 固定 cross-Runtime ownership。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0020](../../docs/adr/0020-workers-help-while-waiting-for-same-runtime-tasks.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-081 — Unobserved Task Exception 不触发 terminate、日志或终态改写

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-043 允许 fire-and-forget，D-045 把 Callable 异常捕获进 Terminal Outcome。若最后没有调用方执行 `get()`，Runtime 可以选择 terminate、在析构中打印、调用全局 handler，或只作为诊断事实记录。强制控制行为会让 Handle 观察方式改变任务/进程语义，析构日志又可能阻塞、重入或在静态销毁期间访问失效设施。

### Decision

Failed Task 的 Exception Outcome 即使从未被 `get()` 观察，也不得触发 `std::terminate()`、重新抛出到 Worker loop、同步写 stderr/stdout、调用用户全局 unobserved-exception callback、自动请求取消其他 Task，或改写 Terminal Outcome。Task execution boundary 捕获并发布 Failed 后，任务执行责任已正常终结；是否观察异常只影响 D-082 的诊断数据。

### Invariants

- Unobserved Exception 不得逃出 Task Control Block/Runtime State 的析构函数。
- 最后 Handle 销毁前后是否观察异常不得改变 Scheduler Shutdown Completion 或 Reaper Finalization。
- Unobserved Exception 不得把 Failed 改成 Cancelled，也不得级联取消 DAG/Coroutine，除非后续明确的结构化传播规则独立要求。
- Runtime 内部不变量异常继续属于 D-040 的 fail-fast 控制面边界，不得误归类为用户 Task Exception。
- 关闭 Metrics/Trace 时仍必须安全释放 Failed Outcome，不能因无诊断 sink 改变行为。

### Scope and variants

| Failure source | Behaviour |
|---|---|
| user Callable ordinary exception | stored Failed Outcome; diagnostics only if unobserved |
| `task_cancelled` signal | Cancelled, not unobserved exception |
| Runtime invariant/control-plane exception | fail-fast policy, not this decision |
| no remaining Handle | Task still completes; no terminate/log side effect |

### Rationale

Future-like Task 的异常属于结果域，不应因观察者生命周期偶然改变进程控制流。把 unobserved failure 限于诊断能保留 fire-and-forget 与 RAII 安全，也避免从析构路径调用未知用户代码。

### Rejected alternatives

- 最后 Handle 析构时 terminate：Handle 可能早于 Task 完成消失，且破坏 fire-and-forget。
- 默认打印 stderr：库擅自选择输出通道，可能阻塞或污染宿主日志。
- 全局用户 callback：引入重入、生命周期、异常与线程上下文协议。
- 自动取消 Scheduler/子任务：把结果观察策略变成业务取消策略。
- 静默且不留任何诊断：虽语义安全，但削弱运行时工程可观测性，D-082 提供低风险替代。

### Consequences

- 应用若必须处理每个异常，需要持有 Handle、调用 get，或在更高层 structured concurrency 中建立监督。
- Runtime 不提供“未观察异常即崩溃”的测试模式作为稳定行为；测试可通过 Metrics/Trace 断言。
- 诊断追踪与生命周期顺序由 D-082 固定。

### Non-goals and deferred risks

- 本决策不定义 DAG parent/child failure propagation。
- 本决策不提供进程级 exception handler。
- 本决策不阻止应用自行封装 fail-fast policy。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐未观察 Task Exception 永远只属于结果/诊断域，不 terminate、打印、回调或改变任务策略。
- Code or data evidence: D-043 固定无 Handle Task 继续，D-045 固定异常捕获，D-040 区分控制面 fatal failure。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0023](../../docs/adr/0023-unobserved-task-exceptions-are-diagnostic-only.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-082 — Exception Outcome 以首次 get 标记 observed 并在最终释放时诊断

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

仅说“记录未观察异常”仍需定义何为观察、何时判断不会再观察，以及如何避免多 Handle/并发 get 重复计数。`state()==Failed` 只能看见类别，不能取得或传播原始异常；最后一个 Handle 销毁也不一定是 Task shared state 的最终释放，因为 Runtime 可能仍持有执行责任。

### Decision

Exception Outcome 具有内部原子 `exception_observed` 标志，初始为 false。任意有效左值 Handle 的 `get()` 观察到 Exception Outcome、在重抛存储异常之前，必须把该标志幂等设为 true；`state()`、`wait()`、`wait_for()` 与 Metrics 读取不标记异常已观察。Task completion shared state 在所有 Handle 和 Runtime execution ownership 都释放、即将最终销毁时，若 Outcome 为 Exception 且标志仍为 false，必须以 noexcept、非阻塞、无用户回调的方式增加 `tasks_failed_unobserved_total`，并在 Trace 启用且 sink 仍可用时发出一个 unobserved-failure diagnostic event。

### Invariants

- 多个并发 `get()` 最多使 observed 标志发生一次 false→true 逻辑转换，但每个调用仍按 D-045 重抛。
- `get()` 即使重抛后未被调用方 catch，也已算观察，因为异常已穿过公共结果边界。
- 仅调用 `state()==Failed` 不算观察原始 Exception Outcome。
- 判断 unobserved 的时点是 completion shared state 最终释放，不是最后 Handle 析构；Runtime 必须在执行责任结束前保持状态存活。
- 诊断路径不得分配为正确性前提、阻塞、抛异常或调用用户代码；sink 不可用时允许丢弃 Trace event，但不得破坏释放。
- Runtime-owned Metrics 核心的生命周期必须覆盖其 Task completion state 最终释放，确保计数可安全提交；最终对外 snapshot 的保留策略在 Metrics 设计中固定。

### Scope and variants

| Observation/release | Effect |
|---|---|
| Exception `get()` | mark observed, then rethrow |
| Value/Cancelled `get()` | no exception-observed flag |
| `state/wait/wait_for` | no mark |
| final release + Failed + false | increment metric; best-effort trace |
| final release + Failed + true | no unobserved diagnostic |
| diagnostics disabled/unavailable | safe release, no control effect |

### Rationale

`get()` 是唯一批准的 Exception propagation Interface，因此以它定义 observed 最清楚。推迟到 shared state 最终释放才能覆盖 Handle 先消失、Task 后失败的场景；noexcept best-effort 诊断则保持 D-081 的非控制边界。

### Rejected alternatives

- `state()==Failed` 即算 observed：调用方未取得原异常，却会压掉最有价值的诊断。
- 最后 Handle 析构时判断：Task 可能尚未完成或 Runtime 仍持有状态。
- 每次失败立即计作 unobserved：之后正常 get 也无法消除误报。
- 从析构调用用户 callback：存在重入、阻塞、抛异常与生命周期风险。
- 保存完整异常文本到 Metrics：可能分配、泄露敏感信息且 `exception_ptr` 不可通用提取消息。

### Consequences

- Task completion state 增加一个轻量 observed bit 和最终释放诊断 hook。
- Metrics 必须区分 `tasks_failed_total` 与 `tasks_failed_unobserved_total`。
- Trace event 只能携带 TaskId、RuntimeId、类型类别等安全元数据；异常文本/类型披露策略需在 Trace 设计中另定。

### Non-goals and deferred risks

- 本决策不保证进程异常终止时能刷新最后的 diagnostic event。
- 本决策不把观察状态公开给应用。
- 本决策不规定 Metrics export/aggregation 生命周期。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐仅 get 标记 Exception Outcome 已观察，并在共享完成状态最终释放时以 noexcept Metrics/Trace 诊断未观察失败。
- Code or data evidence: D-043 固定 Runtime 可在无 Handle 时继续持有执行责任；D-045/D-077 固定 get 为唯一异常传播入口。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0023](../../docs/adr/0023-unobserved-task-exceptions-are-diagnostic-only.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-083 — Backpressure 配额只约束尚未首次运行的 External Submission

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

早期草案把 `global_queue_capacity` 描述为 Global Injection Queue 的硬容量，但 D-002 要求 Graceful Stopping 仍允许同 Runtime Running Task 产生 Internal Submission。若 Internal Submission 也受硬队列容量限制，满队列时 Worker 只能阻塞、拒绝或 inline 执行：阻塞可能耗尽 Worker，拒绝破坏 Drain Work Closure，inline 又绕过调度、优先级和 TaskHandle start 竞态。

### Decision

公共配置改为 `std::size_t external_pending_capacity = 65536`，表示每个 Runtime 同时处于“已接受但尚未首次进入 Running”的 External Submission 最大数量，覆盖其 `Waiting` 与 `Ready` 状态，而不是底层 Global Injection Queue 的物理硬容量。External Submission 在 admission 时原子占用一个 slot，在首次 `Ready → Running` start 线性化或 start 前 Terminal（例如取消）时释放；进入 Running 后即使 Coroutine 之后 Suspended 也不重新占用。由同 Runtime 当前 Running Task 发起的 Internal Submission 不占用该配额，并继续服从生命周期 admission 与 outstanding-work 核算。

### Invariants

- 配额值必须大于 0；0 在 Scheduler 启动事务创建 Worker 前作为配置错误拒绝。
- External Submission 的 Waiting/Ready 转换、队列迁移或 priority 更新不得重复占用/释放 slot。
- start 与 pre-start cancellation 竞争只有赢家负责恰好一次释放 slot。
- Internal Submission 虽不占 external slot，仍必须分配 Task Control Block、计入 Drain Work Closure、发布正常 Outcome。
- 跨 Runtime Worker 向目标 Runtime 提交属于目标 Runtime 的 External Submission，受目标配额约束。
- 该配置不承诺 Runtime 总内存或总 Task 数有硬上限；Internal Submission 可以使 outstanding work 超过 external capacity。

### Scope and variants

| Submission/task state | External slot |
|---|---|
| external admitted Waiting/Ready | held |
| external first enters Running | released once |
| external cancelled before start | released once |
| external Coroutine Suspended after start | not reacquired |
| same-Runtime Internal Submission | never uses external slot |

### Rationale

把背压放在外部准入边界，可保护 Runtime 免受应用入口洪泛，同时不让已经接受的任务因内部派生在 Graceful drain 中自锁。跨 DAG Waiting 与 Ready 计数也避免通过依赖阻塞绕过仅队列长度的限制。

### Rejected alternatives

- Global Queue 物理硬容量覆盖全部任务：Internal Submission 满队列时无法同时保证进度、调度语义和 D-002。
- 只计 Ready Queue、不计 Waiting：大量 DAG 节点可以绕过背压占用内存。
- Internal Submission 也拒绝：可能截断已接受工作的必要派生闭包。
- Internal Submission 满时 CallerRuns：绕过队列、优先级/deadline 和 start/cancel 线性化。
- 让 0 表示 unbounded：把关闭背压隐藏在普通数值中，配置意图不清晰。

### Consequences

- `global_queue_capacity` 从公共草案中移除；内部容器容量/增长策略保持实现私有。
- Runtime 必须分别维护 external pending slot 和全部 outstanding work。
- 失控 Internal Submission 仍可能耗尽内存，需要 Metrics、TaskGraph/structured concurrency 与应用策略治理。

### Non-goals and deferred risks

- 本决策不提供全 Runtime hard memory budget。
- 本决策不限制 Running/Suspended Task 数。
- 本决策不固定 Global Queue 或 Local Deque 的分配策略。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐背压只限制未首次 Running 的 External Submission，Internal Submission 为保证 drain/liveness 不占该配额。
- Code or data evidence: D-002 固定 Graceful Internal Submission，D-003 固定 External admission，D-006/D-052 固定 start/cancel 线性化。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0024](../../docs/adr/0024-backpressure-limits-external-pending-work.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-084 — External Backpressure 仅提供 Reject 与 Block 且默认 Reject

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

External pending slot 耗尽时，Runtime 可以 Reject、Block 或 CallerRuns。CallerRuns 会让 Callable 在 submission 返回 Handle 前执行，改变线程归属、异常边界、TaskState、Priority/Deadline、Trace 和取消竞态；默认 Block 则可能给不了解容量语义的应用带来无界延迟。

### Decision

公共配置提供：

```cpp
enum class ExternalBackpressure {
    Reject,
    Block
};
```

`SchedulerOptions::external_backpressure` 默认 `ExternalBackpressure::Reject`。当 External pending slot 不可用时，Reject 立即产生 admission rejection；Block 对 D-085 允许的调用方等待 slot 或 admission gate 关闭。稳定 Runtime 不提供 `CallerRuns` overflow policy，任何用户 Callable 都必须先成功形成 Task、进入正常调度路径后才执行。

### Invariants

- Backpressure policy 只在 lifecycle admission 仍开放但 external slot 暂时不可用时生效。
- Scheduler 已 Stopping/Stopped 时不得因 Block 等待未来 slot；必须按 admission closed 拒绝。
- Reject 不得创建 Task Identity、Outcome、outstanding-work 或队列元素。
- Block 成功后继续执行同一 admission 事务；在返回成功前必须完成 slot reservation 和 D-003 核算。
- CallerRuns 不得作为隐藏优化、测试模式或 Worker 特例启用。

### Scope and variants

| Policy | Capacity available | Capacity exhausted |
|---|---|---|
| Reject | admit normally | reject immediately |
| Block + eligible caller | admit normally | wait for slot/gate close |
| Block + ineligible Worker | admit normally | D-085 nonblocking rejection |

### Rationale

默认 Reject 提供可预测延迟和显式失败；需要生产者节流的普通线程可以选择 Block。删除 CallerRuns 保持“所有 Task 经 Scheduler 执行”的身份、状态和可观测性不变量，避免 submission 调用栈成为隐式 executor。

### Rejected alternatives

- 默认 Block：配置疏忽会让提交路径无界阻塞。
- CallerRuns：破坏线程归属、调度顺序、异常捕获和 TaskHandle 返回前状态。
- Drop oldest/newest：静默丢弃已接受或正提交工作，破坏 Outcome 保证。
- Grow 作为 policy：等价于绕过 external capacity，无法提供背压。

### Consequences

- primary `submit` 与 nonthrowing admission Interface 需要分别表达 rejection，后续决策固定。
- Block 需要无丢失唤醒与 shutdown 中断，见 D-086。
- 需要 inline fallback 的应用必须在收到明确 rejection 后自行执行，其执行不再伪装为 AstraScheduler Task。

### Non-goals and deferred risks

- 本决策不提供 timeout/cancellable Block overload。
- 本决策不固定 rejection 错误类型。
- 本决策不保证 Reject caller 的重试公平性。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐只保留 Reject/Block，默认 Reject，并明确移除会绕过 Scheduler 的 CallerRuns。
- Code or data evidence: D-041 固定 submit 返回 TaskHandle，D-045 固定 Callable Worker exception boundary，D-083 固定 external slot。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0024](../../docs/adr/0024-backpressure-limits-external-pending-work.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-085 — Block Backpressure 只允许普通非 Worker 调用方等待

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

任意 AstraScheduler Worker 向另一 Runtime 提交时，在目标视角属于 External Submission。若目标配置 Block 且容量满，该 Worker 会停止推进自己的源 Runtime；多个 Runtime 交叉提交或所有 Worker 同时阻塞可形成资源等待环。同 Runtime Internal Submission 已由 D-083 豁免配额，无需 Block。

### Decision

`ExternalBackpressure::Block` 只有普通非 AstraScheduler Worker 线程可以实际等待。任意 AstraScheduler Worker 向其他 Runtime 发起 External Submission 时，如果目标 external slot 可立即取得则正常 admission；若不可立即取得，则无论目标配置 Reject 还是 Block，都必须立即以 capacity rejection 结束，不得阻塞、Helping 目标 Runtime 或执行 CallerRuns。同 Runtime Worker 的 Internal Submission 不使用 external slot，按 D-002/D-083 正常 admission。

### Invariants

- Worker 身份以当前 WorkerContext 判断，不因持有哪个 Scheduler Handle 改变。
- 跨 Runtime Worker 不得进入目标 Runtime 的 capacity condition wait。
- Worker capacity rejection 不得让源 Runtime 进入 Helping Wait；submission 不是 TaskHandle completion wait。
- 有空 slot 时 Worker External Submission 与普通 External Submission 使用相同 admission/start 核算。
- lifecycle gate 已关闭时优先返回 lifecycle rejection，不把它伪装成 capacity full。

### Scope and variants

| Caller → target | Slot | Behaviour |
|---|---|---|
| ordinary thread | available | admit |
| ordinary thread + Block | full | wait |
| any Worker → other Runtime | available | admit as External |
| any Worker → other Runtime | full | immediate capacity rejection |
| Worker → same Runtime | n/a | Internal Submission, no external slot |

### Rationale

Worker 是其源 Runtime 的有限进度资源，不能被另一 Runtime 的入口容量无限扣留。立即拒绝让 Callable 能显式降级、重试或传播失败，同时保持 Runtime 隔离。

### Rejected alternatives

- Cross-Runtime Worker 按 Block 等待：可形成跨 Runtime Worker starvation/deadlock。
- Worker 帮助目标 Runtime 腾 slot：破坏 D-051 Runtime ownership。
- Worker CallerRuns 目标 Task：完全绕过目标调度与隔离。
- 同 Runtime Internal 也 Block：在满队列/单 Worker下自锁。

### Consequences

- 同一 SchedulerOptions 的 Block 对普通线程和 Worker 有明确 caller-relative 差异。
- 跨 Runtime 组件必须处理容量 rejection，不应假设 Block 必然等待到成功。
- Metrics 需按 caller kind 区分 capacity rejection。

### Non-goals and deferred risks

- 本决策不检测应用层跨 Runtime retry livelock。
- 本决策不提供跨 Runtime credit transfer。
- 本决策不改变 Internal/External Submission 的领域定义。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 Block 只阻塞普通线程，任何 Worker 对其他 Runtime 满容量提交都立即拒绝。
- Code or data evidence: D-002/D-003 定义 Internal/External，D-051 固定跨 Runtime Worker ownership，D-083 固定 Internal capacity exemption。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0024](../../docs/adr/0024-backpressure-limits-external-pending-work.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-086 — Block submit 对 slot 与 gate 变化无丢失唤醒且不保证 FIFO

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

普通线程在 capacity 满时 Block，需要同时等待 slot 释放和 lifecycle admission gate 关闭。若只由 slot release 唤醒，Shutdown 后可能永久阻塞；若 check/register 与状态变化存在窗口，也会丢失通知。承诺 FIFO 则会把 waiter 队列结构与线程调度变成公共契约。

### Decision

Block submission waiter 必须在同一 admission 同步域内观察 external slot availability 与 admission gate。slot 释放或 gate 从可接受状态关闭都必须可靠唤醒/使所有相关 waiter 具备重新判定资格，check/register 竞态不得丢失通知。被唤醒者重新竞争：若取得 slot 且 gate 仍允许则完成 admission；若 gate 已关闭则以 lifecycle rejection 返回；若 slot 被其他提交取得则继续等待。Runtime 不保证 FIFO、公平顺序、同时返回或从通知到线程运行的最大延迟。

### Invariants

- gate 关闭优先于之后的 slot availability；不得在 Stopping 之后因旧通知接受 External Submission。
- 每个成功 waiter 恰好占用一个 slot；不得 oversubscribe capacity。
- spurious wake-up 必须通过条件重检安全处理。
- Shutdown/Finalization 不得等待所有 blocked submit caller 退出后才关闭 gate；应先关闭并通知。
- waiter 被拒绝/异常退出不得消耗 slot。
- slot release path 不得同步等待某个特定提交者完成 admission。

### Scope and variants

| Wake reason / race | Result |
|---|---|
| slot available + gate open | one contender may admit |
| gate closes | all waiters eventually return lifecycle rejection |
| spurious/losing race | recheck and wait |
| task starts/pre-start cancels | releases one slot and notifies |

### Rationale

把 slot 与 lifecycle gate 放在同一条件协议中消除“shutdown 已完成但 submit 仍睡眠/后来入队”的竞态。拒绝 FIFO 保留 condition variable、semaphore 或 ticketed admission 等实现选择，并承认 OS 调度无法保证严格返回顺序。

### Rejected alternatives

- 只在 slot release 通知：Shutdown 可能让 waiter 永久阻塞。
- gate 关闭后仍让已等待者优先入队：违反 D-003 的线性化关闭。
- 固定 FIFO：增加 waiter 管理和取消复杂度，且不能保证线程实际调度顺序。
- notify-one gate closure：未获通知的 waiter 可能永远睡眠。

### Consequences

- admission primitive 需共同协调 lifecycle state、slot count 与 waiters。
- 并发测试要覆盖 full queue、start/cancel slot release、Graceful/Immediate/Finalization gate close。
- Metrics 可以记录 block duration 和 wake reason，但不形成延迟 SLA。

### Non-goals and deferred risks

- 本决策不提供 timed Block submit。
- 本决策不规定 condition variable 或 semaphore 实现。
- 本决策不保证无饥饿，只保证状态变化不丢失。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 Block submit 共同等待 slot/gate、无丢失唤醒，gate 关闭中断所有等待，但不承诺 FIFO。
- Code or data evidence: D-003 固定 External admission 线性化，D-083 固定 slot release，D-084/D-085 固定 Block caller。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0024](../../docs/adr/0024-backpressure-limits-external-pending-work.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-087 — Submission rejection 使用稳定原因枚举和专用异常

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-003 延期了 admission rejection 的公共表示，D-084 又要求 Reject/Block 能清楚报告容量与 lifecycle 关闭。若统一抛 `runtime_error` 或返回空 Handle，调用方无法可靠区分重试容量、停止生产和编程错误；若把 allocation/capture failure 也归类为 rejection，则会丢失原始异常语义。

### Decision

公共 rejection reason 固定为：

```cpp
enum class SubmissionError {
    NotRunning,
    Stopping,
    Stopped,
    CapacityExhausted
};
```

并提供 `astra::submission_rejected : std::runtime_error`，其 `SubmissionError reason() const noexcept` 返回稳定原因。`submit()` 在运行时 admission rejection 时抛出该类型；Scheduler `Created`/启动未成功对应 `NotRunning`，关停过程对应 `Stopping`，完成关停对应 `Stopped`，gate 开放但 External pending slot 不可取得且调用不等待对应 `CapacityExhausted`。配置错误、`std::bad_alloc`、Callable/参数 capture construction 异常和其他提交准备异常保持原异常类型，不包装为 `submission_rejected`。

### Invariants

- 每次 runtime rejection 恰好有一个最具体的 `SubmissionError`。
- lifecycle gate 已关闭时优先返回 `Stopping/Stopped`，不得因同时 capacity full 报 `CapacityExhausted`。
- rejected submission 不返回空/Cancelled Handle，不创建可观察 Task。
- `submission_rejected::what()` 文本不作为稳定 ABI；`reason()` 枚举才是程序化分支依据。
- Reaper Finalization 导致尚未 Running Scheduler 无法启动/提交时，对该 Scheduler 的 submit 统一按实际 SchedulerState 报 `NotRunning/Stopping/Stopped`；进程级原因不扩散为 Task admission enum。

### Scope and variants

| Condition | SubmissionError |
|---|---|
| Scheduler Created/not successfully started | `NotRunning` |
| Scheduler Graceful/Immediate Stopping | `Stopping` |
| Scheduler Stopped | `Stopped` |
| gate open + no external slot + nonblocking rejection | `CapacityExhausted` |
| allocation/capture/configuration failure | original exception, not SubmissionError |

### Rationale

小而稳定的原因枚举覆盖调用方真正可采取的动作：稍后启动、停止生产、永久停止使用或背压重试。专用异常让 primary `submit()` 保持直接返回 TaskHandle，同时不吞掉资源与用户类型异常。

### Rejected alternatives

- 返回 invalid TaskHandle：无法区分原因，且错误可能延迟到 get/wait。
- 所有失败统一 `runtime_error`：程序化恢复依赖 what 文本。
- `std::error_code`：当前错误域小且不需要跨系统 category/interoperability。
- 把 `bad_alloc` 包装为 CapacityExhausted：内存失败不等于配置 slot 满。
- 为 Graceful/Immediate 分别设错误：提交者只需知道 gate 已关闭，模式不改变恢复动作。

### Consequences

- 文档与测试必须断言 `reason()`，不匹配字符串。
- nonthrowing rejection path 复用同一 enum，见 D-088。
- 未来新增 admission gate 原因需判断是否真的改变调用方恢复策略，避免枚举膨胀。

### Non-goals and deferred risks

- 本决策不定义 Scheduler construction/start failure 类型。
- 本决策不提供 locale 化错误文本。
- 本决策不承诺 submission_rejected allocation-free construction。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 submit rejection 用专用异常携带稳定 lifecycle/capacity reason，资源与 capture 异常保持原类型。
- Code or data evidence: D-003 延期 rejection 表示，D-084 至 D-086 固定容量与 gate 行为。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0025](../../docs/adr/0025-submit-and-try-submit-share-one-admission-transaction.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-088 — try_submit 以 variant 返回 rejection 且永不等待 capacity

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

默认 Reject 下异常可能成为预期背压控制流；C++20 又没有 `std::expected`。仅提供 throwing submit 会迫使高频生产者捕获异常，而让 `try_submit()` 仍遵循 Block 配置则违背 try 前缀的即时尝试含义。

### Decision

公共 API 提供：

```cpp
template<class T>
using SubmissionResult =
    std::variant<TaskHandle<T>, SubmissionError>;
```

`Scheduler::try_submit(F&&, Args&&...)` 使用与 `submit()` 相同的 invocation/result deduction，但永不因 external capacity 等待：成功返回 variant 的 `TaskHandle<T>` alternative；runtime admission rejection 返回 `SubmissionError` alternative，即使配置 policy 为 Block。`try_submit()` 不是 `noexcept`：allocation、Callable/参数 capture construction 等非 admission 异常保持原类型抛出。编译期不可调用/结果类型错误继续是约束失败。

### Invariants

- variant alternative 顺序固定为 index 0 `TaskHandle<T>`、index 1 `SubmissionError`。
- rejection alternative 不包含 invalid Handle，也不构造 Task。
- Worker 与普通线程的 `try_submit()` 都不等待 capacity；same-Runtime Internal Submission 因不使用 external slot可正常接受。
- lifecycle 与 capacity reason 使用 D-087 的同一枚举，不产生平行 error taxonomy。
- 返回的 variant 在两个 alternative 都可 noexcept move 的前提下不应进入 valueless-by-exception；公共文档仍推荐按 type 而非 index 操作。
- `try_submit()` 与 `submit()` 成功路径必须共享 D-089 的 admission transaction，不能形成较弱的“best effort enqueue”。

### Scope and variants

| API / condition | Behaviour |
|---|---|
| `submit`, Reject/full | throw `submission_rejected(CapacityExhausted)` |
| `submit`, Block eligible/full | wait per D-086 |
| `try_submit`, any policy/full | immediate error alternative |
| either API, gate closed | exception or error alternative with lifecycle reason |
| either API, capture/allocation throws | propagate original exception |

### Rationale

`SubmissionResult` 用标准 C++20 variant 避免引入一个不完整的通用 expected 实现，同时给无异常背压路径明确原因。让 try_submit 始终即时，使延迟语义从调用名即可推断。

### Rejected alternatives

- `optional<TaskHandle<T>>`：丢失 lifecycle 与 capacity reason。
- 自研通用 `expected<T,E>`：扩大项目公共基础库范围，不是 Scheduler 核心目标。
- 等待型 try_submit：名称与延迟行为冲突。
- try_submit 吞掉 bad_alloc/capture exception：把程序/资源故障伪装为可重试 admission rejection。
- out-parameter + enum：结果类型推导和泛型使用笨重。

### Consequences

- C++20 用户可以用 `holds_alternative/get_if/visit` 处理结果。
- 若未来项目基线升级 C++23，可以新增 expected adapter，但不得静默改变现有 alias ABI。
- API/compile tests 需覆盖两种 invocation form、void/move-only result 与所有错误原因。

### Non-goals and deferred risks

- 本决策不提供 timed/cancellable submit。
- 本决策不保证 capture/allocation failure 无副作用于调用方传入的 rvalue 对象。
- 本决策不定义 coroutine-aware async submit capacity wait。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 C++20 try_submit 用 variant 返回 Handle/error，永不等待 capacity，但不吞掉非 admission 异常。
- Code or data evidence: D-041/D-075 固定 TaskHandle/result deduction，D-084/D-085 固定 capacity caller 行为。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0025](../../docs/adr/0025-submit-and-try-submit-share-one-admission-transaction.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-089 — submit 成功与拒绝共享强异常安全 admission transaction

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Task preparation 涉及 gate 检查、external slot、Task Control Block/Callable construction、Task Identity、outstanding-work 和 queue/Waiting publication。若这些步骤只是顺序 check-then-enqueue，shutdown 可提前完成；若构造异常不回滚 slot/count，blocked producer 永久等待；若返回 Handle 后 publication 失败，则产生永不完成的 orphan Task。

### Decision

`submit()` 与 `try_submit()` 必须共享一个强异常安全 admission transaction。成功线性化点只能在以下事实全部不可失败地建立后发生：lifecycle/caller 允许；所需 external slot 已保留；Callable/参数与 Task Control Block/stop state/completion state 已成功构造；Task Identity 已分配；outstanding-work 已计入；Task 已以 Waiting 或 Ready 形式不可丢失地发布给 Runtime。成功方法可以在 Task 已开始甚至 Terminal 后才实际返回 Handle，但返回的 Handle 必须观察该真实 Task。任一步骤在成功线性化前失败或抛异常，必须回滚 slot/outstanding/publication ownership，不执行 Callable，不返回 Handle，并可靠唤醒可能因 slot 等待的提交者。

### Invariants

- admission 结果恰好为“成功 Task”或“无 Task rejection/exception”，不存在 orphan/intermediate public state。
- success publication 与 D-003 Running→Stopping gate 位于同一唯一顺序；先成功者纳入 Drain Work Closure，gate 先关闭者拒绝。
- queue/Waiting publication 之后不得再执行可能导致 transaction 无法提交的用户定义 move/copy。
- Task 可以在 submit 返回前被 Worker 执行；这不是 inline CallerRuns，执行线程仍是正常 Scheduler Worker。
- 构造失败允许内部 TaskId 序列出现 gap，但不得产生可观察 Handle/Trace Task lifecycle；若 Metrics 记录 attempt，必须与 accepted 区分。
- 回滚和 failure cleanup 不得抛出第二异常或泄漏 capacity/outstanding count。
- 对调用方传入 rvalue 的移动/capture 副作用遵循 C++ 构造语义，不承诺失败后恢复参数原值。

### Scope and variants

| Outcome | Runtime facts |
|---|---|
| success | published Task + outstanding + optional external slot + valid Handle |
| lifecycle/capacity rejection | no Task/publication/count |
| allocation/capture exception | rollback all Runtime reservations; original exception |
| task runs before method return | valid success; same Task Outcome observable |

### Rationale

单一 transaction 把 admission、shutdown、backpressure 和任务完成核算连接成可证明边界。强异常安全针对 Runtime 状态，而不作不可能的用户 rvalue 回滚承诺，既防止 orphan Handle 也防止容量泄漏。

### Rejected alternatives

- 先返回 Handle 再 enqueue：enqueue failure 会产生永久 pending Handle。
- 先 check Running 后无保护 enqueue：与 shutdown 形成孤儿竞态。
- 构造失败保留 slot 等待清理线程：可能永久压缩容量并增加恢复路径。
- submit 成功必须等到 Task 尚未开始：无必要地串行化 Worker 与 producer。
- try_submit 使用简化 transaction：产生不同正确性等级的 accepted Task。

### Consequences

- v0.1.0 可先用一把 admission mutex 实现，再以相同线性化契约优化。
- fault-injection tests 必须覆盖每个可抛构造/分配点与 shutdown 竞态。
- Trace/Metrics 的 accepted event 必须在线性化成功之后，attempt/rejected 可独立记录。

### Non-goals and deferred risks

- 本决策不要求整个 submit lock-free。
- 本决策不固定 TaskId 分配器或是否允许 gap。
- 本决策不保证用户 Callable 捕获析构为 noexcept；若析构在栈展开中抛出，遵循 C++ terminate 规则，不由 Runtime 修复。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 submit/try_submit 共享同一强异常安全 admission transaction，成功必有可完成 Task，失败完全回滚 Runtime 预留。
- Code or data evidence: D-003 固定 submit/shutdown 线性化，D-083/D-086 固定 slot，D-041 固定成功 Handle。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0025](../../docs/adr/0025-submit-and-try-submit-share-one-admission-transaction.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-090 — 版本化 Ready 路由保持 External global 与 Internal local

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-001 要求 v0.1.0 所有 Ready Task 使用 Global Injection Queue 作为正确性基线，完整项目又要求 Per-Worker Local Queue 与 Work Stealing。若后续版本不固定 Ready publication 的来源路由，External producer 可能并发写 owner-only deque，Internal Submission 也无法获得 locality；若反过来改写 v0.1.0 路由则失去基线可比性。

### Decision

v0.1.0 继续让所有 Ready Task 进入线程安全 FIFO Global Injection Queue。启用 Work-Stealing 的后续版本中：同 Runtime Worker 当前 Running Task 产生的 Internal Submission 在成功 admission 后由该 Worker owner-push 到自己的 Local Deque bottom；External Submission 进入 Global Injection Queue，任何外部线程和其他 Runtime Worker都不得直接写目标 Worker Local Deque。非 submit 路径产生的 Ready Task（DAG dependency release、Coroutine resume、timer/event wake）若 publication 当前发生在所属 Runtime Worker 上，默认进入该 Worker Local Deque；否则进入 Global Injection Queue。Priority/Deadline 后续可以在不改变 ownership 的前提下选择对应调度 band。

### Invariants

- Local Deque 的 bottom push/pop 只有 owner Worker 执行；其他 Worker 只能从 steal end 操作。
- External producer 永远通过线程安全 injection path，不依赖 TLS 假装成为 target owner。
- 跨 Runtime Worker 的 submission 在目标视角是 External，进入目标 Global Injection Queue。
- Ready publication 必须发生在 admission/outstanding-work 已可靠建立后，继续遵循 D-089。
- 路由位置不改变 Task Identity、TaskState、Terminal Outcome 或 Shutdown eligibility。
- v0.1.0 不为未来 locality 偷偷加入 Local Queue，确保 D-001 benchmark baseline 真实独立。

### Scope and variants

| Version/context | Ready destination |
|---|---|
| v0.1.0 any Ready Task | Global Injection Queue |
| WS same-Runtime Internal Submission | current owner Local bottom |
| WS External Submission | Global Injection Queue |
| WS DAG/resume/wake on owner Worker | current owner Local bottom |
| WS DAG/resume/wake off-Worker | Global Injection Queue |

### Rationale

该路由同时保留外部并发安全、内部 cache locality 和独立 baseline。把非 submit readiness 按 publication context 路由，可让 Coroutine/DAG 复用相同调度核心，而无需给外部事件线程 Local Deque 写权限。

### Rejected alternatives

- 所有版本所有任务走 Global Queue：无法实现计划内 per-worker locality/work stealing。
- External producer 随机 push 某个 Local Deque：破坏 owner-only Chase-Lev 假设。
- Internal Submission 始终 Global：递归 workload 竞争全局锁并失去局部性。
- v0.1.0 预装 Local Queue：污染全局队列性能基线。
- Readiness 总是回到原创建 Worker：Task 可能从未在某 Worker 创建，且造成 affinity hotspot。

### Consequences

- Runtime 需要可靠 TLS WorkerContext 与 Runtime Identity 判定。
- Global Queue 与 Local Deque 都存储统一 Ready Task reference/ownership。
- Priority/Deadline 设计必须保持 owner-only 与 external injection seam，而不是绕过路由。

### Non-goals and deferred risks

- 本决策不固定 Local Deque 的锁/无锁实现。
- 本决策不保证 Task Worker affinity。
- 本决策不定义 NUMA/topology-aware routing。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 v0.1 全 global，后续 external global、same-Runtime internal/local readiness owner-local 的版本化路由。
- Code or data evidence: D-001 固定 baseline，D-002/D-003 定义 Internal/External，D-051 固定 Runtime ownership。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0026](../../docs/adr/0026-work-stealing-balances-locality-with-global-fairness.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-091 — Worker 用默认 64 的 local burst 强制探测 Global Queue

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

纯 local-first LIFO 在递归 Internal Submission 持续产生时可以让 Local Deque 永不为空，从而无限推迟已经进入 Global Injection Queue 的 External Task。每次都先 Global 又会丢失 Work Stealing 的 locality 目标。

### Decision

`SchedulerOptions` 提供 `std::size_t local_burst_limit = 64`，必须大于 0。启用 Local Deque 的 Worker 最多连续从自己的 Local Deque 成功取得并开始 `local_burst_limit` 个 Task；达到阈值后，在开始下一个 Local Task 前必须至少探测一次 Global Injection Queue。若 Global pop 成功则执行该任务并把 local burst count 重置为 0；若 Global 为空，可立即继续 Local 并开始新的 burst。Local 为空时始终先探测 Global；只有 Local 与 Global 在本轮都未取得任务时才进入 steal round。

### Invariants

- burst 计数以成功 start 的 local Task 为准，不以空 pop/取消 claim 计数。
- Worker 执行 Global 或成功 stolen Task 后，local burst count 重置为 0。
- 达到 limit 的 Global probe 不能被“Local 仍非空”跳过。
- Global probe 只保证有界 service opportunity，不保证某个具体 External Task 的运行 deadline 或 FIFO across workers。
- v0.1.0 没有 Local Deque，此选项不改变其 all-global loop。
- Priority/Deadline 调度可以在各来源内部选择 Task，但不得无限取消 Global service opportunity；具体跨 band 规则后续固定。

### Scope and variants

| Condition | Next source action |
|---|---|
| local burst below limit | try Local first |
| local burst reaches limit | mandatory one Global probe |
| mandatory Global empty | start new local burst |
| Local empty | Global probe, then steal |
| Global/stolen success | reset burst |

### Rationale

有限 local burst 在 locality 与 External starvation 之间给出可测试边界。64 是默认调优起点而非实时 SLA；可配置允许 Benchmark 针对 workload/平台优化，同时保留大于 0 的公平性结构。

### Rejected alternatives

- 永远 local-first：递归 Internal workload 可饿死 Global work。
- 永远 global-first：外部流量可压住 Local fork-join，失去 locality。
- 随机在 local/global 二选一：难以提供确定 service bound，测试与调优不稳定。
- 固定不可配置 64：无法根据 task grain 与 contention 调优。
- limit 0 表示无限 local：静默关闭公平性边界。

### Consequences

- WorkerContext 增加轻量 consecutive-local counter。
- Metrics/Benchmark 要测 global service latency、locality 与 burst limit sweep。
- 该规则只防 source starvation，不替代 Priority/Deadline fairness。

### Non-goals and deferred risks

- 本决策不保证严格任务 FIFO。
- 本决策不规定一个 Task 的最大运行时间；长 Callable 仍会延迟 Global work。
- 本决策不固定 cache/NUMA 优化。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐默认 64 的可配置 local burst，周期性强制 Global probe 防止 External starvation。
- Code or data evidence: D-090 固定 source routing；草案原 worker loop 永远 local-first。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0026](../../docs/adr/0026-work-stealing-balances-locality-with-global-fairness.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-092 — Global FIFO、owner Local LIFO、thief 从 oldest end 窃取

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

队列两端语义决定 locality、外部相对顺序和 Chase-Lev API。若 locked Local Deque 与后续 Chase-Lev 使用不同方向，替换实现会改变调度行为；若 Global 使用 LIFO，外部洪泛会加剧旧任务饥饿。

### Decision

同一调度 band 内，Global Injection Queue 使用多生产者/多消费者 FIFO；Local Deque owner 从 bottom push、从 bottom pop，形成 owner 近似 LIFO；thief 从 top steal，优先取得 victim 最旧的 Ready Task。Phase 2 的带锁 Local Deque 和 Phase 3 Chase-Lev Deque 必须保持相同端点语义。队列顺序是 selection policy，不保证 completion 顺序。

### Invariants

- Global 同一 band 的成功 publication 具有可定义 FIFO order，但多个 Worker 并发 pop 后的 start/finish 顺序可不同。
- owner-only bottom 操作与 thief top 操作必须在最后元素竞争中恰好一个成功。
- 被成功 claim 的 Task 只能由一个 Worker start；取消/Immediate 与 claim 继续使用统一 start arbitration。
- Local LIFO 不得绕过 D-091 的 Global probe。
- Priority/Deadline band 之间的选择规则可以改变“先选哪个 band”，但每个 band 内端点语义保持。

### Scope and variants

| Source/operator | End/order |
|---|---|
| External Global producer | FIFO tail publication |
| Global consumer | FIFO head pop |
| Local owner producer | bottom push |
| Local owner consumer | bottom pop, newest first |
| thief | top steal, oldest first |

### Rationale

External FIFO 给等待时间较长的入口任务自然服务顺序；owner LIFO 改善递归工作集局部性；thief 取 oldest 降低与 owner 热端竞争并扩散更粗粒度的旧工作。这也是 Chase-Lev 的标准形状，可让 locked baseline 与无锁版本语义对齐。

### Rejected alternatives

- Global LIFO：持续外部提交可饿死旧任务。
- owner FIFO：降低递归/locality 优势，并与 Chase-Lev 典型访问模式不符。
- thief 从 bottom：与 owner 高频竞争同一端，最后元素冲突增加。
- Phase 2/3 端点语义不同：优化版本会产生功能行为漂移。

### Consequences

- 测试需要区分 dequeue selection order 与实际 start/completion order。
- Chase-Lev memory model 必须证明 last-item CAS 与 buffer visibility。
- Trace 应记录 source/end/steal，不应宣称全局严格 FIFO execution。

### Non-goals and deferred risks

- 本决策不保证跨 band/priority 的 FIFO。
- 本决策不提供 per-task affinity。
- 本决策不固定 batch stealing。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 Global FIFO、owner local bottom LIFO、thief top oldest，并跨 locked/Chase-Lev 保持一致。
- Code or data evidence: 总设计第 6 至 9 节采用该基本方向但未形成决策；D-001 要求 locked baseline 可比较。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0026](../../docs/adr/0026-work-stealing-balances-locality-with-global-fairness.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-093 — Steal round 默认探测最多 8 个不重复 victim

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

固定 Worker i→i+1 容易形成热点；每次 idle 扫描全部 N-1 Worker 在大 Runtime 上会产生 O(N²) 空闲探测流量；纯随机有放回又可能在一轮反复命中同一空 victim。需要定义单轮工作量和 victim 去重，而不承诺拓扑最优。

### Decision

`SchedulerOptions` 提供 `std::size_t steal_probe_limit = 8`，必须大于 0。一个 steal round 在 Worker 数 N>1 时最多探测 `min(N-1, steal_probe_limit)` 个互不重复的 victim；每轮使用 Worker-private pseudo-random state 选择起点/排列，排除自己，不访问共享全局 RNG。任一 steal 成功立即结束该 round 并执行任务；整轮失败后进入 D-094 后续定义的 idle/backoff 协议。N=1 时不产生 steal attempt。

### Invariants

- 同一 round 不重复探测 victim，也不选择自己。
- 多轮必须更新 pseudo-random state，避免永久固定邻居顺序；不保证每个 victim 在有限轮数内必被选中。
- probe limit 限制 victim 数，不限制单次 deque steal 原子重试的内部实现步骤。
- 成功 stolen Task 按 D-092 来自 victim top，并重置 D-091 local burst count。
- PRNG seed/算法不成为公共可复现序列 ABI；测试通过可注入内部 seam 获得确定性。
- Shutdown eligibility 在每个新 steal attempt 前重检，Immediate 不得 steal 并启动新任务。

### Scope and variants

| Runtime size/config | Round probes |
|---|---|
| N=1 | 0 |
| 1 < N <= limit+1 | at most all N-1 distinct victims |
| N > limit+1 | at most limit distinct victims |
| success at kth victim | stop round immediately |

### Rationale

默认 8 给中小 Runtime 足够扩散机会，同时限制大机器上的空 steal 流量。Worker-private randomization 避免共享热点，不重复 victim 提高每次探测的信息量；具体算法仍可随 Benchmark 优化。

### Rejected alternatives

- 固定 ring victim：容易形成同步热点和不均衡传播。
- 每轮扫描全部 Worker：大 N idle contention 成本高。
- 随机有放回：一轮可能浪费多次在同一 victim。
- 全局共享 RNG：引入新的争用点。
- limit=0 表示禁用 stealing：与启用 Work-Stealing 版本目标冲突；基线版本由版本功能开关区分。

### Consequences

- WorkerContext 维护轻量 PRNG state；测试需要 deterministic injection seam。
- Benchmark sweep 比较 probe limit、steal success 与 idle CPU。
- Topology/NUMA-aware victim policy 可在未来版本新增，但必须保留不重复和有界 round 原则或显式修订。

### Non-goals and deferred risks

- 本决策不提供 victim fairness SLA。
- 本决策不定义 batch steal 数量。
- 本决策不固定 PRNG 算法或 seed 对用户可见。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐每轮默认最多 8 个随机化、不重复 victim，成功即停，失败进入 idle。
- Code or data evidence: 总设计建议 pseudo-random victim 但未固定探测边界；D-092 固定 steal end。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0026](../../docs/adr/0026-work-stealing-balances-locality-with-global-fairness.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-094 — Idle Worker 使用有界 active backoff 后进入可通知 park

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Work-Stealing Worker 在 local/global/steal round 失败后，如果永久 busy-spin 会在空闲 Runtime 消耗整核；若立即进入 OS sleep，又会放大短间隔 workload 的唤醒延迟。固定公开 spin/yield 次数会把平台相关性能参数变成 API 契约。

### Decision

Worker 完成一次无任务的 local/global/steal search 后，必须经过有界 active backoff，再进入 D-095 的可通知 park；active backoff 可以按实现/平台组合 CPU pause、少量重新探测与 `std::this_thread::yield()`，但不得无限自旋。任何成功取得并开始 Task、观察到 work publication epoch 变化或 Shutdown mode 变化都重置 idle phase。具体 spin/yield 次数是内部 benchmark-tuned 参数，不进入稳定 `SchedulerOptions` 或功能验收 SLA。

### Invariants

- 没有工作且没有状态变化的 Worker 最终必须停止消耗持续 busy CPU 并 park。
- active backoff 期间每次尝试启动 Task 仍服从 D-091 至 D-093 和 Shutdown eligibility。
- backoff 不得用固定 sleep polling 作为发现工作的唯一机制；park 必须可被 publication 通知。
- 具体 pause/yield 次数变化不得改变 Task admission、selection source order 或 completion 语义。
- Helping Wait 的无工作等待可以复用相同通知核心，但保留外层 Running Task，不把 Worker 标记为可退出。

### Scope and variants

| Idle phase | Allowed action |
|---|---|
| initial active | bounded pause/recheck |
| scheduler yield | bounded OS yield/recheck |
| parked | block on D-095 epoch/state change |
| work/mode observed | reset and restart scheduling loop |

### Rationale

分阶段 idle 在低延迟与空闲功耗之间保留调优空间；只把“有界后必须 park”和“通知驱动恢复”写成语义，避免把某台机器的最佳 spin count 固定到公共配置。

### Rejected alternatives

- 无限 steal/spin：空闲时持续耗 CPU，并放大多 Worker contention。
- 失败一次立刻 sleep：burst workload 的唤醒成本过高。
- 固定周期 sleep polling：任务发现延迟取决于轮询周期且浪费 wakeups。
- 将所有 tuning knobs 首发公开：扩大配置面并让用户承担平台细节。

### Consequences

- Benchmark 必须覆盖 idle CPU、burst wake latency 和 oversubscription。
- 平台层可选择 pause/yield/atomic wait/condition variable，但需满足 D-095。
- Metrics/Trace 记录 idle phase transitions，而不把具体循环次数当兼容契约。

### Non-goals and deferred risks

- 本决策不承诺微秒级 wake latency。
- 本决策不固定自旋指令或 OS primitive。
- 本决策不定义 energy-aware/NUMA idle policy。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 Worker 使用有界 pause/yield backoff 后可通知 park，具体次数保持内部可调。
- Code or data evidence: D-093 固定 failed steal round 边界；总设计建议 spin/yield/sleep 但未固定正确性协议。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0027](../../docs/adr/0027-work-publication-and-parking-use-an-epoch-handshake.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-095 — Work publication 与 park 使用 epoch 双检握手防丢唤醒

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

经典竞态是 Worker 检查队列为空，producer 随后入队并通知，但 Worker 尚未真正进入 wait，通知丢失后它永久睡眠。Local Deque、Global Queue、DAG release、Coroutine resume 与 Shutdown 又来自不同同步域，不能依赖所有 publisher 持有同一队列 mutex。

### Decision

每个 Runtime 必须维护可原子观察的 work publication epoch 或语义等价 generation。任何可能使至少一个 Worker 获得新 Eligible Task、改变合法 selection 集合或要求 Worker 退出/重检的事件，必须先以 release 或等价同步完成状态/任务 publication，再推进 epoch，最后执行 D-096 notification。Worker 在完整 search 失败后读取 epoch snapshot，登记 park intent，然后再次以 acquire 或等价同步检查 local/global、Shutdown eligibility/exit condition 与 epoch；只有仍无工作、不可退出且 epoch 未变时才允许阻塞。epoch 在 Worker 睡眠登记窗口变化时，Worker 必须取消 park 并重新搜索。

### Invariants

- publish-before-epoch-before-notify 的 happens-before 顺序不可反转。
- Worker 的第二次检查发生在 park intent 对 publisher 可见之后；publisher 可用 sleeping count 优化通知，但该 hint 不得破坏 epoch 正确性。
- spurious wake、notify-before-wait 和 notify-after-wait 都必须安全，醒来后总是重检 predicate。
- Task publication、dependency release、Coroutine resume、timer/event wake、Graceful→Immediate、drain completion 与 worker exit request 都必须触发适当 epoch/state change。
- 仅 capacity slot release 不一定产生 Eligible Task，其 submit waiter 唤醒由 D-086 独立处理。
- 若固定宽度 epoch 达到饱和值，Runtime 必须进入不再 park 的安全 slow path或使用等价无 ABA generation；不得允许 wraparound equality 造成丢唤醒。
- epoch 是通知协调元数据，不定义任务先后顺序或计数每个 Task。

### Scope and variants

| Race/event | Required effect |
|---|---|
| task publishes before snapshot | second queue/state check observes it |
| task publishes between checks | epoch mismatch cancels park |
| task publishes after blocking | notification wakes waiter |
| shutdown changes with empty queues | epoch/state wake and exit recheck |
| spurious wake | harmless predicate recheck |

### Rationale

epoch 双检把多个任务来源压缩为一个可靠 wake generation，而不要求 Global Queue mutex 同时保护每个 Local Deque。明确饱和 slow path也避免用“2^64 不会发生”替代形式正确性。

### Rejected alternatives

- 只用 condition-variable notify 不维护 predicate generation：check-then-wait 窗口丢通知。
- 周期 timed wait 兜底：掩盖竞态并增加固定延迟/功耗。
- publisher 先 notify 再入队：Worker 可醒来、查空、再次睡眠。
- epoch 自然 wrap 且忽略 ABA：形式上允许 snapshot 与新状态相等。
- 每个队列独立 sleep primitive：Worker 需同时等待多个源，组合复杂且易丢事件。

### Consequences

- Runtime 需要统一 publication/wakeup facade，队列实现不能各自随意 notify。
- deterministic tests 应在两次 park 检查之间注入 publication/shutdown。
- atomic wait 或 condition variable 都可实现，只要证明相同握手与饱和行为。

### Non-goals and deferred risks

- 本决策不保证每次 epoch 变化唤醒多少 Worker，见 D-096。
- 本决策不要求 epoch lock-free。
- 本决策不提供跨进程 wakeup。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 publish→epoch→notify 与 Worker park 双检握手，并显式处理 generation wrap/饱和。
- Code or data evidence: D-090 定义多个 Ready 来源，D-080 定义 shutdown mode wake need；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0027](../../docs/adr/0027-work-publication-and-parking-use-an-epoch-handshake.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-096 — Ready publication 按可并行工作量通知而控制面变化 notify-all

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

每个 Task 都 notify-all 会造成 thundering herd；永远 notify-one 又会在 batch publication 或多个独立 Ready Task 到来时保留过多 Sleeping Worker。Shutdown/Immediate 若只唤醒一个，其他 Worker 可能无法观察退出条件并完成 join。

### Decision

单个 Ready Task publication 在存在 parked Worker 时至少通知一个；一次原子/批量 publication 的 k 个可并行 Ready Task 应最多通知 `min(k, parked_workers)` 个，允许实现合并通知或由已唤醒 Worker 继续传播。Internal owner-local publication 也必须参与通知，使其他 parked Worker 有机会 steal，不能仅因 owner 当前 active 就跳过所有 wake signal。Graceful/Immediate mode change、drain/exit condition、Reaper/Shutdown 要求全部 Worker 重检的控制面变化必须 notify-all。通知数量是进度提示；D-095 epoch/predicate 仍是正确性来源。

### Invariants

- 任何从 0 Eligible 向至少 1 Eligible 的 publication 不得在存在 parked Worker 时完全不通知。
- 单 Task publication 不要求 notify-all，避免无谓 herd。
- notify-one 唤醒者若未取得该 Task，可重新 park；任务若已被其他 Worker取得则不构成丢工作。
- batch k 的“可并行”数量可低于节点数，例如同一串依赖只发布一个 Ready。
- Shutdown notify-all 必须发生在 mode/exit state publication 之后。
- parked_workers/sleeping count 仅作优化；计数竞态必须由 epoch handshake 补偿。
- 不承诺具体 Worker、公平性或唤醒到 start 的时间上限。

### Scope and variants

| Publication | Wake policy |
|---|---|
| one Ready Task | at least one parked Worker if any |
| k parallel Ready Tasks | up to min(k, parked), coalescing allowed |
| owner-local internal Task | notify at least one if parked exists |
| Shutdown/mode/exit change | notify-all |
| metrics-only update | no worker wake required |

### Rationale

按并行工作量逐步唤醒减少 herd，同时让 local fork 能扩散到 thief。控制面 notify-all 确保每个 Worker及时离开 park 并参与确定性关停；epoch 保证通知优化不会成为正确性单点。

### Rejected alternatives

- 所有 publication notify-all：高并发下 cache/OS wake storm。
- 永远 notify-one：批量 work 可能长期利用不足，关停也可能挂住 sleeper。
- Local publication 从不 notify：只有 owner 执行，Work Stealing 难以扩展并行度。
- 仅按 queue length 唤醒：DAG/priority bands 与并发 claim 使长度不是可靠唯一事实。

### Consequences

- Runtime 可维护 approximate parked count 和 wake budget。
- Stress tests 应覆盖 burst publication、owner-local fork、notify coalescing 与 shutdown sleepers。
- Metrics 记录 wake attempts、actual wakes、spurious/no-work wakes，供 idle tuning。

### Non-goals and deferred risks

- 本决策不保证立即唤醒全部可用 CPU。
- 本决策不固定 notify primitive 或 exact coalescing algorithm。
- 本决策不定义 priority-aware wake target。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐单/批量 Ready 按并行度通知，owner-local 也可唤醒 thief，控制面变化 notify-all。
- Code or data evidence: D-090/D-093 固定 local+steal 路径，D-095 固定通知正确性不依赖 fanout。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0027](../../docs/adr/0027-work-publication-and-parking-use-an-epoch-handshake.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-097 — Chase-Lev 先保留 seq_cst oracle 再采用论文 portable memory-order 版本

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Chase-Lev 原始 2005 算法以顺序一致模型描述，2013 弱内存论文给出 portable C11 与架构专用版本及证明。直接从伪代码“手调”C++ memory order 容易在 ARM/POWER 上产生 well-defined read、uniqueness 或 last-item race 错误；只保留 seq_cst 又失去项目的内存模型学习与性能价值。

### Decision

Phase 3 必须先实现内部 `seq_cst` Chase-Lev reference/oracle variant，通过单元、stress 与线性化测试后，再实现生产 portable C++20 variant。生产 variant 以 Lê、Pop、Cohen、Zappa Nardelli 2013 portable C11 algorithm 为规范来源，使用 D-098 固定的等价或更强 memory orders；不在稳定版本中使用 x86/ARM inline assembly 或依赖非标准硬件内存模型。seq_cst oracle 保留在测试/benchmark 构建中，用于差分验证和性能对照，不作为公共 SchedulerOptions。

### Invariants

- locked deque → seq_cst Chase-Lev → portable optimized Chase-Lev 按顺序交付，不跳过正确性 oracle。
- production memory-order 只能与论文映射等价或更强；任何弱化必须附 memory-model rationale、litmus/stress evidence 与独立决策。
- 架构专用优化不得成为跨 GCC/Clang/MSVC、x86-64/ARM64 正确性的前提。
- 两个 Chase-Lev variant 必须实现相同 owner bottom/thief top/Empty-Retry-Success抽象语义。
- Benchmark 必须同时报告 seq_cst 与 portable variant，证明复杂度换来的实际收益。

### Scope and variants

| Variant | Purpose | Availability |
|---|---|---|
| locked Local Deque | scheduling logic baseline | Phase 2 |
| seq_cst Chase-Lev | memory-model oracle | Phase 3 tests/bench |
| portable C++20 Chase-Lev | production | Phase 3+ |
| architecture-specific asm | excluded | no stable support |

### Rationale

分层 oracle 把算法线性化错误与 memory-order 优化错误分开。采用已发表 portable C11 证明路径而非自创 fence 组合，使 C++20 实现可在弱内存平台上审查；保留 seq_cst 版本给回归和教育对比持续价值。

### Rejected alternatives

- 直接实现“看起来够用”的 relaxed atomics：弱内存错误难以重现且缺乏证明来源。
- production 全 seq_cst 且删除 optimized variant：降低研究/性能价值，无法验证 memory-order收益。
- 只做 x86 TSO 版本：ARM64 与跨平台 CI 不受支持。
- 运行时选 memory-order variant：扩大公共配置并把测试 oracle 带入生产热路径。

### Consequences

- memory-model 文档必须逐行映射论文 Figure 1 与 C++20 操作。
- CI 需在 ARM64 真机/仿真或可用 runner 上执行 stress，TSan 只作为补充而非证明。
- 相关论文成为规范 evidence，不等同于项目验收测试。

### Non-goals and deferred risks

- 本决策不声称 C++20 translation 已被形式化证明。
- 本决策不提供 POWER/ARM handwritten barriers。
- 本决策不固定 compiler codegen。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐保留 seq_cst oracle，并让生产实现严格基于已证明的 portable C11 weak-memory mapping。
- Code or data evidence: Chase & Lev, “Dynamic Circular Work-Stealing Deque” (2005), https://doi.org/10.1145/1073970.1073974；Lê et al., “Correct and Efficient Work-Stealing for Weak Memory Models” (2013), https://doi.org/10.1145/2442516.2442524。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0028](../../docs/adr/0028-chase-lev-follows-the-proven-portable-memory-ordering.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-098 — Portable Chase-Lev 固定 publication、last-item 与 steal 的原子序

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

算法正确性依赖三个 ordering seam：push 必须先写 cell 再发布 bottom；owner pop 与 thief steal 争最后一个元素时至少一方必须看到一致 size 并由 top CAS 决胜；thief 必须在成功 reservation 前读取 cell、成功后才能使用。`memory_order_consume` 在 C++ 工具链中不可依赖，因此需采用更强 acquire。

### Decision

生产 C++20 variant 使用 `std::uint64_t` 原子 `top/bottom`、原子 active-buffer pointer 和 relaxed atomic Task pointer cells，并采用下列不弱于 2013 portable C11 mapping 的顺序：

- owner `push_bottom`: relaxed load bottom；acquire load top；relaxed/acquire-safe load active buffer；必要时 resize；relaxed store cell；release fence；relaxed store bottom publication。
- owner `pop_bottom`: relaxed load/decrement/store bottom；seq_cst fence；relaxed load top；若多于一个元素则 relaxed cell load 后成功；若恰好一个元素，以 seq_cst strong CAS `top: t→t+1` 与 thief 决胜，failure relaxed，并无论输赢恢复 canonical bottom；空队列也恢复 bottom。
- thief `steal_top`: acquire load top；seq_cst fence；acquire load bottom；若 `t < b`，acquire load active buffer（替代 consume）、relaxed load cell，再以 seq_cst strong CAS `top: t→t+1`，failure relaxed；CAS 成功才返回 Success，失败返回 Retry。
- resize 完整构造/copy 新 buffer 后，以 release store 发布 active-buffer pointer；后续 bottom publication 继续发布新 cell/content。

任何实现可以使用更强 order（包括 oracle 的 seq_cst），但 production 弱化上述任何一步必须新建决策与证据。

### Invariants

- cell 写入 happens-before 任何通过 bottom/array acquire 成功 claim 后的使用。
- last item 的 owner 与所有 thieves 中恰好一个 top CAS winner 获得 Task。
- failed CAS 不得 dereference/use 已读取 Task pointer，也不得改变 deque logical ownership。
- owner 在确认多于一个元素时无需 CAS；push 只有 owner 执行，无需 CAS。
- strong CAS 避免把 spurious failure 混入 last-item/Retry 语义；若改 weak 必须在内部循环保持相同抽象结果。
- `memory_order_consume` 不出现在项目实现/规范中。
- memory-order 注释必须解释对应 happens-before/linearization，不写“for safety”式无依据注释。

### Scope and variants

| Operation/path | Linearization/publication point |
|---|---|
| push | bottom publication after cell |
| steal success | successful top CAS |
| pop >1 | owner bottom reservation under proven ordering |
| pop last | successful top CAS |
| failed steal/last pop | no item ownership transfer |

### Rationale

该映射直接保留论文 portable C11 的 release fence、seq_cst fence/CAS 结构，并用 acquire 取代不可靠的 consume，牺牲少量潜在优化换取标准 C++20 可移植性。把 exact seam 写入决策可阻止未来“减少一个 fence”式无证据优化。

### Rejected alternatives

- 全部 relaxed：push visibility 与 last-item size view 不成立。
- 用 volatile 替代 atomics：不建立 C++ happens-before，仍有数据竞争。
- steal CAS 前不读 cell、CAS 后再读：slot 可能被 owner 循环复用并覆盖。
- consume array load：编译器支持/语义历史不稳定，收益不值得。
- compare_exchange_weak 单次调用：spurious failure 会伪装为真实争用。

### Consequences

- memory-model tests 包括 well-defined read、uniqueness、existence 和 last-item race。
- Sanitizer 无报告不能替代 ordering review；反之 TSan 对自定义同步误报需逐项解释。
- active buffer lifetime必须覆盖 thief 的 acquire load，见 D-099。

### Non-goals and deferred risks

- 本决策不规定 cache-line padding。
- 本决策不固定 atomic 是否 lock-free，fallback 由 D-101/平台策略决定。
- 本决策不允许用 benchmark 单独证明正确性。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐逐操作固定 portable paper mapping，并以 acquire 替代 consume。
- Code or data evidence: Lê et al. 2013 Figure 1 给出 portable C11 take/push/steal orders：https://fzn.fr/readings/ppopp13.pdf 。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0028](../../docs/adr/0028-chase-lev-follows-the-proven-portable-memory-ordering.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-099 — Chase-Lev 只增长且旧 Buffer 保留到 deque quiescent teardown

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Thief 可以在 owner resize 前后加载 active-buffer pointer，并在 owner 切换到新 buffer 后继续读取旧 buffer。立即 delete 或复用旧地址会造成 UAF/ABA；hazard pointers、epoch reclamation 或原论文 shared pool shrink 会显著扩大首个无锁版本的证明面。

### Decision

Phase 3 Chase-Lev buffer 使用 power-of-two 容量、满时 owner 单独分配两倍新 buffer并复制逻辑 `[top,bottom)` 区间，始终留一个空 cell；运行期不 shrink、不回收、不复用任何旧 buffer 地址。每个新 buffer 持有前代 buffer 的 owning link 或由 deque 的 retired list 持有；全部 active/retired buffers 只在该 Worker 已退出、Runtime 阻止新 steal、所有其他 Worker 已退出或至少已证明无 active thief，并由 Shutdown/Reaper 完成 join 后统一释放。

### Invariants

- active pointer publication 前新 buffer 完整构造并复制所有当时逻辑元素。
- 任意 thief 已加载的旧 buffer 在其整个 steal operation 期间保持分配且地址不复用。
- 运行期不执行 shrink、retired-buffer free 或 shared buffer pool return。
- doubling 时包含 active 在内的历史 buffer 总容量小于 active capacity 的两倍（忽略初始常数），每个 deque 的保留内存与其历史峰值线性相关。
- buffer teardown 不得早于 worker/thief quiescence 和 join；Scheduler Handle 消失不等于 deque 可释放。
- resize allocation failure 不得丢失已接受 Task；publication fallback 由 D-100 固定。

### Scope and variants

| Event | Buffer lifecycle |
|---|---|
| push fits | reuse active |
| full | allocate 2x, copy, publish, retain old |
| deque shrinks logically | no physical shrink |
| Runtime running/Stopping | retain all generations |
| post-join teardown | free all generations |

### Rationale

保留旧 buffer 直接满足论文 weak-memory proof 中“old arrays are never reused”的关键假设，并把 reclamation 与已经严格设计的 Runtime join/quiescence 边界对齐。代价是按历史峰值保留内存，但 doubling 几何和使总容量仍有明确上界，适合作为首个可审计无锁实现。

### Rejected alternatives

- resize 后立即 delete old：并发 thief 可能仍读取，产生 UAF。
- shared_ptr active buffer：每次 steal 引用计数进入热路径，且原子 shared_ptr 成本/证明面增加。
- hazard pointers/epochs 首发：把 deque 算法与独立 reclamation 系统同时引入，调试面过大。
- shrink + buffer pool：增加 active-array race与地址复用 ABA。
- 永久泄漏到进程退出：绕过 Runtime/Reaper 的资源完成保证。

### Consequences

- deque peak memory 可能在 workload 峰值后不下降，Metrics 必须暴露 active/retired buffer bytes。
- 未来若需要运行期 reclamation，必须独立 ADR/decision 并提供形式或压力证据。
- Runtime State/Worker join 是实际内存回收 boundary，而非 Scheduler Handle 或 queue empty。

### Non-goals and deferred risks

- 本决策不提供 shrink。
- 本决策不实现通用 hazard-pointer/epoch library。
- 本决策不承诺峰值后归还内存给 OS。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 grow-only、旧 buffer 地址不复用，并在 deque/worker 完全 quiescent join 后统一释放。
- Code or data evidence: Chase–Lev 2005 基本算法用 growable cyclic array；Lê et al. 2013 proof 明确使用 “old arrays are never reused” 假设：https://fzn.fr/readings/ppopp13.pdf 。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0029](../../docs/adr/0029-chase-lev-retains-old-buffers-until-worker-teardown.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-100 — Ready Task 采用侵入式调度引用并为 Local resize failure 回退 Global

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Local buffer resize 需要分配，而一个已经接受的 Waiting Task 变 Ready 时不能把 allocation failure 传播给已返回的 Handle，更不能丢失任务。Resize 复制同一 raw Task pointer 到新旧 buffer，如果每个物理 cell 都被当作 shared ownership，又会重复引用计数和释放。

### Decision

每个 Ready Task 的 Task Control Block 必须持有一个逻辑 Scheduling Reference，从 Ready publication 起直到某个 Worker 成功 claim/start，或 pre-start cancellation/Shutdown 最终移除其 queue entry。Global Injection Queue 使用 Task Control Block 内嵌 intrusive link 或等价的 admission 后无额外分配 enqueue 路径。Local Chase-Lev cell 存储 relaxed atomic raw TCB pointer；resize 对同一逻辑 virtual index 的 pointer copy 只是别名，不新增 Scheduling Reference。若 Local push 需要 resize 而 buffer allocation/copy preparation 失败，Runtime 必须把该 Ready Task 发布到 allocation-free Global Injection fallback，保持原 admission/Outcome；不得丢弃、inline 执行或因后台 readiness 抛异常。

### Invariants

- 一个逻辑 Ready entry 恰好拥有一个 Scheduling Reference，与 buffer generation/copy 数无关。
- failed thief 可读取 raw pointer 但在 top CAS 成功前不得 dereference、start 或 release；CAS 失败后只丢弃数值。
- 成功 owner/thief claim 原子取得 Scheduling Reference，随后 start/cancel arbitration 决定执行或释放。
- stale old-buffer cells 在 buffer teardown 时不得逐 cell release，它们不各自拥有引用。
- Global fallback enqueue 在 TCB 已构造后不分配；锁竞争允许，但不得失败。
- 新 submit 若在 TCB/Callable 构造前 allocation 失败仍按 D-089 rollback；本决策只保证已存在 TCB 的 Ready publication。
- fallback Task 继续触发 D-095/D-096 epoch notification，并遵循 Global source fairness。

### Scope and variants

| Path | Scheduling Reference |
|---|---|
| Global intrusive entry | one logical ref |
| Local active cell | same one logical ref |
| resize copy to new cell | alias, no increment |
| successful claim | transfer to Worker execution |
| losing CAS | no ownership |
| pre-start cancellation stale entry | eventual discard releases once |

### Rationale

逻辑引用与物理 buffer cell 分离，才能同时安全复制 resize、避免热路径 shared_ptr，并让 losing thief 不触碰可能已释放对象。预嵌 intrusive Global fallback 则给已经接受的 Task 提供 allocation failure 下的不可丢失出口。

### Rejected alternatives

- atomic shared_ptr per cell：steal/push 热路径引用计数重，resize 复制 ownership 复杂。
- raw pointer 无 Scheduling Reference：最后 Handle 消失或取消时可 UAF。
- resize allocation failure 终止 Runtime：可恢复的局部分配失败升级过度。
- background readiness 失败 Task：allocation failure 不是 Callable Exception Outcome，且可能错误传播 DAG。
- inline execute failed-local-push Task：绕过正常 routing/priority/trace。

### Consequences

- TCB 需要 intrusive queue link 和清晰的 logical ready ownership state。
- Global fallback/normal Global 可以共享同一个 mutex-protected intrusive MPMC deep module。
- Queue tests 必须用计数对象验证 resize copies、lost CAS、cancelled tombstone 和 teardown 不 double-release。

### Non-goals and deferred risks

- 本决策不要求 intrusive reference count；可用 Runtime-owned arena/other equivalent lifetime，只要语义相同。
- 本决策不固定 cancelled queue entry 是 eager unlink 还是 lazy tombstone。
- 本决策不保证 TCB allocation 永不失败。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐逻辑 Scheduling Reference 与 buffer cell 分离，并为已接受 Task 的 Local resize failure 提供 allocation-free Global fallback。
- Code or data evidence: D-043 要求无 Handle Task 继续，D-089 要求 accepted Task 不可丢失，D-099 resize 保留多代 buffer。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0029](../../docs/adr/0029-chase-lev-retains-old-buffers-until-worker-teardown.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-101 — Chase-Lev fast path 条件 lock-free 且索引高水位使用 quiescent rebase

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

原算法假设单调整数索引不溢出；固定宽度 C++ `uint64_t` 最终会 wrap，并可能让极旧 thief 的 CAS 遇到 ABA。声称整个 deque 永久 lock-free 会掩盖 resize allocation、平台 atomic fallback 和任何安全 rebase 协调。

### Decision

Chase-Lev 使用 `std::uint64_t` 虚拟索引，并保证逻辑 size 始终小于 `2^63`。当 owner 在 push 前观察任一索引进入预留高水位区（最迟在 `2^63`）时，启动该 deque 的极低频 quiescent rebase：原子阻止新 thief entry，等待已登记 active thief 离开，在 owner 独占且无 thief 时把 live logical interval 复制/规范化为从 0 开始，设置 `top=0, bottom=size`，再恢复 stealing。Thief entry 使用 maintenance flag + active guard 的双检协议，确保 owner 设置 maintenance 后不会遗漏新 entrant。该 maintenance 可以无界等待被 deschedule 的 thief，因此不宣称 wait-free；正常非 resize/rebase fast path 在所需 atomic lock-free 的平台上保持 Chase-Lev lock-free 特性。若目标平台的关键 64-bit atomics 非 lock-free，构建/运行时选择带锁 Local Deque fallback，而不伪称 lock-free。

### Invariants

- index 不得执行有歧义的 signed overflow 或在 wrap 后继续普通 CAS。
- rebase 开始后新 steal 返回 Retry/跳过 victim，不阻塞在 deque 内；已 active thief 结束后递减 guard。
- owner 只在 active thief 为 0 后复制/重置 index，且期间仍持有全部 Scheduling Reference。
- rebase 前后抽象 Task 顺序、唯一性与 existence 不变。
- rebase 不执行用户 Callable、不改变 TaskState/priority/deadline。
- 高水位与小位宽 test specialization 必须可注入，不能等真实 2^63 操作测试。
- “lock-free”报告必须注明 variant/platform/fast-path条件；带锁 fallback 仍保持功能语义。

### Scope and variants

| Condition | Behaviour |
|---|---|
| normal lock-free atomic platform | Chase-Lev fast path |
| resize | owner allocation/copy, not lock-free allocation |
| index high-water | quiescent rebase, may wait |
| critical atomic not lock-free | locked deque fallback |
| thief sees maintenance | Retry/other victim |

### Rationale

显式 rebase 消除“64 位永远不会 wrap”的形式缺口，同时把极端维护从常规热路径隔离。条件化 lock-free 声明比营销式全局保证准确，也保留 MSVC/GCC/Clang 和不同架构上的可移植 fallback。

### Rejected alternatives

- 忽略 uint64 wrap：理论上允许 ABA/错误 size。
- 使用 signed index 并依赖 overflow：C++ undefined behavior。
- 达到高水位 terminate：可通过安全 quiescence 恢复，不应崩溃进程。
- 128-bit atomic 作为硬要求：跨平台 lock-free 支持不足。
- 每次操作 hazard/epoch rebase：为极端事件增加热路径成本。
- 永远宣称 lock-free：resize/rebase/fallback 不满足。

### Consequences

- deque 增加 active-thief guard 与极冷 maintenance path。
- formal/stress tests 可用 8/16-bit index model 强制 wrap/rebase interleaving。
- Metrics 记录 resize/rebase/fallback variant，不需要把高水位作为公共配置。

### Non-goals and deferred risks

- 本决策不保证 rebase 完成时间有界。
- 本决策不让普通用户触发/控制 rebase。
- 本决策不改变 Runtime Shutdown 的无界等待事实。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐避免索引 wrap，采用极冷 quiescent rebase，并只对具体平台正常 fast path 作条件 lock-free 声明。
- Code or data evidence: Chase–Lev 2005 明确假设 top 不 overflow；D-018/D-020 已提供 Worker quiescence/join 语言。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0028](../../docs/adr/0028-chase-lev-follows-the-proven-portable-memory-ordering.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-102 — Deque 内部操作区分 Success、Empty 与 Retry

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Chase-Lev steal 在读取到候选元素后可能因 CAS 输给 owner/其他 thief；这表示竞争失败，不证明 victim 当前或随后为空。把它与 Empty 合并会污染 steal metrics、victim policy 和测试；返回裸空指针又无法区分。

### Decision

Local Deque 的内部 pop/steal 结果必须使用三态抽象：`Success(TaskRef)` 表示调用取得唯一 Scheduling Reference；`Empty` 表示该操作的线性化观察中没有可取得元素；`Retry` 表示遇到 last-item CAS 失败、maintenance/rebase 或其他允许重新选择/重试的竞争而未取得元素。Worker scheduling loop 可以把 Retry 计为 contention 后尝试同一/其他 victim，但不得把它计作 empty proof 或 Task cancellation。公共 Task API 不暴露该结果类型。

### Invariants

- 只有 Success 转移 Scheduling Reference 并允许 start arbitration。
- Empty/Retry 都不 dereference 或 release candidate raw pointer。
- steal CAS failure 必须返回 Retry；真正 `t >= b` 返回 Empty。
- owner pop last-item CAS failure返回 Retry/Empty-compatible no-task 给 owner loop，但 Metrics 必须保留 contention 分类。
- maintenance flag 导致的 thief skip 返回 Retry，不算 victim empty。
- 一个 steal round 的 probe budget按 victim attempt 计数；是否在同 victim 内重试由实现 tuning，但必须有界。

### Scope and variants

| Internal event | Result |
|---|---|
| unique claim | Success |
| observed no logical item | Empty |
| CAS lost | Retry |
| rebase maintenance | Retry |
| cancelled tombstone claimed | Success then discard/cancel accounting, not Empty |

### Rationale

三态把数据结构事实与竞争事实分开，使 scheduler 能选择合理 backoff，Metrics 能解释 steal failure，也避免 losing thief 对候选对象做生命周期操作。

### Rejected alternatives

- nullable Task pointer：Empty 与 contention 不可区分。
- CAS failure 当 Empty：低估 contention 并可能过早 park。
- 内部无限 CAS retry：一个热点 victim 可耗尽整个 steal round。
- 把 Retry 暴露给用户：这是 queue 实现细节，不是 Task result。

### Consequences

- deque unit tests 和 metrics 分别覆盖 empty/retry/success。
- scheduler 可在 Retry 后继续当前 round，不改变 D-093 victim budget。
- cancelled tombstone 的 discard 仍必须完成引用与 slot/accounting。

### Non-goals and deferred risks

- 本决策不固定同一 victim 的最大 CAS retry 次数。
- 本决策不承诺 Retry 后 victim 一定有工作。
- 本决策不定义 batch stealing。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 deque 内部显式区分唯一成功、真实空和竞争重试。
- Code or data evidence: Chase–Lev 2005 接口区分 Empty 与 Abort；D-092/D-093 固定 scheduler steal round。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0028](../../docs/adr/0028-chase-lev-follows-the-proven-portable-memory-ordering.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-103 — Chase-Lev 对 empty decrement、capacity 与 doubling 使用 checked arithmetic

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

论文以不溢出的自然数索引推理，但直接把 `bottom - 1` 翻译为 C++ unsigned arithmetic 会在初始/重基后的空 deque（bottom=0）下溢为 `UINT64_MAX`，随后可能错误读取 cell。Grow condition 若晚于 `size == capacity - 1` 又会占满最后保留 cell；doubling 也可能溢出 `size_t`。

### Decision

所有 Chase-Lev index/capacity arithmetic 必须显式 checked。Owner `pop_bottom()` 在对 bottom 做减法前若观察 `bottom == 0`，必须在 owner-only/rebase invariant 下直接返回 Empty，不执行 unsigned decrement；其他路径保证减法操作数大于 0。Logical size 仅在已证明 `bottom >= top` 的稳定计算点求差。Push 在 `size >= capacity - 1` 时、写入新 cell 前尝试 grow，保证始终至少一个 unused cell。Capacity 是至少 2 的 power of two，doubling 前检查不超过实现允许值且小于 `2^63`；无法安全 grow 时不 wrap/overwrite，按 D-100 把该 Ready Task路由到 Global fallback。

### Invariants

- 不发生 signed overflow、unsigned underflow 后参与逻辑 size/地址计算或 capacity doubling wrap。
- 初始状态和空 rebase canonical state `top=bottom=0` 的 pop 返回 Empty。
- `capacity - 1` 只在 capacity 已验证至少为 2 后计算。
- cell index mask `virtual_index & (capacity - 1)` 只用于已验证 power-of-two capacity。
- grow copy interval和 push virtual index 都在 checked `[top,bottom)`/bottom 范围内。
- capacity/grow failure 不改变 active buffer、top/bottom 或逻辑 Task ownership。
- 小位宽 model tests 必须覆盖 0、capacity-1、doubling limit 与 rebase threshold。

### Scope and variants

| Boundary | Behaviour |
|---|---|
| bottom=0 pop | Empty before decrement |
| size < capacity-1 | push current buffer |
| size >= capacity-1 | grow before push |
| doubling not representable/allowed | Global fallback |
| high-water index | D-101 quiescent rebase |

### Rationale

无锁证明建立在抽象整数与有效数组范围上；C++ 固定宽度翻译必须显式恢复这些前提。Checked arithmetic 让 sanitizer、model test 和 code review 能验证边界，而不是依赖极端输入“不会发生”。

### Rejected alternatives

- 直接照抄 `size_t b = bottom - 1`：初始空状态下溢并可能越界/误判非空。
- 依赖 unsigned wrap 是 defined：语言层 defined 不等于算法语义正确。
- 满 capacity 后再 grow：破坏保留 cell 与 cyclic alias proof。
- doubling overflow 后 terminate：已存在 allocation-free Global fallback，可安全保持进度。

### Consequences

- fast path 增加可预测的极少边界检查；正常 bottom>0 分支成本很低。
- 代码审查必须将算术前提与 memory-order proof 同等对待。
- property/model tests 使用缩小 index/capacity 类型快速覆盖真实 64 位难达边界。

### Non-goals and deferred risks

- 本决策不固定初始 capacity 数值。
- 本决策不公开 Local capacity 配置。
- 本决策不保证 Global fallback 本身 lock-free。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐对 pop underflow、one-empty-cell grow threshold 和 capacity doubling 全部使用显式 checked arithmetic。
- Code or data evidence: Chase–Lev 论文使用不溢出自然数模型；D-099 要求始终留一个空 cell，D-100 提供 grow failure fallback，D-101 处理索引高水位。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0028](../../docs/adr/0028-chase-lev-follows-the-proven-portable-memory-ordering.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-104 — TaskGraph 以 consuming freeze 形成单次执行 FrozenTaskGraph

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

可复用 Graph template 需要复制/重建每个 node Callable 和 result state，与项目已支持的 move-only Callable/result 冲突；允许运行期间修改则使 successor list、dependency count、cycle detection 和 Scheduling Reference 同时变化，证明面过大。

### Decision

稳定 DAG API 使用三个阶段类型：`TaskGraph` 是仅用于构建的 move-only builder；`FrozenTaskGraph TaskGraph::freeze() &&` 消费 builder，形成结构不可变、move-only 的单次执行图；`GraphRun Scheduler::run(FrozenTaskGraph&&)` 再消费 frozen graph并建立一次 execution instance。成功 run 后同一 FrozenTaskGraph 不得再次执行；需要重复 workload 时显式重建 graph/callables。Builder mutation 与 freeze 不是线程安全操作，需要调用方串行；Frozen 结构在 run 后只由 Runtime 读取。

### Invariants

- run 期间不得新增/删除 Node 或 Edge，不提供 thaw/unfreeze。
- move-only Callable 必须可作为 node body；冻结/运行不要求 CopyConstructible。
- Builder/FrozenGraph destruction 在未 run 时只释放定义资源，不提交或执行 Node。
- 空 FrozenTaskGraph 是合法输入，run 成功返回已完成的 GraphRun，不创建 Worker work。
- freeze 成功后 Node identity/edge set/initial dependency counts 不再变化。
- graph single-shot 不等于 TaskHandle single-observer；GraphRun 仍可复制/共享观察的具体语义后续固定。

### Scope and variants

| Phase/type | Mutable | Reusable execution |
|---|---|---|
| `TaskGraph` builder | yes, caller-serialized | not executable |
| `FrozenTaskGraph` | no | consumed once |
| `GraphRun` | execution state only | observes one run |
| repeated workload | rebuild graph | explicit |

### Rationale

consuming freeze 在类型系统中编码 build-before-run 和 single-shot ownership，同时保留 move-only Callable。不可变结构让 cycle validation、successor traversal 与 dependency counters 能在 Runtime 启动前一次建立。

### Rejected alternatives

- 同一 FrozenGraph 并发/重复 run：需要克隆 move-only Callable 或引入 Callable factory 协议。
- run 时隐式 freeze：validation error 与 admission error 混在一起，且 builder 是否还能修改不清楚。
- 运行期动态加边/节点：dependency/cycle/ready race 复杂度过高。
- 要求节点 `std::function` 可复制以支持复用：排除 move-only capture 并增加复制语义。

### Consequences

- 示例/API 改为 `auto frozen = std::move(graph).freeze(); auto run = scheduler.run(std::move(frozen));`。
- Graph template/cache 不进入首个完整 Runtime；未来若有用例应定义独立 factory/plan 类型。
- Node callable ownership 从 Builder→FrozenGraph→GraphRun 单向移动。

### Non-goals and deferred risks

- 本决策不支持动态图或循环工作流。
- 本决策不定义跨进程 graph serialization。
- 本决策不固定 node result/dataflow类型，当前 DAG 先定义控制依赖。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 consuming freeze 的 move-only 单次执行 DAG，以保留 move-only Callable 并冻结运行结构。
- Code or data evidence: D-075 支持 move-only Task result；总设计早期建议 freeze 但未固定 ownership/reuse。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0030](../../docs/adr/0030-task-graphs-are-validated-single-shot-executions.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-105 — freeze 拒绝 self/duplicate/cycle 并提供确定性 cycle witness

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

重复边会让 dependency counter 重复递减或永久不归零；self-edge 是最小环；一般 cycle 会让所有环内节点永久 Waiting。只返回 bool 无法定位大图错误，而让 run 后才发现会留下已经 admission 的永不完成 GraphRun。

### Decision

`freeze() &&` 必须在产生 FrozenTaskGraph 前验证所有 Node/Edge：Node token 属于该 builder；不存在 self-edge；同一有向 `(from,to)` 边不重复；全图无环。失败时抛公开 `astra::graph_validation_error : std::logic_error`，并通过稳定 `GraphValidationError reason() const noexcept` 区分 `ForeignNode`、`SelfEdge`、`DuplicateEdge`、`Cycle`。Cycle reason 必须携带按 Node insertion-id 表示、首尾同 Node 的一条确定性 cycle witness；相同 builder/edge insertion 顺序必须产生相同 witness。验证复杂度目标 O(V+E)，可先用 Kahn 判定，再用确定顺序 DFS 提取 witness。

### Invariants

- validation 发生在 Scheduler admission 之前，失败不创建 Runtime Task/GraphRun。
- Node insertion-id 在 builder 内唯一、单调且只用于 graph identity/order，不等于 Runtime TaskId。
- duplicate detection 不通过静默去重修复输入；调用方必须看到错误。
- cycle witness 中每一相邻 pair 都对应实际有向边，且至少包含 self closure；SelfEdge 单独优先报告。
- error 的 `what()` 文本不稳定，reason 与 NodeId witness 是程序化契约。
- freeze failure 后被 consuming 调用的 builder 只保证可析构/重新赋值，不保证保留全部 callable 状态供修复。

### Scope and variants

| Invalid graph | Reason |
|---|---|
| Node token from another graph | `ForeignNode` |
| A→A | `SelfEdge` |
| duplicate A→B | `DuplicateEdge` |
| A→...→A | `Cycle` + witness |
| empty/isolated nodes | valid |

### Rationale

把所有结构错误压到 freeze 边界可保证 Runtime 只接收 DAG。确定 witness 让测试、诊断与 AI/人类 review 可复现；拒绝静默去重避免隐藏 builder bug和 dependency count差异。

### Rejected alternatives

- run 时检测 cycle：已经创建任务状态，失败回滚复杂且可能永久 Waiting。
- 只用 dependency count 最终不归零判断：错误发现太晚且无 witness。
- duplicate edge 自动去重：掩盖调用方错误和成本。
- bool freeze：无法程序化诊断原因/节点。
- 维护动态 transitive closure：builder 每次加边成本高，single-shot freeze 无需。

### Consequences

- Builder 保存 insertion order 和可高效检测 duplicate 的临时结构。
- Graph validation tests 覆盖 disconnected cycles、multi-cycle deterministic witness、foreign tokens。
- 显式 DAG 不会出现 D-050 的未检测 indirect wait cycle；动态 TaskHandle waits 仍不受此保证。

### Non-goals and deferred risks

- 本决策不提供自动 cycle breaking。
- 本决策不保证 witness 是最短 cycle，只保证确定且真实。
- 本决策不验证 Callable 业务层资源依赖死锁。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 freeze 前拒绝 foreign/self/duplicate/cycle，并为 cycle 提供确定真实 witness。
- Code or data evidence: D-050 将显式依赖的 cycle detection 交给 DAG；总设计列出环/重复边风险。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0030](../../docs/adr/0030-task-graphs-are-validated-single-shot-executions.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-106 — External GraphRun 原子占用每 Node external pending slot

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

run 接受整个 graph 后，非 root Node 虽处于 Waiting 但已经持有 Callable/TCB，若只让 roots 或整个 graph 占一个 External Pending slot，大图可以绕过 D-083 的 Waiting+Ready backpressure。逐 node 边执行边 admission 又会产生半个 graph 被接受、其余被拒绝的不可恢复状态。

### Decision

从普通线程或其他 Runtime Worker 调用 `Scheduler::run()` 属于 External Graph Submission；成功 admission 必须原子为 graph 的每个 Node 预留一个 external pending slot，并将全部 Node 计入 outstanding work，随后只发布 zero-predecessor roots 为 Ready，其余为 Waiting。若 `node_count > external_pending_capacity`，即使 policy 为 Block 也立即以 `CapacityExhausted` 拒绝，因为条件不可能同时满足；否则 Reject/Block 与 Worker caller eligibility 复用 D-084 至 D-086。由同 Runtime Running Task 发起的 run 属于 Internal Graph Submission，不占 external slots但全部 Node 仍计入 Drain Work Closure。Empty graph 需要 0 slot 并立即完成。

### Invariants

- graph admission 是 all-or-nothing；不得返回部分 GraphRun、部分 Node Handle 或留下 Waiting orphan。
- external slot 逐 Node 在其首次 Running 或 pre-start Terminal 时恰好释放，沿用 D-083。
- Block 等待的是一次取得 node_count 个 slot；gate 关闭立即拒绝，不做部分 reservation。
- cross-Runtime Worker 在容量不足时按 D-085立即拒绝，不 Block。
- 构造 Node TCB/GraphRun state 失败必须回滚全部 slot/outstanding，遵循 D-089 强异常安全。
- frozen graph 被 move 给失败 run 后只保证资源安全，不保证可重试；需要重试时调用方应先用未来可能的 preflight 或重建 graph，本决策不提供隐式回退。

### Scope and variants

| Run caller/graph | Capacity behaviour |
|---|---|
| External V>capacity | immediate reject |
| External V<=capacity + Reject | atomic reserve V or reject |
| ordinary external + Block | wait until V slots/gate close |
| foreign Worker + insufficient | immediate reject |
| same-Runtime Internal | exempt slots, count all outstanding |
| empty | success completed, 0 slots |

### Rationale

按 Node 计入 Waiting/Ready capacity 保持 D-083 的内存保护含义；all-or-nothing transaction 保证 graph 的结构闭包不会因背压破裂。明确 V>capacity 永不等待避免 impossible condition hang。

### Rejected alternatives

- 每个 graph 只占一个 slot：大型 graph 绕过 backpressure。
- 只为 roots 占 slot：大量 Waiting node 绕过容量。
- Node 就绪时逐个 admission：后继可能被拒绝，GraphRun 无法兑现执行闭包。
- Block 分批保留 slot：多个大 graph 可能各占一部分后互相等待。
- capacity 不足时自动 Internal：越权绕过 caller classification。

### Consequences

- Graph size 受 External pending capacity 的直接上限；超大外部 graph 需提高配置或从受控内部 workflow 构造。
- admission 需要 bulk slot reservation 和 bulk rollback。
- Metrics 区分 graph attempts、nodes admitted 与 graph capacity rejection。

### Non-goals and deferred risks

- 本决策不提供 streaming/dynamic graph admission。
- 本决策不提供 graph preflight reservation token。
- 本决策不限制 Internal Graph 的 node count。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 External Graph 原子按全部 Node 占 Waiting/Ready slot，过大 graph 立即拒绝，Internal Graph 豁免 slot。
- Code or data evidence: D-083 定义 External Pending Capacity 覆盖 Waiting/Ready；D-089 固定 admission transaction。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0030](../../docs/adr/0030-task-graphs-are-validated-single-shot-executions.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-107 — DAG dependency release 使用 completion publication 后的 exactly-once countdown

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

多个 predecessor 可在不同 Worker 同时终结。Successor 必须在所有 predecessor Outcome 已发布后恰好一次从 Waiting 转为 Ready 或依传播策略终结；若先 decrement 再 publish predecessor Outcome，最后线程可能启动 successor 而看不到前驱结果，重复边/重复 completion 又会 underflow counter。

### Decision

Frozen graph 为每个 Node 固定 immutable predecessor count 和 successor list；GraphRun 为每个 Node 初始化 `remaining_predecessors`。每个 predecessor 必须先按 D-071 完整发布自己的 Terminal Outcome/TaskState，再以 release 或等价同步记录给每条唯一 outgoing edge 的 predecessor disposition，并对 successor counter执行一次 atomic decrement。只有观察到 `fetch_sub(...) == 1` 的唯一线程成为 successor release owner，以 acquire 或等价 fence 观察全部 predecessor dispositions，然后按后续 D-110 failure/cancellation policy 恰好一次发布 Ready 或 Terminal。Zero-predecessor root 由 graph admission owner 直接发布 Ready。

### Invariants

- 每条 frozen edge 对每次 GraphRun 恰好贡献一次 countdown，不多不少。
- counter 不得 underflow；duplicate edge 已由 D-105 排除，double completion由 Terminal Outcome exactly-once 排除。
- successor release owner 在决定前观察所有 predecessor 的完整 Terminal Outcome/category。
- 非 owner predecessor thread 不得提前 enqueue/cancel successor。
- Ready publication 先建立 Scheduling Reference，再走 D-090/D-095 路由与通知。
- GraphRun completion 只在全部 Node Terminal 后发布；queue empty 或 roots done 不足够。

### Scope and variants

| Node | Initial/release |
|---|---|
| root count=0 | Ready at admission commit |
| count=N>0 | Waiting |
| predecessor completion N→N-1 | remain Waiting |
| final 1→0 | unique release owner decides |

### Rationale

completion-before-countdown 与 release/acquire 汇合把 predecessor Outcomes 安全传给最后 release owner；原子 1→0 transition 提供唯一 successor publication，无需锁住整个 graph。

### Rejected alternatives

- decrement 后发布 predecessor Outcome：successor 可见 counter=0但结果未完成。
- 每个 predecessor 都检查 counter==0并 enqueue：可能重复 publication。
- 全 graph 一把锁：简单但 completion 热路径 contention 随边数增长。
- successor 主动轮询 predecessors：浪费 CPU并增加 wake complexity。

### Consequences

- Node execution state 包含 immutable edge arrays、atomic counter 与 predecessor disposition summary。
- fan-out completion cost O(out-degree)，属于 DAG Benchmark 指标。
- Failure/Cancelled disposition 的合并优先级需单独决定。

### Non-goals and deferred risks

- 本决策不定义自动 predecessor value injection。
- 本决策不规定 edge storage allocator/layout。
- 本决策不支持运行期新增 edge。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 predecessor Outcome 先发布、每 edge exactly-once decrement，并由唯一 1→0 owner释放 successor。
- Code or data evidence: D-071 固定 Outcome/terminal publication；D-105 排除 duplicate edge。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0030](../../docs/adr/0030-task-graphs-are-validated-single-shot-executions.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-108 — TaskGraph Node 是 void 控制任务且不提供隐式 typed dataflow

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Typed DAG dataflow 需要定义多 predecessor 参数拼接、move-only result fan-out、引用生命周期、failure typing 和 NodeKey→run-specific Handle 绑定，远超“DAG Task Graph”的控制依赖核心。若允许非 void Node 却丢弃结果，会制造静默错误。

### Decision

稳定 TaskGraph Node Callable 必须按 D-059 的普通/stop-token invocation selection 可调用，且选定 invocation 的结果恰好为 `void`；非 void 或引用结果在 `emplace()` 编译期拒绝。Graph Edge 只表达执行控制依赖，不自动把 predecessor value 注入 successor，也不为 Node 暴露 `TaskHandle<T>`。需要数据传递时，Node 明确捕获/引用调用方管理的共享状态、消息对象或在 Graph 外组合普通 typed TaskHandle；其线程安全和生命周期由所选类型/所有者表达。

### Invariants

- Node 正常返回发布 Succeeded/Value(void)，异常和 `task_cancelled` 分别发布 Failed/Cancelled。
- Node stop-token injection 与普通 submit 使用同一优先普通 form 的规则，避免两套 Callable concept。
- Runtime 不静默 discard 非 void result。
- GraphRun completion/report 不包含 Node value，只包含状态与 failure exception_ptr。
- 共享数据对象必须在全部访问 Node 期间存活；Graph single-shot ownership 不自动拥有任意裸引用 capture。

### Scope and variants

| Node Callable result | Graph emplace |
|---|---|
| `void` | supported |
| stop-token form returning void | supported per D-059 |
| object/reference result | compile-time rejected |
| data sharing | explicit captured/shared object |

### Rationale

控制 DAG 已能实现依赖释放、并行度、失败/取消传播与调度测试；不自动 typed dataflow 保持接口深度，并避免重复 TaskHandle 已解决的复杂结果 ownership。显式共享类型让 ownership 风险在用户代码可见。

### Rejected alternatives

- 任意返回值但忽略：静默丢失结果，易误用。
- 首发 typed NodeKey<T>/automatic injection：组合规则、fan-out 与 move-only result 证明面过大。
- Graph 内返回 std::any：丢失编译期类型安全并引入分配/cast。
- 为每个 Node 暴露 TaskHandle<void>：扩大单 Node 控制面并使 graph cancellation/ownership 分叉；GraphRun report 足够。

### Consequences

- TaskGraph 重点明确为调度控制图，而非数据流语言。
- 示例用显式共享 state 展示安全数据传递，并突出同步由 edge 建立但对象本身仍需正确访问。
- 未来 typed dataflow 应作为独立模块/版本，而不是修改控制 Edge 含义。

### Non-goals and deferred risks

- 本决策不提供自动 result fan-out/join。
- 本决策不验证 capture lifetime 或数据竞争。
- 本决策不禁止 Node 内使用普通 TaskHandle。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 TaskGraph 作为 void control-DAG，不静默丢值，也不首发自动 typed dataflow。
- Code or data evidence: D-059 固定 Callable invocation；D-104 选择 single-shot graph。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0030](../../docs/adr/0030-task-graphs-are-validated-single-shot-executions.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-109 — DAG Edge 支持 RequireSuccess 与 AfterCompletion 两种策略

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

若所有 edge 都只等待 completion，失败后的普通处理节点仍会运行并可能读取无效共享数据；若所有 edge 都 success-gated，清理、汇总和诊断节点无法在 predecessor Failed/Cancelled 后执行。

### Decision

每条冻结 Edge 固定一个 `DependencyPolicy`：`RequireSuccess`（builder API 默认）或 `AfterCompletion`。最后 predecessor countdown owner 按 D-107 汇合所有 disposition：只有所有 `RequireSuccess` predecessors 都为 Succeeded，且所有 predecessors 已 Terminal，successor 才发布 Ready；任一 `RequireSuccess` predecessor 为 Failed/Cancelled 时 successor 不执行 Callable并发布 Cancelled。`AfterCompletion` predecessor 的 Succeeded/Failed/Cancelled 都满足该 edge，只贡献完成顺序，允许 successor 运行清理/汇总逻辑。相同 `(from,to)` 不允许以不同 policy 重复建边，继续按 DuplicateEdge 拒绝。

### Invariants

- Edge policy 在 freeze 后不可变，run 时不从全局失败模式动态改写。
- successor 必须等所有 predecessor Terminal，即使阻断原因已提前出现，确保 deterministic report/countdown 与资源可见性。
- propagated Cancelled successor 不执行 Callable、不取得 Running 状态，并释放 external slot/outstanding once。
- AfterCompletion 不自动向 successor 注入 exception；它通过显式共享状态或 GraphReport 外部处理。
- 一个 successor 混合两种 edge 时，只要任一 RequireSuccess 不成功就取消；AfterCompletion 仍需完成 countdown。
- Graph-wide cancellation/Immediate 可直接取消尚未 Running successor，不因 AfterCompletion 获得 cleanup 豁免。

### Scope and variants

| Predecessor edge/outcome | Edge satisfied for run? |
|---|---|
| RequireSuccess + Succeeded | yes after terminal |
| RequireSuccess + Failed/Cancelled | no; successor Cancelled after all deps terminal |
| AfterCompletion + any terminal | yes |
| mixed + one required failure | successor Cancelled |

### Rationale

两个显式 policy 覆盖正常 pipeline 和 finally-style ordering，而不引入复杂 bool predicate/callback。默认 success-gated 是数据依赖的安全选择；AfterCompletion 必须由 builder 明确标注，风险可见。

### Rejected alternatives

- 所有 edge completion-only：失败后下游误运行风险高。
- 所有 edge success-only：无法表达 cleanup/diagnostic continuation。
- 失败立刻取消 successor 不等其他 predecessors：资源/报告顺序与 disposition 汇合复杂，其他 edge 仍需安全完成。
- 任意 predicate edge：执行时机和线程安全扩展过大。
- duplicate pair 以多 policy 共存：dependency count和语义含混。

### Consequences

- Frozen edge storage 保存 1-bit/enum policy；release owner汇总 required-failure bit。
- Tests 覆盖 diamond mixed-policy、多个 failure 与 all-terminal-before-propagation。
- GraphReport 需要区分直接 Cancelled 与 dependency-propagated Cancelled 的内部 reason/count。

### Non-goals and deferred risks

- 本决策不提供 retry、fallback branch 或 catch edge。
- 本决策不自动执行 cleanup under graph-wide cancellation。
- 本决策不传递 typed predecessor values。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 Edge 默认 RequireSuccess，并显式支持 AfterCompletion cleanup ordering。
- Code or data evidence: D-107 固定 predecessor disposition汇合；D-108 固定 control dependency。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0031](../../docs/adr/0031-graph-failures-cancel-only-required-descendants.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-110 — Node failure 只传播取消到 RequireSuccess descendants

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

一个 Node Failed 后，可以 fail-fast 取消整个 Graph，也可以让完全无依赖的 branch 继续。全图 fail-fast 会丢弃已接受的独立工作，并使多个并发 failure 只剩竞态赢家；完全继续又违反 RequireSuccess 数据/控制前提。

### Decision

Node Failed 或 Cancelled 后，Runtime 按 D-109 Edge policy 只阻止依赖其成功的 successor，并传递形成 Cancelled Terminal Outcome；该传播递归作用于 RequireSuccess descendants。与失败 Node 没有 RequireSuccess 依赖路径的独立 branch 继续正常调度；AfterCompletion successor 在全部 predecessor Terminal 后仍可运行。Node failure 本身不自动发起 Graph-wide cancellation，也不向已 Running sibling 发布 stop request。

### Invariants

- original Failed Node 保留 Exception/Failed Outcome；descendant propagation 不复制异常为新的 Failed Node。
- propagated descendants 使用 Cancelled Outcome，并在内部 reason 标记 `DependencyFailed` 或 `DependencyCancelled` 供 report/metrics区分。
- 多个 predecessor failure 并发时 successor只 Terminal一次；report 保留所有实际 Failed Node exception。
- independent branch 与 AfterCompletion continuation 不因其他 branch failure被隐式取消。
- Running successor意味着其 Required predecessors此前都 Succeeded；之后不可因别处 failure回滚 start。
- GraphRun 必须等待继续运行的 independent/cleanup branch全部 Terminal。

### Scope and variants

| Relation to failed node | Default behaviour |
|---|---|
| RequireSuccess descendant | propagated Cancelled before start |
| AfterCompletion successor | may run after all deps terminal |
| disconnected/independent branch | continues |
| already Running sibling | continues, no stop request |

### Rationale

局部 success-gated propagation 精确匹配 DAG dependency 语义，并最大化保留独立并行工作。原始异常只保留在真正抛出的 Node，避免制造虚假失败；GraphReport 聚合全部实际 failures。

### Rejected alternatives

- first failure cancels whole Graph：独立工作丢失，失败竞态使结果不确定。
- 所有 successor completion-only：RequireSuccess 名义失效。
- descendant 标记 Failed 并复制 exception：伪造多个执行失败，统计/诊断失真。
- 向 Running sibling stop：failure传播越过依赖边，改变业务策略。

### Consequences

- Graph state 在任何真实 Failed Node 存在时最终为 Failed，即使所有 descendants 是 Cancelled。
- cancellation reason 需要内部枚举但不成为 TaskState 新值。
- 用户需要全图 fail-fast 时显式调用 GraphRun request_cancel，或构建监督策略。

### Non-goals and deferred risks

- 本决策不实现 retry/circuit-breaker。
- 本决策不自动选择“最重要”failure。
- 本决策不定义 graph-wide cancel 与 failure 同时发生的 report precedence，见 D-111/D-112。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 failure 只取消 RequireSuccess descendants，independent/AfterCompletion branches 继续。
- Code or data evidence: D-109 固定 Edge policy；D-044 固定 Terminal Outcome 不可变。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0031](../../docs/adr/0031-graph-failures-cancel-only-required-descendants.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-111 — GraphRun request_cancel 显式请求全部非终态 Node 停止

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

局部 failure propagation 不提供全图 fail-fast/用户取消。Graph-wide Cancellation 需要覆盖 Waiting/Ready、Running 和未来 Suspended，并与每 Node start race、Scheduler Immediate 和重复 Handle request 协调。

### Decision

有效 `GraphRun` 提供 `void request_cancel() const noexcept`，可由任意应用线程/Worker及任意副本并发调用；它在全图 cancellation request 已可靠、幂等发布并向当时所有非终态 Node 分类传播后立即返回，不等待 Graph completion。尚未 Running 的 Waiting/Ready Node 与请求线性化竞争，取消先胜出则发布 Cancelled且永不执行；Running Node 收到 cooperative stop request并按 D-056 由真实退出决定 Outcome；Suspended Coroutine Node 由 Coroutine cancellation 决策处理。请求发布后新完成 predecessor不得再把已取消 Waiting successor发布 Ready。Terminal Node 不改写。

### Invariants

- Graph request 与每 Node start 形成唯一顺序，复用 D-052，不存在 Cancelled 后执行。
- Graph-wide request 覆盖 AfterCompletion Node；它不是 cleanup guarantee。
- request 不强杀 Worker/Callable，不等待不合作 Running Node。
- 多次/并发 request 不重复完成 Node、重复 stop callback 或创建第二 Graph generation。
- Node failure与 Graph cancel并发时，真实 Failed Outcome保持 Failed；尚未执行 descendants可因 graph request或dependency传播 Cancelled。
- Empty/已 Terminal Graph request 是无副作用 no-op。
- 空 GraphRun Handle 的 request_cancel 按 TaskHandle 风格为 no-op，其他 invalid操作后续同 D-112固定。

### Scope and variants

| Node state at request race | Effect |
|---|---|
| Waiting/Ready, cancel wins | Cancelled, no Callable |
| start wins/Running | stop request only |
| Suspended | deferred Coroutine rule |
| Terminal | no-op |

### Rationale

显式全图命令让调用方选择 fail-fast/用户取消，而默认 Node failure 仍局部传播。复用单 Task classification 避免图层发明强杀或伪终态；noexcept request便于关停与监督路径组合。

### Rejected alternatives

- first Node failure自动 request_cancel whole Graph：取消独立 branch且不可配置。
- 只取消 roots：已释放 descendants继续运行，语义不完整。
- Waiting cleanup AfterCompletion继续运行：与“取消全部非终态”冲突并扩大策略。
- request 同步等待：不合作 Node可无限阻塞命令。
- Running Node立即 Cancelled：伪造终态并破坏 RAII。

### Consequences

- GraphRun shared state 保存 graph stop source/request bit；Node token需要组合 graph-level request。
- Graph completion仍可能无界，等待 API 必须保留 unbounded/timed选择。
- Graph report区分 direct graph cancellation、dependency propagation与真实 task_cancelled。

### Non-goals and deferred risks

- 本决策不提供 cancellation reason 给用户 Callable，仍是 stop_token bool request。
- 本决策不保证 cancellation latency。
- Suspended Coroutine awaiter注销另行固定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 GraphRun 显式、幂等、request-only 地取消全部非终态 Node，复用单 Task start-vs-cancel语义。
- Code or data evidence: D-052 至 D-060 固定 Task cancellation；D-109/D-110 固定默认局部传播。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0031](../../docs/adr/0031-graph-failures-cancel-only-required-descendants.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-112 — GraphRun 共享不可变 GraphReport 与四态聚合结果

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Graph 可以同时包含多个 Failed、Cancelled 和 Succeeded Node，不能像单 Task 一样只存一个 exception_ptr。若 `get()` 只重抛第一个 failure，会丢失并发错误；若每次 report 动态遍历运行状态，则多观察者可能看到不同顺序/集合。

### Decision

GraphRun completion 时发布一个共享、不可变、按 GraphNodeId 升序规范化的 `GraphReport`，包含 Node 总数及 Succeeded/Failed/Cancelled counts、每个 Failed Node 的 `{GraphNodeId, exception_ptr}`、以及按内部取消原因聚合的 counts。公共 `GraphRunState` 为 `Running/Succeeded/Failed/Cancelled`：全部 Node Terminal 前为 Running；完成后若 Failed count>0 则 Failed，否则若 Cancelled count>0则 Cancelled，否则 Succeeded。有效 GraphRun 提供非阻塞线性化 `state()` 与左值限定 `const GraphReport& get_report() const &`；get_report执行无界完成等待但不自动重抛 Node exception，调用方可对 report 中 exception_ptr显式 `rethrow_exception`。临时/rvalue get_report编译期拒绝。

### Invariants

- GraphReport 只在全部 Node Terminal 且 counts/failure list构造完成后一次发布。
- 多个 GraphRun副本重复 get_report返回同一 report对象引用，观察不消费 exception_ptr。
- failure list包含所有真实 Failed Node，dependency-propagated Cancelled不伪装为 failure。
- 状态优先级固定 Failed > Cancelled > Succeeded，与 cancellation request先后无关，只依最终 Node Outcomes。
- Empty graph report总数/counts均0，状态 Succeeded。
- report lifetime由至少一个有效 GraphRun持有；rvalue API限制防最直接悬垂。
- report构造所需存储必须在 graph admission时预留或保证 completion path不因分配失败丢失报告。

### Scope and variants

| Final node aggregate | GraphRunState |
|---|---|
| any Failed | Failed |
| no Failed, any Cancelled | Cancelled |
| all Succeeded / empty | Succeeded |
| not all Terminal | Running |

### Rationale

不可变 aggregate report完整保留并发 failure，而不选择任意“第一个异常”。状态优先级给监控一个小而确定的摘要，显式 rethrow保留调用方对多异常处理策略的控制。

### Rejected alternatives

- GraphRun get重抛第一个异常：丢失其他 failure且“第一个”受调度竞态影响。
- Graph report每次扫描 live nodes：开销高且返回集合可能漂移。
- descendant propagated Cancelled复制 exception：夸大 failure 数。
- Graph state仅 Completed：监控无法区分成功/失败/取消。
- 按 completion order列 failure：结果顺序 nondeterministic，测试困难。

### Consequences

- Graph admission为 failure array/report预留 O(V) 或 bounded storage。
- GraphRun不复用 TaskHandle<T>，因为 aggregate result与多 failure语义不同。
- report可直接驱动 Metrics/Trace和测试断言。

### Non-goals and deferred risks

- 本决策不序列化 exception_ptr。
- 本决策不提供 automatic aggregate exception throw。
- 本决策不公开每个成功 Node value（Node为void）。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐 GraphRun发布 deterministic immutable aggregate report，状态按 Failed>Cancelled>Succeeded 聚合且不自动重抛任意单一失败。
- Code or data evidence: D-044/D-071 固定单 Task immutable publication；D-108固定void nodes；D-110固定多 failure保留。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0031](../../docs/adr/0031-graph-failures-cancel-only-required-descendants.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-113 — GraphRun wait/wait_for/get_report 复用 caller-relative Helping 与 self-run拒绝

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

GraphRun completion依赖全部 Node Terminal。普通 Worker若直接条件等待同 Runtime Graph，会产生 starvation；正在执行该 GraphRun Node 的 Task等待自己的 GraphRun则形成必然闭环，因为 Graph completion必须包含当前 Node terminal。跨 Runtime仍需保持 source ownership。

### Decision

GraphRun 是 copyable/movable shared capability，默认/moved-from为空并提供 `valid()`；invalid结果/等待/state操作抛 `std::logic_error`，invalid `request_cancel()` no-op。有效 GraphRun 提供 `void wait() const`、基于 steady clock的 `GraphWaitResult::{Completed,TimedOut} wait_for(duration) const` 与 D-112 `get_report() const &`。非 Worker执行无界/有界 blocking wait；Worker等待其他 GraphRun时同/跨 Runtime复用源 Runtime Helping Wait、D-066 timeout observation boundary、D-078/079 depth limit与D-080 Shutdown eligibility。当前正在执行的 Node对所属同一 GraphRun调用 wait/wait_for/get_report时，必须在任何 probe/Helping副作用前抛 `std::logic_error`，任何 duration均如此。

### Invariants

- wait/wait_for只同步 GraphReport publication，不重抛 Node failures；get_report返回report。
- Graph wait多观察者共享同一 completion且无丢失唤醒，顺序/延迟不保证。
- cross-Runtime Worker只帮助源 Runtime，不执行目标 Graph nodes。
- direct Graph self-run检测使用 GraphRun Identity，不只比较 Node/Task Identity。
- indirect跨 Graph/Task wait cycle仍不保证检测。
- TimedOut不请求 graph cancel、不伪造 report，后续等待继续同一 run。
- same GraphRun中的一个Node等待另一个普通 TaskHandle仍按TaskHandle规则，不自动视为Graph self-run；可能形成间接环，用户应使用Edge。

### Scope and variants

| Caller/target | Progress/result |
|---|---|
| non-Worker | block until report/deadline |
| Worker not in target run | source Helping Wait |
| Worker executing target run Node | logic_error before effects |
| timeout | TimedOut, run continues |

### Rationale

GraphRun复用已验证的caller-relative等待核心，同时增加比Task self-wait更宽的Graph membership检测，阻止“当前Node本身阻止全图完成”的确定死锁。聚合wait不传播Node异常，使监督代码先完成、后检查report。

### Rejected alternatives

- 所有Worker Graph wait拒绝：失去同Runtime fork/graph组合进度。
- Graph Node允许等待自己的GraphRun：completion定义使其必然不能正常返回。
- Worker阻塞OS线程：可能耗尽Worker Group。
- get_report重抛failure：与完整aggregate report冲突。
- TimedOut自动request_cancel：把观察变成策略。

### Consequences

- WorkerContext需要current GraphRun Identity（Node任务才设置）。
- Graph/Task wait共享内部wait-loop，但target completion与self-check策略可参数化。
- Tests覆盖Node self-run、foreign GraphRun、depth exceeded和timeout overshoot。

### Non-goals and deferred risks

- 本决策不检测两个GraphRun互等或Graph↔Task间接环。
- 本决策不提供stop-token wait/wait_until。
- 本决策不定义Coroutine co_await GraphRun，后续另定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 授权接受当前窗口推荐方案；推荐GraphRun复用caller-relative Helping，且任何Node等待所属GraphRun都作为direct self-run拒绝。
- Code or data evidence: D-047至D-051、D-061至D-066、D-078至D-080固定Task waiting；D-107/D-112固定Graph completion。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0031](../../docs/adr/0031-graph-failures-cancel-only-required-descendants.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-114 — astra::Task<T> 是 cold、move-only、single-shot coroutine owner

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Coroutine function 调用可以立即执行（hot）或先返回 suspended frame（cold）。Hot coroutine 可能在 Scheduler admission 前运行用户代码、抛异常或完成，破坏 Task Identity/Backpressure/Trace；copyable coroutine owner 又会产生双 destroy 风险。

### Decision

公共 `astra::Task<T>` 是 cold、move-only、single-shot coroutine frame owner：promise `initial_suspend()` 返回 `std::suspend_always`，调用 coroutine function 只完成 frame/参数/promise构造并返回 Task，不执行函数 body。Task 可默认构造为空；move 转移唯一 frame ownership 并使源为空；禁止复制。未被 spawn 的有效 Task 析构时在 initial/其他合法 suspended point调用 `destroy()` 恰好一次；空 Task析构no-op。Task 本身不提供 wait/get/state/cancel，也不是 TaskHandle；只有 D-115 spawn 后才成为 Runtime Task。

### Invariants

- coroutine body 在成功 Scheduler admission 和 Worker resume 前不得执行。
- Task owner始终至多一个；不得从多个 Task对象destroy同一frame。
- 默认/ moved-from Task 的 spawn作为 caller error，行为由 D-115固定。
- frame allocation/parameter copy发生在 coroutine function call时，可在spawn前抛出；Runtime尚未接管，不形成Task Outcome。
- coroutine reference parameter/capture 的目标生命周期由调用方负责；cold semantics不延长引用对象生命周期。
- Task<T> 的 T只允许D-075相同的void或去顶层cv可移动非引用对象；reference/immovable在编译期拒绝。

### Scope and variants

| Task object | Ownership/behaviour |
|---|---|
| freshly returned | owns initial-suspended frame |
| moved-to | destination owns, source empty |
| unspawned destruction | destroy suspended frame |
| successful spawn | ownership transfers to Runtime |
| empty/moved-from | no frame |

### Rationale

Cold single-owner Task把所有用户执行放到Scheduler admission之后，并使frame lifetime可由一个明确owner线性转移。它也让Backpressure rejection在body尚未执行时发生，符合普通submit语义。

### Rejected alternatives

- hot Task：用户代码可在Task Identity/admission前运行，异常边界与thread affinity漂移。
- copyable Task owner：需要共享destroy仲裁且容易double resume/destroy。
- Task本身同时是Future：混合未spawn frame owner与已spawn shared result capability。
- unspawned析构detach/leak frame：丢失参数/局部对象析构和内存。

### Consequences

- coroutine factory调用本身的allocation failure与spawn rejection是两个清楚阶段。
- structured composition使用TaskHandle/后续awaiter，不通过复制Task。
- examples必须避免在cold Task引用参数离开作用域后再spawn。

### Non-goals and deferred risks

- 本决策不提供generator/async-stream。
- 本决策不保证frame allocation elision。
- 本决策不定义custom coroutine allocator首发API。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐Task<T>为initial-suspend cold、move-only single owner，未spawn析构安全destroy。
- Code or data evidence: C++ draft [dcl.fct.def.coroutine] 定义initial/final await及frame destroy前提：https://eel.is/c++draft/dcl.fct.def.coroutine 。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0032](../../docs/adr/0032-coroutine-tasks-are-cold-and-runtime-owned-after-spawn.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-115 — spawn 强保证转移 frame 并返回统一 TaskHandle<T>

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Coroutine Task需要进入现有admission、Cancellation、Outcome与Metrics体系。若spawn返回另一种Future，会重复API；若rejection已窃取并destroy caller Task，普通容量重试会意外丢掉尚未执行的frame。

### Decision

`Scheduler::spawn(Task<T>&&)` 对有效cold Task执行与submit相同的Internal/External classification、external slot、Backpressure、lifecycle gate和强异常安全admission，成功返回 `TaskHandle<T>` 并把frame唯一ownership转移给Task Control Block；initial-suspended coroutine作为一个Ready Task发布。Admission rejection抛D-087 `submission_rejected`，并在失败返回/抛出时保持传入Task仍有效且拥有原frame，允许调用方重试或析构；TCB/allocation异常同样不转移frame。空/moved-from Task在任何admission副作用前抛`std::logic_error`。另提供永不capacity-block的 `try_spawn(Task<T>&&) -> SubmissionResult<T>`，runtime rejection保留Task ownership。

### Invariants

- 成功spawn恰好清空source Task并建立一个有效TaskHandle/Task Identity。
- 失败spawn不得resume/destroy/窃取source frame，不占slot/outstanding或发布Trace TaskStarted。
- spawned coroutine从首次Ready起与普通Task共享TaskState、stop state、Terminal Outcome和Handle并发语义。
- 一个Coroutine Task跨多次resume只占一个External pending slot；slot在首次Running时释放，不因Suspended重新占用。
- body可在spawn返回前由正常Worker首次resume/完成，但不得在caller thread inline执行。
- `try_spawn`只把runtime rejection转variant error；其他异常保持原类型，source ownership仍按transaction commit决定。

### Scope and variants

| Spawn path | Source Task after call |
|---|---|
| success | empty; Runtime owns frame |
| lifecycle/capacity rejection | still valid |
| allocation/TCB failure | still valid |
| empty source | logic_error, remains empty |

### Rationale

统一TaskHandle让Coroutine自然复用结果、异常、取消和等待生态；failure不消费rvalue source提供实际可用的强保证，尤其适合try_spawn容量重试。Cold Task确保retry前body没有副作用。

### Rejected alternatives

- spawn返回CoroutineHandle/Future新类型：重复Task result抽象。
- rejection销毁frame：try/backpressure重试会丢工作。
- spawn在caller线程先resume一次：破坏Scheduler thread affinity与admission-before-execution。
- 每次resume视为新External task/slot：一条逻辑Task反复backpressure，状态/Outcome分裂。

### Consequences

- spawn实现必须延迟source nulling到admission commit最后阶段。
- TaskHandle<T>无需知道结果来自Callable还是Coroutine。
- API tests覆盖rejection后retry、body未执行、frame析构一次。

### Non-goals and deferred risks

- 本决策不提供自动spawn-on-construction。
- 本决策不提供direct `co_await Task<T>`。
- 本决策不定义Graph coroutine node overload，后续固定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐spawn成功才转移frame并统一返回TaskHandle，rejection保持cold Task可重试。
- Code or data evidence: D-041/D-089固定TaskHandle和admission transaction；D-114固定cold frame ownership。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0032](../../docs/adr/0032-coroutine-tasks-are-cold-and-runtime-owned-after-spawn.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-116 — 每个 Coroutine Task 以唯一 resume ownership 驱动 Ready/Running/Suspended

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

C++标准要求resume对象当前确实suspended，并警告并发resume可产生data race/UB。Completion、stop callback、timer与event可能并发尝试唤醒同一frame；若每个callback直接resume，就会双resume或在Worker外运行。

### Decision

每个spawned Coroutine Task必须用内部单调resume-ownership状态机保证任一时刻最多一个resume segment：Ready publication持有一个Scheduling/Resume Ticket；唯一Worker claim执行 `Ready→Running` 后才可调用`coroutine_handle::resume()`；resume返回时若到达final suspend则进入Terminal publication路径，若在普通await point suspended则通过D-118 handshake提交 `Running→Suspended`。任一外部completion/cancellation source只能竞争把同一个Suspended generation转换为一个Ready ticket并按D-090路由，绝不得直接或并发调用resume。每个resume segment仍属于同一Task Identity/outstanding-work/Handle。

### Invariants

- resume只由所属Runtime正常Worker在成功claim Ready ticket后调用。
- 同一frame绝不并发resume，也不resume Running/final-suspended/destroyed frame。
- 一个suspension generation最多发布一个后续Ready ticket；losing wake source无副作用注销/释放registration。
- `Running→Suspended→Ready→Running`可重复，Task终态一旦发布停止所有future resume。
- Worker可与首次不同，Coroutine代码不得假设thread identity或跨suspend持有thread-affine锁。
- Resume callback不执行用户coroutine body，最多执行noexcept状态竞争、enqueue与notify。
- Public TaskState只显示Ready/Running/Suspended，不暴露Suspending/ResumeQueued等内部瞬态。

### Scope and variants

| Event | State/owner |
|---|---|
| spawn commit | Ready ticket |
| Worker claim | Running, sole resumer |
| ordinary await commit | Suspended generation |
| wake winner | Ready ticket on source Runtime |
| co_return/unhandled exception | final suspend then Terminal |

### Rationale

把resumption视为Scheduling Reference的特殊形式可复用现有queue/start arbitration，并将标准的“必须suspended且不能concurrent resume”前提编码为Runtime状态机。Callback只enqueue还保留worker affinity、Trace和Shutdown eligibility。

### Rejected alternatives

- completion线程inline resume：用户代码在线程来源不可控处运行并可能递归。
- 多callback各自resume：并发resume UB。
- 每次resume创建新Task Identity：结果、取消、Metrics与DAG Node分裂。
- Suspended继续标Running：监控与取消无法区分当前是否执行。

### Consequences

- TCB包含coroutine handle、resume generation/ticket和registration ownership。
- Race tests覆盖completion-before-arm、stop-vs-completion、double callback、worker migration。
- Awaitable integration必须实现统一suspend/wake handshake，见D-118。

### Non-goals and deferred risks

- 本决策不保证resume在原Worker。
- 本决策不支持并行resume同一frame。
- 本决策不定义arbitrary foreign awaitable cancellation。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐所有resume由唯一Ready ticket和Worker claim驱动，callback只竞争enqueue，绝不inline/concurrent resume。
- Code or data evidence: C++ draft规定resume非suspended coroutine为UB并警告concurrent resumption data race：https://eel.is/c++draft/dcl.fct.def.coroutine ，https://eel.is/c++draft/coroutine.handle 。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0032](../../docs/adr/0032-coroutine-tasks-are-cold-and-runtime-owned-after-spawn.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-117 — final_suspend 保持frame suspended并由Runtime恰好一次destroy

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

若`final_suspend`使用`suspend_never`，frame可能在resume调用内自动销毁，Runtime随后检查handle/done或访问promise会悬垂；若多个completion/Handle析构路径都destroy，又会double destroy。标准要求final await不可抛，destroy也只允许对suspended coroutine。

### Decision

`astra::Task<T>::promise_type::final_suspend() noexcept` 必须返回 `std::suspend_always`或等价always-suspend awaiter。`co_return`/`unhandled_exception`先把Value/Exception/Cancelled内容写入Runtime completion storage；当前唯一resume owner到达final suspend后，以release或等价同步发布Terminal Outcome/TaskState并通知waiters，然后在证明frame处于final suspend且没有active resume/awaiter registration会再访问frame时，由Runtime frame owner调用`destroy()`恰好一次。Result value/exception_ptr存储在独立TCB completion state，frame destroy后TaskHandle仍可重复观察。

### Invariants

- final_suspend与其await methods不得抛异常。
- Runtime不得在handle仍Running或非suspended时destroy。
- Terminal Outcome内容构造发生在completion publication和frame destroy之前。
- frame destroy执行promise、parameter copies和locals析构；异常不得逃出Runtime cleanup，析构抛异常遵循C++ terminate边界。
- Frame只由unspawnedTask owner或spawnedRuntime owner之一destroy，不重叠。
- TaskHandle最后副本销毁不直接destroy仍Running/Suspendedframe；Runtime execution ownership继续持有。
- final destroy后保留的TaskHandle不包含raw frame dependency。

### Scope and variants

| Frame path | Destroy owner/time |
|---|---|
| unspawned initial-suspended | Task destructor |
| spawned final-suspended | Runtime after outcome publication |
| spawned ordinary Suspended | not destroy merely for Handle loss/cancel request |
| admission rejection | caller Task still owns |

### Rationale

Always-suspend final point给Runtime一个符合标准前提的确定destroy边界，并把结果生命周期与frame生命周期解耦。这样TaskHandle可长期持有结果而不保留全部coroutine locals。

### Rejected alternatives

- final_suspend suspend_never：Runtime post-resume访问handle/promise可能UAF。
- completion callback/last Handle destroy frame：可能与resume竞态或过早结束Coroutine。
- 结果直接留在promise直到最后Handle：为结果长期保留整个frame和locals。
- frame泄漏到Runtime shutdown：违反资源完成边界。

### Consequences

- co_return value需构造/移动到TCB completion storage，继续满足D-075约束。
- tests用析构计数验证unspawned/rejected/completed各一路exactly once。
- Shutdown/Reaper必须等待可能永久Suspended foreign awaitable coroutine，不能强毁。

### Non-goals and deferred risks

- 本决策不保证用户析构函数noexcept。
- 本决策不提供frame pooling/custom allocator。
- 本决策不允许terminal frame由任意awaiter线程destroy。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐final suspend always-suspend且noexcept，Runtime在Outcome发布后唯一destroy，结果独立于frame存活。
- Code or data evidence: C++ draft [dcl.fct.def.coroutine] 要求final await不可potentially-throwing，destroy非suspended为UB：https://eel.is/c++draft/dcl.fct.def.coroutine 。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0032](../../docs/adr/0032-coroutine-tasks-are-cold-and-runtime-owned-after-spawn.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-118 — Astra awaiter 以 arm-trigger handshake 线性化 suspend 与wake

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

`await_ready()`看到未完成后，目标可能在`await_suspend()`注册continuation之前或期间完成。若completion直接enqueue，而current coroutine尚未提交Suspended，Worker可能在原resume返回前再次resume；若先标Suspended再注册，completion可能丢失导致永久挂起。

### Decision

所有Astra内建异步awaiter必须使用每suspension generation唯一的arm-trigger handshake。`await_suspend`先建立不可丢失的completion/stop registration，trigger可在此期间只把ticket标记Pending而不能resume/enqueue；若registration发现条件已满足，awaiter返回不挂起并保持当前Task Running。确需挂起时，当前resume owner先提交内部Suspending→公共Suspended并交出resume ownership，再arm ticket；arm与并发trigger中恰好一方成为wake winner，将该generation一次转换为Ready并按source Runtime路由/通知。任何loser只清理registration，不发布第二Ready。

### Invariants

- completion-before-register、during-register、after-arm三种时序都不得丢wake或double enqueue。
- 在current resume owner交出frame前，trigger不得调用resume或使另一Worker成功claim该frame。
- `await_suspend`返回false路径不发布Suspended/Ready，不递增outstanding或创建新Task。
- registration/trigger/cleanup必须noexcept作为Runtime正确性前提；无法注册所需内存应在suspend commit前同步抛出并让current coroutine继续异常展开。
- source Runtime/Task identity在ticket中稳定持有，frame/TCB lifetime覆盖所有registration callback。
- wake publication继续服从source Shutdown eligibility；Immediate可触发stop wake但不inline destroy。

### Scope and variants

| Race | Winner/result |
|---|---|
| condition already ready | no suspend |
| trigger before arm | pending; arm publishes one Ready |
| arm before trigger | trigger publishes one Ready |
| completion+stop concurrent | one trigger wins; D-119决定await_resume |

### Rationale

arm-trigger把语言级suspend window与Runtime Ready publication连接为一个可证明协议，避免最常见的lost wake和resume-before-suspend。它可复用于TaskHandle、GraphRun、timer和I/O adapter。

### Rejected alternatives

- callback直接resume：可能在await_suspend尚未返回时递归/并发resume。
- 先Suspended后普通callback注册：completion窗口丢失。
- 先注册且completion立即enqueue无arm：resume ownership尚未交出。
- 用短timeout polling检查目标：延迟、功耗且仍不解决destroy竞态。

### Consequences

- Awaiter/TCB增加小型generation ticket和registration state。
- deterministic seam需暂停在register/commit/arm各点注入completion/stop。
- 第三方awaitable若不使用该协议，其正确性/cancellation由第三方承担，Runtime不声称可控。

### Non-goals and deferred risks

- 本决策不规定具体callback容器/reclamation。
- 本决策不允许并行多个active await expression于同一coroutine。
- 本决策不定义await_resume返回值。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐所有内建awaiter用register→suspend commit→arm/trigger唯一winner协议消除lost/double wake。
- Code or data evidence: D-116要求唯一resume ownership；C++ draft规定resume必须针对suspended coroutine。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0033](../../docs/adr/0033-suspended-coroutine-cancellation-is-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-119 — Suspended cancellation 只唤醒内建awaiter而不强毁foreign suspension

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Suspended coroutine已执行并可能把continuation注册到外部awaiter。Runtime无法通用撤销任意第三方registration或证明callback不再访问frame；直接destroy会造成UAF。另一方面，Astra内建awaiter可以把stop request纳入D-118 trigger竞争，实现及时协作取消。

### Decision

Task/Graph/Immediate cancellation作用于Suspended Coroutine时必须发布其stop request，但不得仅因Suspended就发布Cancelled Outcome或destroy frame。若当前await是Astra内建cancellation-aware awaiter，registration必须把stop callback作为与正常completion并列的trigger；stop胜出时取消/失效正常registration、发布一个source-Runtime Ready ticket，并在`await_resume`作为显式cancellation point抛`task_cancelled`。若当前是无法被Runtime识别/撤销的foreign awaitable，Runtime只保留stop request并等待它自行resume；它可以永久Suspended，使Task/Shutdown/Finalization永久Pending。

### Invariants

- stop与normal completion最多一个wake winner，frame绝不double resume。
- stop wake不在callback线程运行coroutine，不直接destroy frame。
- 内建awaiter的`await_resume`观察到stop-winner后抛Cancellation Signal；Callable可catch并正常继续/返回，仍按D-058/D-056决定Outcome。
- target TaskHandle/GraphRun不会因waiter被cancel而自动request_cancel，除非用户显式请求。
- foreign awaitable的callback ownership未知时不得unregister/free frame猜测安全。
- Immediate Shutdown不获得比单Task request更强的Suspended frame销毁权限。
- Runtime文档必须把cancellation responsiveness区分为Astra-aware与foreign awaitable。

### Scope and variants

| Suspension kind | Stop behaviour |
|---|---|
| Astra TaskHandle/GraphRun/timer awaiter | compete trigger, enqueue resume, throw at await_resume |
| user awaiter implementing Astra protocol | same contract |
| arbitrary foreign awaitable | set stop request only; wait for natural resume |
| already final-suspended | terminal path/no rewrite |

### Rationale

该边界保持C++ frame/registration lifetime安全，同时让Runtime自带awaiters具有实际可用的cooperative cancellation。承认foreign awaitable可永久挂起比强毁或伪造完成更符合现有Shutdown设计。

### Rejected alternatives

- Suspended立即destroy：外部callback可能UAF，且destroy只允许suspended但不代表registration安全。
- Suspended立即发布Cancelled但保留frame：Outcome与仍可能resume执行的事实矛盾。
- 所有foreign awaitable强制支持stop：标准awaitable没有统一注销协议。
- stop winner同时取消被await目标：越权传播取消。
- cancellation callback inline resume：违反唯一Worker resume模型。

### Consequences

- 内建awaiter必须提供noexcept stop registration/cleanup或在commit前失败。
- Shutdown/Finalization诊断需要标识长期foreign suspension。
- 第三方integration文档需定义可选Astra cancellation-aware awaiter contract。

### Non-goals and deferred risks

- 本决策不提供强制Coroutine cancellation。
- 本决策不保证foreign event最终到达。
- 本决策不定义reason payload，stop_token仍只有请求状态。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐内建awaiter通过stop trigger唤醒并在await_resume协作抛取消，foreign awaitable只请求不强毁。
- Code or data evidence: D-052/D-056固定cooperative cancellation；D-117固定safe destroy boundary；C++ draft警告resume/destroy前提：https://eel.is/c++draft/coroutine.handle 。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0033](../../docs/adr/0033-suspended-coroutine-cancellation-is-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-120 — co_await 左值 TaskHandle 在 source Runtime 异步恢复并传播同一 Outcome

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Coroutine若调用blocking `get()`会使用Helping Wait并保留嵌套C++栈，失去suspension价值。Completion线程inline resume又破坏Runtime ownership。由于非void await_resume返回共享`const T&`，从临时/rvalue Handle await还会在awaiter销毁后悬垂。

### Decision

有效左值 `TaskHandle<T>` 提供仅限Astra-managed `astra::Task` promise使用的 `operator co_await() const &`；rvalue overload deleted。Awaiter复制Handle以覆盖suspension registration lifetime。进入await时先检查Direct Self-Await与source stop request：self在任何注册前抛`std::logic_error`；已stop则作为cancellation point抛`task_cancelled`。目标已Terminal时不suspend并由await_resume复用`get()`语义：非void返回同一`const T&`，void返回void，Exception/Cancelled按原规则抛出。目标未完成时用D-118注册completion与stop trigger、source Task转Suspended；winner只在source Runtime发布一个resume ticket。Cross-Runtime target只提供completion，不执行/恢复source frame。

### Invariants

- 只有`astra::Task` promise满足await_suspend约束；foreign coroutine尝试使用在编译期拒绝，而不是runtime TLS猜测。
- lvalue source Handle必须在返回引用使用期间保持有效；awaiter copy只覆盖suspension，不允许rvalue逃逸引用。
- await target completion不阻塞OS Worker、不进入Helping depth。
- completion winner与stop winner唯一：stop先线性化则await_resume抛task_cancelled；completion先线性化则传播target Outcome，之后的stop留给下一个cancellation point。
- waiter cancellation不取消target Task。
- direct self比较Task Identity；indirect await cycle仍不保证检测。
- source Graceful允许resume作为已接受Task继续执行；source Immediate通过stop trigger唤醒并取消点退出，不直接resume foreign Runtime。

### Scope and variants

| Await target/source | Behaviour |
|---|---|
| already Terminal | no suspend, propagate Outcome |
| incomplete same Runtime | suspend; completion queues source resume |
| incomplete other Runtime | suspend; target completion queues source Runtime resume |
| source stop wins | resume then throw task_cancelled |
| self | logic_error before registration |
| rvalue Handle | compile-time rejected |

### Rationale

左值await将TaskHandle的共享结果语义自然延伸到Coroutine，又避免blocking stack和临时引用悬垂。Source-Runtime resume保持WorkerContext、Shutdown、Metrics与Local/Global routing归属。

### Rejected alternatives

- await内部调用blocking get：占用Worker栈并可能深度超限。
- completion线程inline resume：并发/线程归属不可控。
- rvalue await：await_resume引用可能在awaiter销毁后悬垂。
- awaiter自动取消target：越权改变共享Task。
- foreign coroutine runtime检测后支持：没有可靠source Runtime/frame ownership协议。

### Consequences

- TaskHandle增加lvalue operator co_await与内部awaiter。
- await cycles通过Trace诊断，显式依赖仍推荐DAG。
- tests覆盖target/stop winner线性化、rvalue compile fail、cross-Runtime source resume。

### Non-goals and deferred risks

- 本决策不提供按值await result。
- 本决策不支持foreign coroutine executor interop。
- 本决策不检测indirect await cycle。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐TaskHandle左值co_await，source Runtime排队恢复、Outcome语义复用get且stop作为竞争trigger。
- Code or data evidence: D-076固定lvalue result lifetime；D-116/D-118/D-119固定resume/cancellation handshake。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0034](../../docs/adr/0034-coroutine-awaits-resume-only-through-the-source-runtime.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-121 — co_await 左值 GraphRun 返回共享 GraphReport 并拒绝 self-run

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

GraphRun blocking wait已有self-run拒绝和aggregate report，但Coroutine需要不占Worker栈地等待。Report同样通过共享const引用返回，临时GraphRun await存在悬垂风险；当前Graph Node await自己的run仍是必然completion环。

### Decision

有效左值 `GraphRun::operator co_await() const &` 仅允许Astra-managedTask promise，rvalue deleted。Awaiter复制GraphRun，已完成时不suspend并返回同一`const GraphReport&`；未完成时通过D-118 completion/stop handshake使source Task Suspended，winner只在source Runtime发布resume ticket。当前source Task若是目标GraphRun的Node，必须在任何registration/probe前抛`std::logic_error`。Source stop winner在await_resume抛`task_cancelled`，completion winner返回report且不自动重抛Node exceptions。

### Invariants

- Graph self-run检测使用GraphRun Identity并覆盖empty/positive duration之外的async路径。
- rvalue/temporary GraphRun await编译期拒绝，caller lvalue维持report lifetime。
- awaiter不执行Graph nodes、不帮助target Runtime、不阻塞Worker。
- waiter cancellation不request_cancel GraphRun。
- report publication先于resume trigger，await_resume观察完整immutable report。
- indirectGraph↔Task/Graph cycle不保证检测。

### Scope and variants

| Case | Await result |
|---|---|
| completed GraphRun | immediate const GraphReport& |
| incomplete | suspend/resume on source Runtime |
| current Node awaits own run | logic_error |
| source stop wins | task_cancelled |
| Failed GraphRun | report returned; no auto rethrow |

### Rationale

GraphRun async wait保持aggregate result设计，不人为挑选异常，并与TaskHandle共享同一source-resume协议。Self-run拒绝防止一个Node把自己包含的完成集合作为await目标。

### Rejected alternatives

- await_resume重抛first failure：丢失aggregate语义。
- 允许own GraphRun await：必然无法完成。
- rvalue await：report引用生命周期不安全。
- target completion thread resume：破坏Runtime ownership。

### Consequences

- GraphRun completion state支持coroutine continuation registration。
- structured Graph supervisor可在Coroutine中await report而不占Worker。
- tests复用D-113 self-run与cross-Runtime组合。

### Non-goals and deferred risks

- 本决策不提供GraphReport按值复制awaiter。
- 本决策不取消GraphRun。
- 本决策不支持foreign coroutine。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐GraphRun左值co_await返回aggregate report，source resume并拒绝own-run Node。
- Code or data evidence: D-112/D-113固定Graph report/wait；D-118/D-119固定awaiter handshake。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0034](../../docs/adr/0034-coroutine-awaits-resume-only-through-the-source-runtime.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-122 — cancellation_point 不挂起而 yield 总是经 Global 重新排队

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Coroutine需要显式协作取消点和主动让出执行权。若yield按普通Internal routing push Local bottom，owner下一次local pop可能立即取得同一Coroutine，外部/其他local工作难获公平机会；若yield只是`std::this_thread::yield()`，coroutine仍占据当前Worker调用栈且没有TaskState Suspended/Ready转换。

### Decision

提供仅限Astra-managedTask的两个内建awaiter：`astra::cancellation_point()` 的`await_ready`/`await_resume`不产生实际suspension，单次检查current Task stop token，已请求则抛`task_cancelled`，否则继续；`astra::yield()`总是形成一次合法suspension generation，若进入时stop已请求则不排队并在当前resume内抛`task_cancelled`，否则把同一Coroutine Task从Running经内部Suspended转换为Ready，并强制发布到source Runtime Global Injection Queue（不是owner Local bottom），通知其他Worker后当前resume返回。后续唯一Worker claim再resume。

### Invariants

- cancellation_point不分配、不注册callback、不改变TaskState。
- yield不创建新Task Identity、External slot或outstanding count。
- yield成功必须真正交还当前Worker loop，不能inline resume或仅调用OS yield。
- Global routing是Internal fairness override，不占External Pending Capacity。
- source Immediate/stop request优先使yield成为Cancellation Signal，不重新排队已取消intent的Coroutine。
- source Graceful允许yielded Ready Task继续属于Drain Work Closure。
- foreign coroutine使用这两个awaiter编译期拒绝。

### Scope and variants

| Awaiter/state | Behaviour |
|---|---|
| cancellation_point, no stop | continue synchronously |
| cancellation_point, stop | throw task_cancelled |
| yield, no stop | suspend, Global Ready publication |
| yield, stop | throw without requeue |

### Rationale

显式cancellation point与D-060函数helper一致，但适合Coroutine语法；Global requeue使yield兑现“给其他工作service opportunity”，避免Local LIFO立即自取，同时仍复用normal Scheduler path。

### Rejected alternatives

- yield push Local bottom：owner很可能立即pop同一Task，公平效果弱。
- yield调用`std::this_thread::yield`：没有suspend且仍占coroutine resume segment。
- yield创建新Task：Identity/Outcome/outstanding分裂。
- cancellation_point隐式request stop：观察与控制混淆。

### Consequences

- Global Queue可能承载内部yield work但不消耗external slot；Metrics需区分publication origin。
- CPU-bound Coroutine可定期co_await yield/cancellation_point实现合作式响应。
- Benchmark比较yield latency、公平性与Global contention。

### Non-goals and deferred risks

- 本决策不实现时间片抢占。
- 本决策不保证yield后由不同Worker恢复。
- 本决策不提供sleep/timer，Deadline阶段另定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐无挂起cancellation_point和真正suspend后Global重排队的yield。
- Code or data evidence: D-060提供stop-token helper，D-091提供Global service fairness，D-116固定resume ticket。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0034](../../docs/adr/0034-coroutine-awaits-resume-only-through-the-source-runtime.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-123 — Graph 通过显式 emplace_coroutine 接受 Task<void> Node

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-108把普通Node Callable限制为void，但完整Runtime需要DAG Node能够Suspended/Resume。若Node内部spawn一个独立Coroutine Task再blocking wait，会创建第二Task Identity并增加Helping depth；若把任意返回Task的Callable自动识别为Coroutine，又使emplace overload/deduction含混。

### Decision

`TaskGraph` 提供显式 `emplace_coroutine(Task<void>&&)`，消费一个cold `astra::Task<void>` 作为单个Graph Node body；普通`emplace(F)`仍要求F invocation返回void。Coroutine frame ownership随Builder→FrozenGraph→GraphRun移动，run admission成功后绑定到该Node的同一个Task Control Block/GraphNodeId，不创建child TaskHandle、第二Task Identity、第二outstanding count或额外external slot。Node首次Ready由Workerresume initial-suspended frame，普通await转Suspended，最终Outcome直接成为Node Outcome并参与Edge release/report。

### Invariants

- 只接受Task<void>，非voidCoroutine Node编译期拒绝，保持Graph无typed value result。
- builder/freeze/run失败或从未run时，当前owner恰好一次destroy cold suspended frame。
- graph cancellation/Shutdown对Coroutine Node使用D-119 Suspended规则；foreign awaitable可使GraphRun永久Pending。
- Node每次resume仍按GraphRun Identity设置WorkerContext，own GraphRun await继续拒绝。
- frame final destroy与Node Terminal publication遵循D-117。
- `emplace_coroutine`命名显式，不对任意Callable return type做隐式magic。

### Scope and variants

| Graph node creation | Supported |
|---|---|
| `emplace(void callable)` | yes |
| `emplace_coroutine(Task<void>&&)` | yes |
| callable returning Task<void> via ordinary emplace | no |
| Task<T nonvoid> node | no |

### Rationale

显式overload把Coroutine lifecycle整合为同一个Node，而不改变control-DAG的void contract或创建嵌套Task。Cold Task frame天然适合single-shot FrozenGraph ownership链。

### Rejected alternatives

- Node spawn child再wait：额外Identity/slot/Helping stack和取消传播。
- ordinary emplace自动unwrap Task<void>：模板诊断与行为不够显式。
- typedCoroutine Node：重新引入D-108延期的数据流问题。
- Graph不支持Coroutine Node：两个核心模块无法组合。

### Consequences

- Frozen Node body是void Callable或cold Task<void>的tagged representation。
- Graph stress tests覆盖suspend/resume、dependency release与cancel race。
- Future typed graph可新增独立dataflow node type，不修改当前Node语义。

### Non-goals and deferred risks

- 本决策不支持generator/stream Node。
- 本决策不自动spawn nested Task。
- 本决策不提供per-nodeTaskHandle。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐显式emplace_coroutine(Task<void>)把frame作为同一Graph Node执行，不创建child Task。
- Code or data evidence: D-108固定void control Node；D-114/D-115固定cold Task ownership；D-111固定Graph cancellation。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0030](../../docs/adr/0030-task-graphs-are-validated-single-shot-executions.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-124 — Task<T> 不直接 co_await，必须显式 spawn 后等待 TaskHandle

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

允许`co_await Task<T>`可隐式在current Runtime spawn child，但必须决定child cancellation、parent lifetime、异常传播、parent放弃时的结构化join和capacity；若只在同一frame symmetric transfer执行，又绕过Scheduler Task Identity/priority/trace。

### Decision

稳定`astra::Task<T>`不提供`operator co_await`。需要运行child coroutine时，调用方必须显式选择Scheduler并调用`spawn/try_spawn`取得TaskHandle，再对左值Handle `co_await`、wait或get。Runtime不执行隐式spawn、symmetric transfer或parent→child cancellation propagation。Graph内使用D-123显式Node API。

### Invariants

- 创建coldTask不绑定currentRuntime；只有spawn选择execution domain。
- 没有隐藏external/internal capacity admission或TaskIdentity。
- Parent取消不自动取消显式spawn child；用户可持有Handle并request_cancel，未来structured scope需独立设计。
- Task析构若未spawn安全destroy，不因出现在co_await表达式改变。
- 编译诊断应指出“spawn Task then await TaskHandle”。

### Scope and variants

| Composition | Stable path |
|---|---|
| run top-level Task | scheduler.spawn |
| await child result | spawn → lvalue TaskHandle co_await |
| graph coroutine node | emplace_coroutine |
| direct co_await Task | compile-time rejected |

### Rationale

显式spawn保留execution domain、admission、Identity和cancellation ownership，避免在一个operator中偷渡structured concurrency的大量未决语义。Task保持纯cold frame owner，TaskHandle保持唯一async result capability。

### Rejected alternatives

- implicit same-Runtime spawn：隐藏capacity/rejection与child lifetime。
- symmetric transfer inline child：绕过Scheduler和Trace。
- parent auto-cancel child：没有明确scope/join contract。
- Task既spawnable又awaitable但语义不同：泛型代码难以推断。

### Consequences

- Coroutine组合多一步显式spawn，但所有关键策略可见。
- 未来可新增StructuredTaskScope而不破坏Task/Handle基础语义。
- 文档不把Task<T>称为lazy future，应称cold coroutine owner。

### Non-goals and deferred risks

- 本决策不实现structured concurrency scope。
- 本决策不提供symmetric transfer优化。
- 本决策不阻止应用封装helper组合spawn+await。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐Task不可直接await，必须显式spawn获得Handle，避免隐藏child策略。
- Code or data evidence: D-114区分Task owner与TaskHandle；D-115固定spawn；D-120固定Handle await。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0034](../../docs/adr/0034-coroutine-awaits-resume-only-through-the-source-runtime.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-125 — 不增加 wait_until、stop-token blocking wait 或 callback completion API

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

TaskHandle/GraphRun已有wait、wait_for和co_await。再增加wait_until会引入system/steady clock选择，stop-token blocking wait需新的三态结果，公开completion callback又需要executor/reentrancy/lifetime协议。这些都可由现有最小组合覆盖或应由Coroutine adapter内部使用。

### Decision

计划内稳定TaskHandle与GraphRun不提供公共`wait_until`、带`std::stop_token`的blocking wait/wait_for overload，也不提供`on_complete(callback)`/continuation callback注册API。普通线程使用wait或循环wait_for自行结合外部停止策略；Astra coroutine使用D-120/D-121 cancellation-aware co_await；内部awaiter可访问私有completion registration seam但不得公开泄漏。FinalizationControl保持其已批准独立接口，不受本决策修改。

### Invariants

- 缺少stop-token wait不允许wait_for超时自动取消target。
- 公共用户callback不得在Task completion/destructor线程被Runtime隐式调用。
- 内部completion registration只能恢复Runtime-owned continuation ticket，不执行任意用户callback。
- wait_for继续用steady duration和Completed/TimedOut两态。
- 未来新增接口必须说明现有wait_for/co_await不能满足的具体用例与executor/lifetime语义。

### Scope and variants

| Need | Supported path |
|---|---|
| unbounded thread wait | wait/get/get_report |
| bounded thread wait | wait_for |
| cancellable async wait | Astra co_await |
| absolute deadline | caller converts to steady remaining duration |
| arbitrary callback | application coroutine/adapter, not core API |

### Rationale

这保留短而正交的public surface，并避免用户callback重入Runtime completion路径。Coroutine已经是项目批准的异步组合机制，内部registration无需升级为不受控callback API。

### Rejected alternatives

- wait_until(system_clock)：墙钟调整语义复杂。
- stop-token wait扩展现有result enum：增加StopRequested第三态并与target cancellation混淆。
- on_complete任意callback：需定义执行线程、异常、重入和destroy竞态。
- 每种组合都首发overload：扩大测试矩阵而缺乏独立能力。

### Consequences

- 普通线程的可取消等待可用短wait_for循环，代价是poll interval。
- 高效无轮询取消等待仅在Astra coroutine路径提供。
- interop库可在外层封装callback/expected，不改变core completion。

### Non-goals and deferred risks

- 本决策不禁止未来基于真实用例新增adapter library。
- 本决策不改变Scheduler shutdown/finalization waits。
- 本决策不提供foreign executor continuation。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner授权接受当前窗口推荐方案；推荐以wait/wait_for/co_await覆盖同步与异步，不扩张wait_until、stop-token阻塞和任意callbackAPI。
- Code or data evidence: D-061至D-066固定thread wait；D-120/D-121固定async wait。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0034](../../docs/adr/0034-coroutine-awaits-resume-only-through-the-source-runtime.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-126 — Coroutine sleep 只接受 steady-clock Wake Time 并且取消感知

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Coroutine需要一种不占用Worker的定时等待，但`system_clock`会因墙钟校准跳变，任意Clock模板会扩大跨Clock转换和测试矩阵；同时sleep必须与D-119的Suspended cancellation协议组合，而不能让timer和stop各自恢复一次frame。

### Decision

提供仅限Astra-managed `astra::Task` promise使用的`co_await astra::sleep_for(duration)`与`co_await astra::sleep_until(std::chrono::steady_clock::time_point)`，结果为`void`。`sleep_for`在进入awaiter时以`steady_clock::now()`计算Timer Wake Time，duration转换和time-point加法执行检查并在正向溢出时饱和到`steady_clock::time_point::max()`。非正duration或已经到期的Wake Time不建立timer registration、不真正suspend，但仍先观察current Task stop request：已stop抛`task_cancelled`，否则同步继续。

### Invariants

- Public sleep API不接受`system_clock::time_point`或任意Clock time point。
- 实际定时等待把Task从Running提交为Suspended，不占用Worker thread。
- Timer Wake Time表示“不早于该时刻重新具备Ready资格”，不表示开始或完成期限。
- Sleep awaiter是cancellation point；stop胜出时`await_resume`抛`task_cancelled`。
- rvalue/lifetime问题由无外部对象引用的awaiter value与Runtime timer registration共同覆盖。
- duration饱和不能wrap成过去时间或导致意外立即resume。

### Scope and variants

| Input | Behavior |
|---|---|
| positive duration / future steady time | register and suspend |
| non-positive duration / expired time, no stop | synchronous continuation |
| any input, stop already requested | throw task_cancelled without registration |
| system-clock time point | compile-time unsupported |

### Rationale

单一monotonic clock使sleep不受墙钟调整影响，并与现有wait_for和调度器内部时间测量一致。即时路径仍作为cancellation point，使循环代码无需在`0ms`边界另插停止检查。

### Rejected alternatives

- `system_clock` sleep-until：NTP或人工调时会改变等待语义。
- templated arbitrary Clock：需要定义Clock是否steady以及跨域换算误差。
- 非正duration总是强制yield：把定时等待和D-122的公平yield混为一谈。
- sleep不观察stop：Immediate shutdown可能被长期定时器拖住。

### Consequences

- 应用若持有墙钟目标，必须在边界层换算为剩余steady duration并承担墙钟变更策略。
- Tests使用可注入的内部steady clock seam或受控timer driver验证边界，不依赖真实长时间sleep。
- Timer Wake Time与后续Task Deadline必须在API、Metrics与Trace中使用不同名称。

### Non-goals and deferred risks

- 不承诺纳秒级精度、固定jitter或硬实时唤醒。
- 不提供blocking thread sleep API。
- 不支持周期timer、cron或墙钟日历调度。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受steady-clock、cancellation-aware且非正duration不真实挂起的Coroutine sleep。
- Code or data evidence: D-118/D-119定义awaiter trigger与Suspended cancellation；D-122定义内建cancellation point。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0035](../../docs/adr/0035-coroutine-timers-are-driven-by-runtime-workers.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-127 — Runtime Worker 驱动 indexed timer heap 而不新增 Timer Thread

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Timer expiry需要把Suspended Task重新发布为Ready。每Runtime专用timer thread能及时标记到期，但不能抢占正在执行的Callable，且会增加线程启动、shutdown、orphan handoff与Reaper join协议。仅用lazy tombstone的heap又会让大量已取消长期timer长期占用内存。

### Decision

每个Runtime State持有mutex保护、按`(WakeTime, TimerSequence)`全序排列的indexed min-heap。Worker在每次返回scheduler loop以及进入park前抽取全部已到期timer；无Ready work时，park deadline取当前最早Wake Time。插入一个新的更早timer必须参与D-096/D-103的publication epoch协议并至少通知一个parked Worker重新计算deadline。项目不创建per-Runtime或process-wide专用Timer Thread。

Timer registration拥有稳定TimerId和heap index。Timer expiry与source Task stop callback通过D-118同一generation的原子winner竞争；expiry winner移除entry后只向source Runtime Global Injection Queue发布一个resume ticket，stop winner也只发布一次并主动从heap执行O(log n) indexed erase。任何路径都不得在timer mutex内resume coroutine或运行用户代码。

### Invariants

- Heap tie使用单调TimerSequence，测试与Trace中的相同Wake Time顺序确定。
- timer mutex只保护registration/index/heap，不保护coroutine execution。
- 先从heap解除ownership，再在锁外发布Ready ticket与notify。
- stop/expiry最多一个winner；loser只能做幂等清理，不能二次enqueue。
- 取消长期timer后不留下直到原Wake Time才清理的heap tombstone。
- Timer resume进入Global Queue，不依赖原Worker Local Deque存活或亲和性。
- Timer registration和Task Control Block的引用关系必须避免cycle；Task terminal后不存在可触发的活跃timer。

### Scope and variants

| Event | Required action |
|---|---|
| insert not-earliest timer | heap insert; normal epoch policy |
| insert new earliest timer | heap insert; epoch advance and notify |
| Worker observes due timer | pop; win trigger; Global publish |
| stop wins | indexed erase; Global cancellation resume |
| all Workers busy | process due timers at next scheduler boundary |

### Rationale

Worker-driven timer queue复用既有调度线程和park handshake；专用线程不能让非抢占任务更早让出CPU，因此不值得扩大Runtime lifecycle。Indexed erase用适度锁内复杂度换取长期运行时可预测的空间占用。

### Rejected alternatives

- 每Runtime一个Timer Thread：扩大thread/lifecycle/reaper表面。
- 进程级共享Timer Service：引入跨Runtime registration lifetime和额外global contention/failure domain。
- lock-free timer wheel首发：取消删除、动态范围和精度契约尚无收益证据。
- lazy-cancel min-heap：长期Wake Time的取消风暴会造成无界tombstone retention。
- 到期线程inline resume：破坏D-116唯一resume owner和Scheduler策略。

### Consequences

- Worker parking需要同时观察work epoch与最早Wake Time。
- Timer heap是一个有锁的慢路径；高频timer吞吐需由Benchmark决定是否引入分层wheel。
- Tests需覆盖new-earliest lost wakeup、stop/expiry race、indexed erase与Runtime teardown。

### Non-goals and deferred risks

- 本决策不承诺在Worker被长Callable占用时准时处理到期timer。
- 本决策不实现timer batching/coalescing或platform high-resolution timer。
- 若Benchmark证明mutex heap瓶颈，未来可替换内部结构但不得改变D-126语义。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受per-Runtime indexed timer heap由Worker loop/park驱动，不新增timer thread。
- Code or data evidence: D-096/D-103定义publication/park epoch；D-116/D-118定义resume ticket和trigger handshake。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0035](../../docs/adr/0035-coroutine-timers-are-driven-by-runtime-workers.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-128 — Timer Wake Time 是 best-effort eligibility 边界并参与 Runtime drain

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

即使timer thread在目标时刻标记到期，非抢占Scheduler也无法保证Worker立即运行continuation。还必须决定Graceful shutdown是否等待已经接受的sleep，以及能否把timer当作外部尚未接受工作丢弃。

### Decision

Timer Wake Time只保证Runtime不会因该timer在目标时刻之前使Task Ready；到时后恢复与执行均为best-effort，不给出最大jitter或返回延迟。已注册timer属于其Task Identity和Drain Work Closure，不额外占External Pending slot、不新增outstanding Task count。Graceful shutdown保留并驱动这些timer，因此长期或饱和到`time_point::max()`的sleep可使无界shutdown/finalization wait长期不完成；Immediate shutdown向Suspended Task请求stop，由D-127的stop winner撤销timer并排队取消恢复。

### Invariants

- timer到期不是Task Terminal Outcome，也不直接减少outstanding count。
- sleep suspension/resume不进行第二次admission或capacity占用。
- Graceful不得为了缩短shutdown而改写Wake Time或伪造Cancelled。
- Immediate仍是cooperative cancellation：foreign awaitable例外保持D-119语义。
- Scheduler Handle消失后，Runtime State与Reaper路径继续保有timer heap直到Runtime完成。
- 文档不得把sleep精度或Deadline功能描述为硬实时保证。

### Scope and variants

| Lifecycle | Existing timer behavior |
|---|---|
| Running | normal best-effort expiry |
| Graceful | retained and drained at original Wake Time |
| Immediate | request stop; Astra timer registration is cancellable |
| orphan Runtime | Runtime State/Reaper path continues driving lifecycle |

### Rationale

accepted Task的sleep只是其执行过程的一部分；Graceful若暗中取消会违背Drain Work Closure。把期限表达为eligibility而非execution guarantee，诚实反映非抢占Work-Stealing Runtime的能力边界。

### Rejected alternatives

- Graceful立即触发全部timer：改变用户程序的时间语义。
- Graceful取消全部timer：把Graceful退化为局部Immediate。
- timer registration另占capacity/outstanding：重复核算同一Task。
- 承诺Wake Time后固定延迟内运行：长Callable与OS scheduling无法保证。

### Consequences

- 应用不得在需要有界关停时创建不可取消的超长期Graceful sleep；可在超时后显式升级Immediate。
- Metrics要区分timer lateness、ready queue wait和task runtime。
- Reaper Finalization的无界语义自然覆盖仍在sleep的orphan Runtime。

### Non-goals and deferred risks

- 不提供强制线程中断或preemption。
- 不替应用选择Graceful等待上限。
- Deadline scheduling对Task start miss的定义由后续独立决策确定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受Wake Time仅是best-effort eligibility边界，timer属于原Task与Drain Work Closure。
- Code or data evidence: D-004定义Drain Work Closure；D-014/D-028定义Immediate对Suspended工作的处理；D-126/D-127定义sleep与timer driver。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0035](../../docs/adr/0035-coroutine-timers-are-driven-by-runtime-workers.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-129 — Priority 是四级不可变调度提示并通过显式 TaskOptions 配置

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

过细整数优先级使策略、兼容性和测试矩阵膨胀；可运行期间任意reprioritize又需要跨Global/Local/steal队列定位并迁移Task。Priority还必须覆盖Callable、Coroutine与Graph Node，而不能只装饰一种submit入口。

### Decision

公开`enum class Priority : std::uint8_t { Low, Normal, High, Critical };`。Priority是Task在首次admission时解析并固定的base scheduling hint，不是抢占级别、权限或实时保证。最终统一配置值为`TaskOptions`，至少包含`Priority priority{Priority::Normal}`及D-132的optional Task Deadline；提供options-first的`submit/try_submit/spawn/try_spawn` overload，Graph提供对应`emplace/emplace_coroutine` node options overload。TaskHandle不提供`set_priority`/`boost`。

无options overload遵循上下文默认：External Submission解析为Normal；same-Runtime Internal Submission及其spawn默认继承current Task的base Priority；Graph Node未显式给options时在GraphRun admission时继承GraphRun提交上下文的base Priority，External GraphRun为Normal。显式`TaskOptions`总是覆盖继承，即使值为Normal。Coroutine每次resume和Graph Node整个生命周期保持已解析Priority。

### Invariants

- Priority在admission transaction成功前验证，拒绝/异常不消费Callable或cold Task的既有重试保证。
- Priority不改变External Pending slot、outstanding count或Task Identity。
- Higher Priority不允许绕过Runtime lifecycle、capacity、dependency、Wake Time或start arbitration。
- Internal默认继承只读取current Task上下文，不沿TaskHandle wait边动态传播。
- Cross-Runtime submission在目标Runtime视为External；无options时为Normal，不继承foreign Worker Priority。
- Graph successor Priority由自身已解析options决定，不因predecessor Priority改变。
- 未知枚举值在public boundary确定性拒绝为`std::invalid_argument`。

### Scope and variants

| Admission form | Resolved base Priority |
|---|---|
| external, no options | Normal |
| same-Runtime internal, no options | current Task Priority |
| explicit TaskOptions | options.priority |
| external GraphRun node, no node options | Normal |
| internal GraphRun node, no node options | submitting Task Priority |
| coroutine resume | original resolved Priority |

### Rationale

四级枚举足以表达少量业务紧迫度且保持策略可解释。admission-time不可变Priority避免公开queue migration协议；默认继承让fork/join子工作不会意外降为Normal，而显式options保留调用方控制。

### Rejected alternatives

- 0～1000整数：伪精度且很难定义相邻值差异。
- binary high/normal：缺少后台与极紧急两个常见层次。
- TaskHandle动态reprioritize：需要并发定位、迁移和已Running语义。
- wait时自动priority donation：动态wait graph可能成环且需可撤销boost。
- Cross-Runtime隐式继承：让目标策略受foreign execution context影响。

### Consequences

- TCB/Graph Node保存一个解析后的base Priority。
- API文档必须把Priority描述为hint并明确没有preemption/donation。
- Future structured scope若需要priority inheritance/donation，应作为独立协议设计。

### Non-goals and deferred risks

- 不提供用户自定义priority数量或比较器。
- 不提供CPU/IO class、tenant quota或权限检查。
- 不解决持锁低优先级Task导致的OS级priority inversion。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受四级、admission-time不可变Priority、Internal默认继承及统一TaskOptions API。
- Code or data evidence: D-083至D-090定义统一admission与Internal/External分类；D-114/D-123要求Callable、Coroutine和Graph组合。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0036](../../docs/adr/0036-priority-uses-four-weighted-queue-bands.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-130 — Global 与每个 Local Source 都按四个 Priority band 分区

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

若Priority只存在Global Queue，Internal Task一旦进入Local Deque就失去优先级语义；若用单个Chase-Lev并在中间搜索高Priority项，会破坏owner/thief端点算法与O(1)操作。单独高优先级central queue又会让所有高优先级Internal work失去locality。

### Decision

Global Injection Source按四个Priority band持有四条MPMC FIFO；每个Worker Local Source按四个band持有四条独立、同构的owner-bottom/thief-top deque，Phase 2 locked baseline与Phase 3 Chase-Lev均保持该形状。Task publication依据已解析base Priority恰好进入一个band；同一band继续服从D-092的Global FIFO、owner Local近似LIFO和thief oldest端点语义。

D-091的source policy保持外层约束：local burst未到阈值时先在Local bands选择，达到阈值必须在Global bands探测，Local和Global本轮均失败才进入steal round；Priority不得取消Global service opportunity。Steal对一个victim的四个band按D-131的Priority selection选择并从对应top窃取，不跨band移动cell。

### Invariants

- 每个Local band仍只有该Worker执行bottom push/pop，任意thief只执行top steal。
- Chase-Lev index、grow buffer与rebase协议按band独立，不共享top/bottom。
- Task scheduling reference只属于一个band/cell；band scan失败不复制ownership。
- Public TaskState不暴露band或source瞬态。
- Immediate cancellation与claim race在选出cell后继续使用统一arbitration。
- 空band必须被work-conserving fallback跳过，不能让Worker因目标band为空而park。

### Scope and variants

| Source | Per-Priority structure |
|---|---|
| Global Injection | 4 × MPMC FIFO |
| Worker Local | 4 × owner-bottom/thief-top deque |
| timer/coroutine resume after first start | Global band of original Priority |
| Deadline first-start work | D-133 dedicated Global deadline band |

### Rationale

band partition让每次队列操作仍为O(1)且不改变Chase-Lev证明，同时让Priority跨External、Internal、Coroutine和Graph一致。四倍固定小数量是可控内存成本。

### Rejected alternatives

- 只分Global：Internal Local work无Priority语义。
- 单deque中间搜索：破坏Chase-Lev形状并产生并发删除问题。
- 高Priority全部强制Global：持续紧急fork/join失去locality并增加contention。
- 每Task一条priority queue：内存与管理不可接受。
- mutable band迁移：与D-129不可变策略冲突。

### Consequences

- 每Worker维护四套Local deque metadata；空Worker的固定开销增加。
- Queue metrics和Trace必须带Priority band标签。
- Benchmarks需比较单band基线与四band空扫描/内存成本。

### Non-goals and deferred risks

- 不要求四个band连续分配或共享buffer。
- 不定义NUMA/affinity下的进一步分区。
- deadline-bearing first-start work的heap结构另由D-133规定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受Global与每Worker Local各自四band，保留既有source/endpoint契约。
- Code or data evidence: D-091/D-092固定source fairness与deque端点；D-097至D-103固定Chase-Lev实现约束。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0036](../../docs/adr/0036-priority-uses-four-weighted-queue-bands.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-131 — Priority 用 8:4:2:1 加权公平选择且永不抢占 Running Task

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

严格最高优先级优先会在持续Critical流量下永久饿死Low/Normal；完全round-robin又让Critical只获得与Low相同吞吐。AstraScheduler的Callable不可抢占，因此Priority只能决定下一次Ready claim，不能改变正在运行的segment。

### Decision

每个Worker对Local、Global和steal-source selection维护等价的work-conserving weighted service calendar，默认权重为`Critical:High:Normal:Low = 8:4:2:1`。在一个source持续同时有四个band Eligible且没有并发claim干扰时，每15次成功claim至少分别提供8、4、2、1次band service opportunity；实现可用平滑加权轮转或等价deficit算法，必须避免把同一band的配额全部长时间聚簇。目标band为空时立即扫描其他非空band并成功工作，成功claim才推进相应calendar accounting。

Priority selection发生在D-130当前被允许探测的source内部，不推翻D-091 local/global outer policy。Priority只影响尚未claim的Ready Task；Running Callable或coroutine resume segment绝不被强制抢占、挂起、迁移或降级。`astra::yield`/自然await返回scheduler后，下一次resume才重新参与原base Priority band选择。

### Invariants

- Low权重必须大于0；持续Eligible不能因更高band持续有work而永久失去service opportunity。
- work-conserving fallback不得为了等待未来高Priority work让CPU idle。
- 权重是相对service share而非wall-clock latency或completion比例。
- 一个长Running Low Task可延迟Critical Task；文档必须明确非抢占边界。
- source fairness和priority fairness分别计量，不能用一个counter冒充另一个保证。
- 默认权重是稳定行为；本轮不公开任意用户权重配置，避免配置产生0权重饥饿。

### Scope and variants

| Situation | Behavior |
|---|---|
| all bands nonempty | weighted 8:4:2:1 service |
| selected band empty | immediate work-conserving fallback |
| local burst reaches 64 | mandatory Global probe before more Local |
| Task already Running | no preemption |
| coroutine suspends then resumes | re-enters original Priority band |

### Rationale

指数权重给紧急work明显吞吐优势，同时给每一级正向、可测试的service floor。把source fairness置于外层保留Work-Stealing locality和External admission边界；非抢占声明避免把调度hint包装成实时承诺。

### Rejected alternatives

- strict priority：低band可永久starve。
- equal round-robin：Critical区分度太弱。
- aging后永久提升base Priority：改变Task identity policy且需要可见boost状态。
- OS thread priority映射：影响整个Worker上运行的混合任务，无法按Task隔离。
- 每次随机抽band：难以确定性测试service bound。

### Consequences

- WorkerContext增加少量per-source weighted calendar状态。
- Trace可记录selected band和fallback reason；Metrics按band统计service/queue wait。
- 高Priority latency仍受当前Running segment、local burst和OS调度影响。

### Non-goals and deferred risks

- 不提供hard maximum wait或deadline guarantee。
- 不实现priority donation/inheritance beyond admission-time Internal default。
- 不把priority映射到native thread scheduling class。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受8:4:2:1 work-conserving加权公平、source policy外层以及明确非抢占。
- Code or data evidence: D-091声明Priority不能取消Global opportunity；D-129/D-130定义Priority和bands。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0036](../../docs/adr/0036-priority-uses-four-weighted-queue-bands.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-132 — Task Deadline 是显式 steady-clock 首次开始目标

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

“Deadline”若不说明对象，可能表示开始、完成或取消时刻。非抢占Runtime既不知道Callable成本也不能保证完成时间；相对duration若到admission才换算，还会因TaskOptions在调用前保存多久而产生歧义。Timer Wake Time则是最早Ready时刻，不能复用同一类型。

### Decision

定义强类型`TaskDeadline`，只包装`std::chrono::steady_clock::time_point`，通过`TaskDeadline::at(time_point)`或`TaskDeadline::after(duration)`构造；`after`在factory调用时立即读取steady clock并以checked/saturating加法固定绝对值，不在后续admission时重新计时。最终`TaskOptions`为包含`Priority priority{Priority::Normal}`与`std::optional<TaskDeadline> deadline{}`的值类型。

Task Deadline表示Task“希望不晚于该绝对时刻首次成功从非Running状态线性化为Running”。start arbitration成功后、调用用户Callable或resume frame前采样`start_time`：`start_time <= deadline`为met，`start_time > deadline`为missed。它不是completion deadline、Timer Wake Time或自动取消时刻。无options/无deadline的Task、Internal child和Graph Node都不隐式继承调用方Deadline；只有显式TaskOptions携带Deadline。

### Invariants

- Deadline只使用steady clock；public API不接受system clock或裸duration字段。
- `after(duration)`的计时起点是factory调用，不是submit/admission；文档与tests必须覆盖options延迟使用。
- 负duration可形成过去Deadline并正常admit，不能作为参数错误或隐式rejection。
- met/missed判定在首次start恰好一次，Coroutine后续resume不重新判定。
- Waiting Graph Node可因dependency迟到而miss；deadline不使其在依赖满足前Eligible。
- Deadline不沿Internal submit、Handle await、Graph edge或Coroutine child隐式传播。
- Task Deadline与Timer Wake Time使用不同类型、字段和Trace名称。

### Scope and variants

| Time relation at first start | Disposition |
|---|---|
| no deadline | not applicable |
| start <= deadline | met |
| start > deadline | missed |
| cancelled before first start | did not start; neither met nor missed |
| coroutine later resume | no new deadline evaluation |

### Rationale

首次开始是Scheduler实际能直接影响且可精确观测的边界。强类型和绝对steady time消除“等待到Deadline”与“应在Deadline前开始”的概念混用，也避免墙钟跳变。

### Rejected alternatives

- completion deadline：需运行成本估计、preemption或到时强杀。
- deadline即取消时刻：混淆scheduling hint与cooperative cancellation。
- `system_clock::time_point`：墙钟调整会改变排序与miss判断。
- admission时解析relative duration：TaskOptions保存时间会改变含义。
- Internal/Graph自动继承deadline：隐藏绝对约束传播并可能让大量child全部过期。

### Consequences

- TCB在首次start前保存optional absolute Deadline，并记录一次内部disposition用于Metrics/Trace。
- 想表达完成SLA的应用必须在外部观察结果并显式取消/升级，不能把Task Deadline当完成保证。
- API examples应把`TaskDeadline::after(10ms)`就近构造在submit调用处。

### Non-goals and deferred risks

- 不公开per-Task DeadlineStatus查询API；稳定观测面由Metrics/Trace决策定义。
- 不估算WCET或自动计算slack。
- 不支持修改/延长已经admit的Deadline。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受steady-clock强类型TaskDeadline作为显式首次开始目标，不继承、不自动取消。
- Code or data evidence: D-126区分Timer Wake Time；D-129建立TaskOptions与不可变Priority；Runtime为非抢占模型。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0037](../../docs/adr/0037-deadline-is-best-effort-first-start-edf.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-133 — 首次开始的 Deadline Task 进入 Runtime-wide indexed EDF heaps

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Chase-Lev Deque只能从两端O(1)访问，不能在保持算法证明的同时按任意绝对Deadline选择最早项。把Deadline Task仍放Local band会让同一Priority下的新LIFO Task压过更早Deadline；把所有Ready Task改成central heap又会摧毁无Deadline工作负载的locality。

### Decision

每个Runtime State在Global Source内按四个Priority band持有四个mutex保护的indexed EDF min-heap，仅存放“带Deadline且从未Running”的Ready Task，排序键为`(TaskDeadline, AdmissionSequence)`。External、same-Runtime Internal及Graph Node只要带Deadline，在首次Ready publication时都进入对应Global EDF heap而不进入Local Chase-Lev；Waiting Graph Node直到dependency release才入heap。Deadline Task可以在Deadline之前立即被选择，Deadline不是Wake Time。

成功claim并完成首次Running arbitration后，Task永久退出EDF结构；若Coroutine此后Suspended，其resume按原base Priority进入普通Global band。start前取消、Immediate清理或admission rollback使用stable scheduling entry id执行O(log n) indexed erase，不保留长期tombstone。Heap lock内只操作调度metadata，不调用用户代码、发布Outcome或resume frame。

### Invariants

- EDF只比较同一Priority band内的Deadline Task；跨Priority由D-131/D-134决定。
- 相同Deadline以不可复用的AdmissionSequence形成确定全序；sequence耗尽必须确定性fail-fast或拒绝，不能wrap后破坏排序。
- Task恰好持有一个Scheduling Reference：EDF entry、普通queue cell、claim owner或terminal cleanup之一。
- Internal Deadline Task强制Global routing但仍保留Internal capacity豁免与Drain Work Closure身份。
- Heap entry移除与claim/cancel必须线性化，最多一个路径取得start responsibility。
- Deadline miss本身不重新入队或改变heap order；判定发生在pop/claim之后的首次start边界。

### Scope and variants

| Task phase/type | Scheduling structure |
|---|---|
| Ready, deadline, never started | Global EDF heap for base Priority |
| Waiting DAG deadline node | no queue until dependency release |
| Ready, no deadline | normal Global/Local Priority band |
| coroutine resume after first start | normal Global Priority band |
| cancelled before start | indexed erase and terminal arbitration |

### Rationale

把EDF限制为首次开始且显式带Deadline的任务，使deadline排序正确而不侵入Chase-Lev证明。Runtime-wide heap牺牲这部分任务的locality，换取跨Worker统一的同band earliest-deadline选择。

### Rejected alternatives

- Local Chase-Lev中扫描最早Deadline：并发中间删除破坏算法与复杂度。
- 每Worker本地Deadline heap：跨Worker没有统一EDF且steal需多heap比较。
- 所有Task进入central priority heap：无Deadline主路径失去Work-Stealing locality。
- deadline到时才入Ready：误把Deadline当Timer Wake Time。
- lazy tombstone：大量start前取消会积累长期metadata。

### Consequences

- Deadline-heavy workload集中竞争四个heap mutex；Benchmark必须量化。
- Internal deadline work会增加Global traffic并使D-091的Global service机制生效。
- Future scalable EDF可替换内部结构，但必须保持同band ordering、tie break和single-reference规则。

### Non-goals and deferred risks

- 不实现distributed/multi-Runtime全局EDF。
- 不把already-running/suspended continuation重新放回EDF。
- 不为Deadline Task提供cache affinity承诺。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受按Priority分区的Runtime-wide indexed EDF heap，仅调度从未开始的Deadline Task。
- Code or data evidence: D-092/D-097固定deque端点与Chase-Lev形状；D-129至D-131固定Priority bands/fairness；D-132固定first-start语义。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0037](../../docs/adr/0037-deadline-is-best-effort-first-start-edf.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-134 — Priority 主导、同 band deadline-preferred EDF 且 miss 仅被观测

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

需要固定Priority与Deadline谁先比较，以及连续Deadline流量是否能饿死普通FIFO work。若miss后自动提升、取消或失败，会改变用户Outcome并制造新的竞态；若strict EDF覆盖Priority，Low Deadline可以完全绕过Critical业务分类。

### Decision

D-131的Priority weighted calendar先选择band，Deadline绝不跨band提升Task；在一次Global band service内，EDF heap优先于该band普通FIFO，但每个Worker对该band最多连续成功claim默认`deadline_burst_limit = 8`个Deadline Task，随后若普通FIFO非空必须给它一次service opportunity，再开始新deadline burst。FIFO为空时继续EDF；EDF为空时立即FIFO，整个选择保持work-conserving。该固定默认不在首个稳定API中公开配置。

Deadline miss只记录事实：Task继续Running，base Priority、Terminal Outcome和取消状态均不改变。miss不会自动boost、cancel、throw、reject、skip或让Graph successor失败。已过期Task仍按正常lifecycle/capacity规则admit并参与EDF；Graceful/Immediate语义不因Deadline改变。Metrics/Trace必须分别记录deadline admission、met、missed、cancelled-before-start以及非负start lateness；项目只宣称best-effort deadline-aware scheduling。

### Invariants

- Priority是primary policy，EDF是same-band ordering；文档不得声称全Runtime strict EDF。
- 连续EDF流量不能永久饿死同Global band普通FIFO。
- deadline burst是成功claim计数；空pop、lost claim或cancel cleanup不计。
- D-091 Local source可在Global probe前继续其有限burst，因此Deadline没有wall-clock dispatch bound。
- 一个Running Low/no-deadline segment可延迟Critical Deadline Task；Runtime不抢占。
- missed Task的Value/Exception/Cancelled Outcome仍完全由Callable/cancellation决定。
- cancelled-before-start单独计数，不伪造met或missed。

### Scope and variants

| Interaction | Rule |
|---|---|
| Critical no-deadline vs Low expired deadline | Priority calendar remains primary |
| same-band EDF and FIFO both nonempty | up to 8 EDF, then 1 FIFO opportunity |
| deadline already past at admission | accept normally; likely miss at start |
| miss at first start | record and run |
| shutdown | unchanged Graceful/Immediate semantics |

### Rationale

Priority先行保留显式业务等级，same-band EDF为同等级紧迫任务提供合理顺序；deadline burst则保护普通Global work。只观测miss是非抢占best-effort Scheduler最诚实、组合性最好的行为。

### Rejected alternatives

- strict EDF across allPriority：Priority失去含义。
- Priority严格高到低再EDF：Low仍可永久starve，违反D-131。
- miss自动cancel/fail：改变Outcome且可能在Callable开始边界竞态。
- missed自动Critical boost：过期洪泛可压垮所有正常工作。
- Deadline永远压过同band FIFO：普通work可永久starve。
- 提供硬实时宣传：无法约束Callable、OS或硬件调度。

### Consequences

- Worker Global selector增加per-band deadline burst counters。
- Deadline性能评估需同时报告miss ratio、lateness、FIFO fairness和heap contention。
- 应用若要求miss即取消，必须使用独立timer/stop policy在上层显式实现并接受cooperative边界。

### Non-goals and deferred risks

- 不承诺最大lateness、可调度性分析或admission feasibility test。
- 不实现Earliest Deadline First抢占或CPU reservation。
- 不提供per-task miss callback；Observability接口后续固定。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受Priority primary、same-band deadline-preferred EDF、8:1普通work公平以及miss仅观测。
- Code or data evidence: D-091/D-131提供source和priority service boundaries；D-132/D-133定义Deadline与EDF结构。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0037](../../docs/adr/0037-deadline-is-best-effort-first-start-edf.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-135 — Metrics 提供 Off、Basic、Detailed 三级且默认 Basic

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

完全关闭统计有利于测量最小Scheduler overhead，但默认关闭会让生产问题没有基础事实；所有延迟都默认采样则会给每个状态转换增加clock read、bucket更新和cache contention。Metrics不能成为调度正确性依赖，否则Off模式会改变行为。

### Decision

公开`enum class MetricsLevel { Off, Basic, Detailed };`，`SchedulerOptions::metrics_level`默认`Basic`。Basic记录D-136的生命周期、准入、调度、取消、timer/deadline、graph/coroutine基础counter与当前gauge；Detailed在Basic之上记录固定schema的per-Priority/per-Worker分解和D-137的延迟直方图。Off跳过所有非正确性观测更新，但Scheduler行为、同步、TaskId/AdmissionSequence、Trace和错误处理保持相同。

`Scheduler::metrics_snapshot()`返回拥有自身存储的不可变`RuntimeMetricsSnapshot`值；它可从任何线程调用，不等待Task完成、不执行Helping、不重置counter。Snapshot包含`schema_version`、RuntimeId、capture steady time、MetricsLevel、Worker count、当前SchedulerState/ShutdownMode和`saturated`标志。Off时返回带identity/config/state但measurement区标记disabled的合法snapshot，而不是伪造启用的零流量。

### Invariants

- Metrics更新不能作为queue publication、Task completion、park或shutdown的happens-before来源。
- MetricsLevel在Scheduler构造时固定；稳定API不支持运行时切换导致部分区间口径混合。
- snapshot不调用用户代码、不写文件、不停止Worker。
- Detailed关闭时histogram/per-worker数据明确absent，不用全零冒充已采集。
- 未知MetricsLevel或非法Detailed配置在Worker启动前`std::invalid_argument`拒绝。
- Metrics allocation失败不能破坏已admit Task执行；snapshot自身分配失败可按普通C++异常报告。

### Scope and variants

| Level | Counters/gauges | Histograms | Per-worker breakdown |
|---|---|---|---|
| Off | disabled | disabled | disabled |
| Basic | enabled | disabled | absent |
| Detailed | enabled | enabled | enabled |

### Rationale

Basic默认提供运维最低可见性，Detailed显式支付clock和空间成本，Off允许基准建立下界。构造时固定level使一个snapshot口径稳定并保持实现简单。

### Rejected alternatives

- Metrics总是全部开启：微任务和Coroutine segment会被观测成本主导。
- 默认Off：故障发生后无法回溯基本运行量。
- 运行中任意切换：counter区间与histogram分母难解释。
- snapshot顺便reset：并发读者互相干扰且可能丢累计事实。
- Metrics控制调度：Off/Basic会产生功能差异。

### Consequences

- Runtime State保留固定level及相应shards到join完成。
- Benchmark必须明确记录MetricsLevel，并至少报告Off与Basic overhead差异。
- 用户需要区间值时在外部对两个累计snapshot做checked delta。

### Non-goals and deferred risks

- 不首发运行时动态开关或Prometheus exporter。
- 不提供跨进程聚合。
- 不保证snapshot无分配或async-signal-safe。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受Off/Basic/Detailed三级、默认Basic和只读非重置snapshot。
- Code or data evidence: 总设计已要求Runtime Metrics但未固定开销/快照语义；前述Task/Runtime状态提供事件边界。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0038](../../docs/adr/0038-metrics-are-sharded-saturating-fuzzy-snapshots.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-136 — Basic Metrics 使用稳定事件口径、分片饱和 counter 与独立 gauge

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

仅列出`submitted/completed/pending`不足以判断准入拒绝、取消发生阶段、steal、timer或deadline行为；但使用TaskId、GraphId等高基数label会让内存不可控。并发counter若自然wrap会把长期Runtime的累计事实倒退。

### Decision

Basic schema使用无动态label的固定`std::uint64_t`字段族：

- admission：`submission_attempts`、`accepted_task_identities`、`rejected_lifecycle`、`rejected_capacity`、`blocking_submit_waits`、`blocking_submit_wakeups`；
- execution/outcome：`first_starts`、`resume_segments`、`succeeded`、`failed`、`cancelled_before_start`、`cancelled_cooperative`、`unobserved_failures`；
- scheduling：`global_claims`、`local_claims`、`steal_attempts`、`steal_successes`、`steal_failures`、`worker_parks`、`worker_wakes`、`explicit_yields`；
- coroutine/timer：`coroutine_suspends`、`timer_registrations`、`timer_fires`、`timer_cancellations`；
- graph：`graph_admission_attempts`、`graph_runs_accepted`、`graph_runs_rejected`、`graph_nodes_terminal`；
- deadline：`deadline_admitted`、`deadline_met`、`deadline_missed`、`deadline_cancelled_before_start`。

当前gauge至少为`waiting_tasks`、`ready_tasks`、`running_tasks`、`suspended_tasks`、`external_pending_slots_used`、`parked_workers`、`active_timer_entries`和`active_graph_runs`。Graph Node是accepted Task Identity并进入task state/outcome counters；GraphRun attempt另计一次graph counter，不能把整个Graph当一个Task重复计数。Coroutine同一Identity只计一次first start/outcome，每次后续resume计segment。

高频counter按Worker shard并以relaxed atomic或等价无数据竞争方式更新；external/control事件使用Runtime shards。聚合和加法采用checked saturating arithmetic，任何字段达到`UINT64_MAX`后保持饱和并设置snapshot`saturated=true`，绝不wrap。Gauge在每个真实状态转移边界成对更新且不得下溢；Metric bug不得被clamp掩盖，debug/test build fail-fast。

### Invariants

- fixed schema不包含TaskId、NodeId、用户名称或任意字符串label。
- accepted_task_identities只在admission commit后增加；rejection不增加。
- terminal outcome恰好增加succeeded/failed/cancelled类别之一；取消类别按是否曾Running分开。
- steal_attempts = successes + failures只在相同snapshot截面可能暂时不等，最终静止Runtime必须相等。
- Deadline met+missed+cancelled-before-start最终覆盖带Deadline accepted Task，但运行中snapshot允许pending差额。
- Gauge是当前投影，counter是累计事实；两者不能用同名字段混淆。
- MetricsLevel Off时不维护这些measurement，不能读取未初始化shard。

### Scope and variants

| Entity | Counting identity |
|---|---|
| ordinary Callable Task | one accepted identity |
| spawned Coroutine Task | one identity across all resumes |
| Graph Node | one accepted identity per node |
| GraphRun | separate run admission/control counters |
| Handle copies | never counted as tasks |
| timer registration | event counter, not new task |

### Rationale

固定低基数schema覆盖调度器的主要故障面，并允许无外部metrics库实现。分片降低热原子竞争，饱和保持长期累计单调；明确Identity避免Coroutine/Graph重复核算。

### Rejected alternatives

- 只保留submitted/completed：无法区分拒绝、状态和调度路径。
- 每Task/名称label：无界cardinality。
- 自然unsigned wrap：累计counter倒退且delta失真。
- 每次snapshot停止所有Worker：观测侵入调度与shutdown。
- GraphRun只计一个Task：掩盖per-node capacity/outstanding事实。

### Consequences

- schema字段名和口径一旦进入稳定版即需版本化兼容。
- Tests需在quiescent point验证counter conservation，在并发点只验证安全范围/单调性。
- Per-Priority细分只在Detailed提供，Basic字段保持总量。

### Non-goals and deferred risks

- 不提供用户定义counter或tag。
- 不保证跨Runtime counter可直接相加而无identity重叠问题。
- 不首发百分位预计算。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受固定低基数事件schema、Task Identity核算、分片饱和counter与独立gauges。
- Code or data evidence: D-047/D-079定义Task终态；D-084容量；D-104 GraphRun；D-114 Coroutine Identity；D-132 Deadline。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0038](../../docs/adr/0038-metrics-are-sharded-saturating-fuzzy-snapshots.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-137 — Metrics Snapshot 是有界扰动的 fuzzy snapshot，Detailed 使用固定 log2 直方图

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

要得到所有counter/gauge同一瞬间的事务快照，需要暂停Worker或把所有更新串行化，违背Metrics低侵入目标。直接保存所有样本或HDR库会增加无界内存/依赖；只给平均值则掩盖tail latency。

### Decision

`metrics_snapshot()`逐shard以acquire/relaxed-safe读取并聚合，因此是fuzzy snapshot：每个字段来自capture调用区间内的某个合法时刻，但字段之间不承诺单一全局线性化点，也不承诺并发时满足守恒等式。Snapshot记录`capture_started_at`与`capture_finished_at`，调用不锁住调度队列或暂停Worker；在Runtime quiescent且无并发snapshot mutation时，最终snapshot必须满足D-136守恒关系。

Detailed latency使用固定64个base-2纳秒bucket（bucket 0包含0～1ns，后续按`[2^(n-1), 2^n)`，最后bucket吸收溢出）和`count/sum_ns/max_ns`饱和字段，不存原始样本、不在Runtime内计算percentile。至少记录：`ready_queue_wait`（每次Ready publication到对应start/resume）、`execution_segment`（进入用户Callable/resume到返回scheduler）、`task_wall_time`（admission commit到Terminal Outcome）、`blocking_admission_wait`、`timer_wake_lateness`（Wake Time到timer被处理，clamp at zero）、`deadline_start_lateness`（仅miss，Deadline到first start）、`worker_park_duration`与`runtime_join_latency`。外部工具从bucket估算percentile并保留schema_version。

### Invariants

- fuzzy不等于data race；每个字段读取和聚合必须有定义行为且单调counter不会倒退。
- snapshot期间不获取queue/deque/timer/graph执行锁形成跨模块锁顺序。
- latency使用steady clock且负测量视为clock/instrumentation bug，不能静默进入巨大unsigned bucket。
- Coroutine每个segment分别进入ready_queue_wait/execution_segment，task_wall_time只记录一次。
- timer_wake_lateness测timer driver处理延迟，不等同continuation start queue wait；两者分别记录。
- sum/max/bucket同样saturating并传播`saturated`。

### Scope and variants

| Requirement | Guarantee |
|---|---|
| per-field memory safety | yes |
| global transactional instant | no |
| quiescent conservation | yes |
| raw samples retained | no |
| percentile in Runtime | no |
| bounded histogram memory | yes |

### Rationale

fuzzy snapshot是并发Runtime中成本与可解释性的合理边界。固定log2 histogram不引入第三方依赖、空间恒定，并保留比平均值充分得多的tail信息。

### Rejected alternatives

- stop-the-world snapshot：改变被观测延迟和shutdown行为。
- 单global metrics mutex：高频状态转移串行化。
- 只保存average：tail regression不可见。
- 保存每个sample：内存无界且导出昂贵。
- Runtime内直接给p99 double：聚合不可组合且精度/算法不透明。

### Consequences

- 用户不能对并发snapshot强断言`accepted == terminal + gauges`；需要在quiescent gate验证。
- 工具库需提供histogram merge与percentile估算，但不进入调度热路径。
- Benchmark输出应保存原bucket或稳定序列化格式，而不只保存单个p50/p99。

### Non-goals and deferred risks

- 不保证跨机器时钟可比较。
- 不提供OpenTelemetry/Prometheus exporter首发。
- 不规定GUI展示格式。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受非事务fuzzy snapshot和固定log2纳秒直方图，quiescent时才要求守恒。
- Code or data evidence: D-135/D-136定义Metrics level/schema；总设计要求latency与worker/reaper指标。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0038](../../docs/adr/0038-metrics-are-sharded-saturating-fuzzy-snapshots.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-138 — TraceCollector 是显式共享、可重复capture且有界的内存记录器

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

在每个Task事件上同步格式化JSON或写文件会把I/O、锁和分配放进调度热路径；每Runtime再增加Trace writer thread会扩大lifecycle。只给Scheduler一个内部ring又难以把多个Runtime和Reaper放到同一时间轴，也不便控制测量区间。

### Decision

公开线程安全共享capability `TraceCollector`。用户显式创建Collector并通过`SchedulerOptions::trace_collector`以`std::shared_ptr`附加到一个或多个Runtime；默认为空，禁用路径只执行可预测的null/disabled fast branch。Collector初始Stopped，可重复执行单一活动capture周期：`start_capture()`清空上一代buffer并返回move-only `TraceCapture`；同一Collector并发第二次start确定性失败。`TraceCapture::stop()`线性化禁止新事件、等待当前固定大小emit临界区退出，然后返回不可变`TraceSnapshot`；stop幂等共享同一结果，不等待Task、Worker join或Runtime completion。

每次capture按`TraceOptions`预分配固定容量：每Worker使用single-producer ring，external/control producers使用有界MPMC buffer，Reaper coordinator向附着Collector注册独立producer。emit不执行heap allocation、文件I/O、用户callback或阻塞等待buffer；buffer满时drop-newest并以饱和counter记录per-producer/per-kind loss。一个Collector共享给多个Runtime时使用同一steady-clock origin；Runtime State持有Collector注册直到join，Reaper可把handoff/join/finalization事件写入相关Collector。

### Invariants

- Collector不拥有Scheduler/Runtime lifecycle；shared reference只保持collector state，不阻止Runtime shutdown。
- capture generation切换必须防止上一代late emitter写入新代buffer。
- stop只等待已进入的bounded emit critical section，不等待用户Callable返回。
- buffer overflow不能阻塞Worker或覆盖尚未完成写入的slot。
- Trace disabled/overflow不能改变Task选择、Outcome、wake或shutdown。
- Collector attached后可在Stopped状态低成本存在；同一Runtime不动态替换collector pointer。
- TraceCapture析构若尚未stop则执行安全disable并丢弃/保留可取结果的具体RAII路径必须noexcept，不得抛出。

### Scope and variants

| State | Event behavior |
|---|---|
| no collector | fast no-op |
| collector Stopped | fast disabled no-op |
| capture Recording, capacity available | append fixed event |
| capacity full | drop-newest and count loss |
| capture stop in progress | generation/active-emitter handshake |

### Rationale

显式共享Collector允许用户选择观测范围并跨Runtime合并，同时把格式化和I/O移出Runtime。可重复capture避免为每次实验重建Scheduler，有界非阻塞buffer保护Scheduler活性。

### Rejected alternatives

- 每事件同步JSON/log：严重污染性能并可能重入I/O。
- per-Runtime Trace thread：增加线程、shutdown和Reaper复杂度。
- 无界vector：长进程内存不可控且reallocation在热路径。
- overwrite-oldest MPMC ring：snapshot与并发覆盖协议复杂且会丢失capture开头因果链。
- buffer满时阻塞：观测反向改变调度和deadlock风险。
- 全局隐式singleton recorder：多库/测试之间控制权不清。

### Consequences

- Trace用户必须预估容量并检查loss metadata。
- shared Collector可把多个Scheduler和Reaper事件放进同一capture，但Runtime未附加时不会被偷偷追踪。
- Benchmark默认不附加Collector；另设Trace overhead与loss capacity实验。

### Non-goals and deferred risks

- 不首发streaming network sink或后台持续落盘。
- 不保证stop为严格wall-clock有界；被OS挂起的emitter可延迟quiescence。
- 不允许一个Collector同时存在多个独立capture timeline。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受显式共享、可重复capture、固定容量非阻塞TraceCollector，不新增writer thread。
- Code or data evidence: D-007/D-015定义Runtime/Reaper lifetime；D-135要求观测不影响正确性；总设计要求Chrome Trace。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0039](../../docs/adr/0039-trace-capture-is-bounded-and-exported-offline.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-139 — Trace 使用版本化固定事件 schema 与稳定逻辑 Identity

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Trace若记录raw pointer、线程本地字符串或实现类名，会在不同运行中不可比较并可能泄漏地址；若Metrics、Trace和状态机使用不同事件含义，分析结果无法互证。Coroutine一个Task多个resume segment、Graph一个Run多个Node也要求明确identity层级。

### Decision

内部`TraceEvent`是trivially-copyable固定大小记录，至少包含`schema_version`、capture-relative steady timestamp_ns、EventKind、ProducerId/local sequence、RuntimeId、WorkerId、TaskId以及可选GraphRunId/NodeId，并用紧凑枚举字段记录Priority、source、TaskState/Outcome、reason和deadline disposition；缺失identity使用显式invalid sentinel，不存raw pointer或任意用户字符串。

稳定EventKind族至少覆盖：admission/rejection；Task Ready/claim/first-start/segment-end/Terminal/cancel-request；Local/Global/steal source与steal success；Worker park/wake；Coroutine suspend/resume/yield；Timer register/fire/cancel；GraphRun accepted/terminal与Node dependency release；Deadline met/miss；Runtime handoff/join-ready/joined；Finalization begin/escalate/coordinator-exit/complete。Task segment begin/end复用同一TaskId并携带SegmentSequence；Graph coroutine node同时携带同一TaskId和NodeId，不创建第二identity。

TraceOptions以固定category bitmask控制事件族，默认启用Task lifecycle、scheduling、Coroutine/Graph和Runtime lifecycle，允许关闭极高频steal-attempt等类别；category关闭不计为dropped。每个producer timestamp必须单调不降，local sequence严格递增；跨producer只按`(timestamp_ns, ProducerId, local_sequence)`形成导出确定全序，不声称原事件存在全局线性化顺序。

### Invariants

- Trace事件定义必须引用同一state/admission/start/terminal线性化边界，不能在方便位置近似命名。
- TaskId/RuntimeId/GraphRunId不因对象地址复用；耗尽策略不能wrap造成同capture identity冲突。
- EventKind与字段枚举有显式数值/schema version；新增可向后兼容，重解释旧值必须升级major schema。
- fixed record不持有指针、string_view或需要析构的对象。
- Task Outcome Value内容、异常文本和用户数据默认不进入Trace。
- source-local timestamp回退视为instrumentation error并计diagnostic/drop，不静默破坏顺序。
- Metrics与Trace允许因采样/loss数量不同，但事件口径名称必须一致。

### Scope and variants

| Entity | Stable trace identity |
|---|---|
| Runtime | RuntimeId |
| Worker lifetime | RuntimeId + WorkerId |
| ordinary/coroutine task | TaskId |
| coroutine segment | TaskId + SegmentSequence |
| GraphRun | GraphRunId |
| Graph Node task | GraphRunId + NodeId + TaskId |
| producer buffer | ProducerId + local_sequence |

### Rationale

固定binary event把热路径压缩为clock read和bounded append；逻辑ID支持跨线程flow，不泄漏地址。per-producer order加确定merge足以生成可重现文件，又不引入昂贵global event sequence原子。

### Rejected alternatives

- raw coroutine_handle/TCB地址：地址复用、ASLR和泄漏问题。
- hot-path动态JSON/map：分配与格式化不可控。
- 一个global sequence atomic：所有Worker每事件争用同一cache line。
- 只记录Task start/end：无法解释排队、steal、suspend和shutdown。
- Trace保存异常what/value：敏感数据、lifetime和分配问题。

### Consequences

- 需要维护独立trace schema文档和兼容性测试fixture。
- Chrome exporter可从segment与flow identity重建Worker timeline。
- 用户任务命名若未来需要，应通过有界intern table和显式隐私策略另定。

### Non-goals and deferred risks

- 不承诺不同进程/机器之间timestamp同步。
- 不首发任意用户payload或stack trace。
- 不把Trace当审计日志或可靠event bus。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受固定versioned event、逻辑ID、per-producer ordering与低基数payload。
- Code or data evidence: D-047/D-079/D-092/D-116等已固定可复用事件边界；D-136固定Metrics口径。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0039](../../docs/adr/0039-trace-capture-is-bounded-and-exported-offline.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-140 — Chrome Trace 在 capture 停止后离线确定性导出并显式报告损失

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Chrome Trace JSON要求字符串转义、时间单位换算和事件phase映射，不适合Worker hot path。丢事件的bounded trace如果仍生成看似完整的timeline会误导分析；日志与Trace若共用同步sink也会让高频诊断污染Benchmark。

### Decision

只有Stopped `TraceSnapshot`可通过独立`write_chrome_trace(snapshot, std::ostream&)`或等价工具离线导出Chrome Trace Event JSON；core不接受文件路径、不在Runtime线程打开/flush文件。Exporter按`(timestamp_ns, ProducerId, local_sequence)`稳定merge，把capture origin映射为`ts`微秒double/整数精度策略，Runtime映射process lane、Worker/producer映射thread lane，Task execution segment使用duration event，enqueue→claim/suspend→resume使用flow/instant event，metadata携带schema、options和identity。

Snapshot保存每producer/event-kind的recorded与dropped饱和计数、capacity及capture时间范围。任意drop仍必须输出语法有效JSON，但顶层metadata明确`trace_complete=false`及loss totals；依赖完整因果关系的分析/测试只有在所有dropped为0时才可通过。相同snapshot与exporter版本必须产生byte-for-byte确定输出（除用户选择的pretty-print模式）。Exporter校验enum/identity/segment nesting，损坏snapshot返回明确错误而不生成伪造闭合事件。

Logging与Trace保持独立：日志只用于低频ERROR/WARN/INFO控制面诊断，不逐Task同步记录；Trace事件不调用logger。Benchmark除专门trace-overhead case外禁用Trace与高频日志，并把capture options/loss写入结果metadata。

### Invariants

- exporter失败不影响Runtime或原TraceSnapshot，可重试到其他ostream。
- nanosecond到Chrome单位换算执行checked arithmetic，不因长capture溢出。
- drop不能通过合成TaskStarted/Completed事件“修复”timeline。
- flow id从稳定logical identity派生且在文件内无歧义。
- JSON字符串即使主要来自固定schema也必须正确escape。
- Trace/Log sink不得在Worker执行路径共享锁。
- `trace_complete=true`只表示collector未报告drop/schema corruption，不表示应用没有未追踪Runtime或关闭category。

### Scope and variants

| Condition | Export behavior |
|---|---|
| zero loss | complete metadata, deterministic timeline |
| dropped events | valid JSON + incomplete/loss metadata |
| active capture | snapshot/export rejected until stop |
| corrupt/unknown required schema | explicit export error |
| disabled category | declared in options, not counted as loss |

### Rationale

离线导出把不可控I/O成本移出Scheduler，并允许严格验证。显式loss防止把bounded diagnostic trace误用成可靠审计记录；日志分离保留低频运维信息而不污染任务timeline。

### Rejected alternatives

- Worker直接写Chrome JSON：同步I/O与JSON锁污染调度。
- drop时静默输出：分析可能得到错误因果结论。
- 自动补齐缺失begin/end：伪造事实。
- core API接受path并管理文件：扩大filesystem/error/lifecycle责任。
- 每Task同时写日志：重复开销和不可控输出。

### Consequences

- tools层需要Chrome exporter、schema validator和golden files。
- 容量不足时用户需缩短capture、关闭category或增加buffer后重跑。
- Trace Viewer可以拒绝完整性要求较高的分析，同时仍展示带警告的部分timeline。

### Non-goals and deferred risks

- 不首发Perfetto protobuf、OTLP或实时viewer transport。
- 不保证Chrome UI能展示所有未来自定义category。
- 不把trace loss视为Scheduler功能失败。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受stop后离线确定性Chrome JSON、显式loss完整性和Trace/Log分离。
- Code or data evidence: D-138/D-139定义collector/event；总设计要求Chrome Trace并警告同步日志污染。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0039](../../docs/adr/0039-trace-capture-is-bounded-and-exported-offline.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-141 — Benchmark 采用 Google Benchmark 微基准加独立场景 runner

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

队列操作/submit等微基准适合成熟iteration harness，但DAG、shutdown、priority混合负载与process-wide finalization需要多阶段协调、子进程隔离和自定义结果。自写全部计时框架容易重复造轮子，只用单一microbenchmark API又会扭曲Runtime场景。

### Decision

Benchmark是与core library隔离的开发组件，由默认OFF的CMake选项`ASTRA_BUILD_BENCHMARKS`启用。使用固定/pinned版本Google Benchmark承载单进程microbenchmarks（queue primitive、admission、TaskHandle outcome、Coroutine suspend/resume、timer/graph transition、Metrics/Trace instrumentation cost），并实现仓库内`astra_bench_scenarios` runner承载多阶段Runtime workloads、跨variant orchestration与JSON artifact。benchmark依赖不进入public headers、安装接口或普通consumer link graph。

每个case明确分离setup、warmup、timed region、drain/verification和teardown；是否把Task allocation、Callable construction、submission或shutdown纳入timed region必须是case metadata而非隐含选择。Timed region使用steady clock，开始由barrier协调，结束后必须验证expected task count/checksum/outcome和零未预期rejection/drop；验证失败则case invalid，不输出可比较性能结论。Finalization/Reaper等不可重启全局状态case必须每个样本运行在新子进程。

### Invariants

- benchmark target不改变core编译宏、memory ordering或queue implementation来制造不可比较fast path；variant差异必须显式命名。
- Debug/sanitizer build用于正确性，不与Release性能数字比较。
- 默认性能配置为Release、优化开启、assert策略和编译器flags完整记录。
- MetricsLevel、TraceCollector、logging状态进入case metadata；默认吞吐case为Metrics Off、Trace disabled，另有Basic/Detailed/Trace overhead cases。
- setup/verification不误计入timed region，除非case名称明确测试end-to-end。
- 子进程异常、timeout、checksum失败或trace/metric loss使样本invalid，不能按慢样本吞入统计。
- pinned dependency更新通过独立review，不随网络最新版本漂移。

### Scope and variants

| Work | Harness |
|---|---|
| primitive latency/throughput | Google Benchmark |
| multi-phase Runtime workload | astra_bench_scenarios |
| finalization/reaper lifecycle | isolated child process |
| sanitizer/stress correctness | test suite, not performance corpus |
| result aggregation | repository tooling over JSON artifacts |

### Rationale

成熟micro harness解决迭代与基础统计，自有scenario runner保留Runtime领域语义和隔离需求。严格verification防止“更快”只是因为漏任务、拒绝或提前退出。

### Rejected alternatives

- 全部手写benchmark loop：warmup、optimization barrier和reporting易出错。
- 所有场景塞进Google Benchmark iteration：process lifecycle与多阶段结果难表达。
- benchmark直接进core target：依赖与编译选项污染用户库。
- 不验证结果只计时：调度bug会表现为性能提升。
- finalization case同进程重复：D-035禁止重启Reaper，样本不独立。

### Consequences

- 仓库增加bench scenario protocol、child launcher和JSON schema。
- CI可构建benchmark但普通PR不一定运行完整性能矩阵。
- 开发文档必须给出可复制的configure/build/run命令和artifact位置。

### Non-goals and deferred risks

- 不把Google Benchmark API暴露给Astra用户。
- 不首发在线benchmark service或云数据库。
- 不把sanitizer结果当真实性能数字。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受Google Benchmark微基准、自有场景runner、严格timed-region/verification和finalization子进程隔离。
- Code or data evidence: D-035定义Reaper不可重启；D-135/D-138定义Metrics/Trace开销模式；总设计要求Benchmark Framework。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0040](../../docs/adr/0040-benchmarks-are-verified-reproducible-experiments.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-142 — Benchmark corpus 覆盖基线、机制成本与组合工作负载

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

只测空任务吞吐会奖励全局队列的简单实现却无法证明Work-Stealing对不均衡负载的价值；只测一个递归demo又无法定位Chase-Lev、Coroutine、DAG、Priority或观测开销。与外部库比较若语义和线程数不对齐也容易产生误导。

### Decision

维护三层固定corpus：

1. primitive/mechanism：Global FIFO uncontended/contended、locked Local与Chase-Lev owner/steal/last-item/grow、submit/try_submit与backpressure、Handle wait/get、cancel race、Coroutine create/resume/yield/sleep、DAG edge release/freeze、priority band scan、EDF heap、Metrics levels和Trace event emit；
2. scheduler workloads：empty/micro task、calibrated CPU-bound、producer-consumer external injection、recursive fork-join、highly imbalanced steal、recursive quicksort、DAG chain/fan-out/fan-in/random acyclic、Coroutine suspend/resume storm、timer storm/cancel storm、mixed Priority fairness、Deadline EDF/miss curve、Graceful/Immediate shutdown和orphan Reaper；
3. combination/end-to-end：Graph coroutine nodes with timers/cancellation、Internal submission under backpressure、Priority+Deadline+steal、Metrics/Trace enabled capture。

必须保留in-tree mutex-protected Global FIFO fixed-worker baseline和Phase 2 locked Work-Stealing semantic oracle，与Chase-Lev variant使用相同Task body、worker count、admission和shutdown边界。oneTBB comparison为构建时可选adapter，仅在可表达相近语义的subset运行，记录library/compiler版本且不宣称feature parity；缺失oneTBB不使Astra benchmark失败。Worker matrix默认取不超过报告hardware concurrency的唯一`1,2,4,8,...`幂次及最大值，所有随机图/负载使用记录的固定seed。

### Invariants

- 每个variant执行同一逻辑work量并产生相同checksum/outcome集合。
- baseline不能省略TaskHandle/Outcome成本却与完整Astra end-to-end结果直接比较；不同surface必须单独标注。
- queue microbench与scheduler end-to-end数字不得混在同一排名。
- random workload的seed、shape parameters和生成器版本进入artifact。
- sleep-based fake CPU work禁止；CPU workload用可验证、难被优化消除的deterministic kernel。
- worker count超过可用CPU的oversubscription case必须显式分类，不混入default scaling chart。
- oneTBB/其他外部baseline不承担Astra correctness oracle角色。

### Scope and variants

| Question | Required comparison |
|---|---|
| Work-Stealing value | Global FIFO vs locked WS vs Chase-Lev |
| lock-free deque value | locked semantic oracle vs Chase-Lev |
| instrumentation cost | Off vs Basic vs Detailed vs Trace |
| fairness | per-Priority service share and tail wait |
| Deadline behavior | offered load vs miss/lateness curve |
| external ecosystem context | optional oneTBB comparable subset |

### Rationale

分层corpus既能回答“哪里花时间”，也能回答“完整功能是否改善真实组合负载”。in-tree baselines保留语义可比性，外部baseline只提供背景而不劫持设计目标。

### Rejected alternatives

- 只测micro empty task：代表性不足。
- 只与oneTBB比：功能/ABI/策略不同且依赖环境。
- 删除locked oracle只保留最快实现：回归时失去语义与性能参照。
- 每次随机seed：结果不可复现。
- 用sleep模拟工作：主要测OS timer/scheduling。
- 单一worker count：看不到contention/scaling拐点。

### Consequences

- corpus运行成本较高，需要quick、standard和research profile，但case定义/语义相同。
- 新调度机制进入稳定版前必须增加primitive与至少一个组合case。
- Results文档需解释surface差异而非只列“倍数”。

### Non-goals and deferred risks

- 不保证Astra在每个case优于所有baseline。
- 不首发NUMA、I/O或GPU workload。
- 不把外部库内部指标映射成Astra Metrics schema。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受三层corpus、in-tree semantic baselines、固定seed/worker matrix和oneTBB仅作可选背景比较。
- Code or data evidence: 总设计列出micro/imbalanced/fork-join/DAG/oneTBB；D-001要求先locked baseline；后续决策增加Coroutine/Priority/Deadline。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0040](../../docs/adr/0040-benchmarks-are-verified-reproducible-experiments.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-143 — Benchmark artifact 保留环境与原始重复，回归门槛由专用稳定 runner 执行

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

单次运行、只报告best value或随意删除outlier会掩盖噪声。共享PR CI机器的频率、邻居负载和虚拟化变化很大，硬编码“不得慢5%”容易产生假失败；完全没有回归流程又使Benchmark沦为展示脚本。

### Decision

场景runner默认协议为：每个case/variant至少2秒warmup，随后10个独立measured repetitions，每个repetition目标timed region至少1秒；参数可通过profile提高或降低，但artifact必须记录实际值且低于default的quick结果不能更新长期baseline。variant执行顺序按记录seed随机化，支持process-per-repetition isolation；不自动删除outlier。报告每个repetition原值、median、MAD、p10/p90及bootstrap 95% confidence interval，同时保留Metrics histogram buckets而非只存派生p99。

版本化JSON artifact至少记录：git commit/dirty、benchmark schema/case version、UTC wall timestamp仅作标识、steady durations、OS/kernel、CPU model/logical count、memory、power/affinity/turbo信息若可得、compiler/version/flags、build type/LTO/sanitizer、dependencies、worker/options/capacity/seed、Metrics/Trace level、raw repetition、checksum和invalid/loss diagnostics。缺失不可探测metadata标记unknown，不猜测。

普通PR CI只运行benchmark build、smoke correctness和宽松异常检测，不以共享runner数字阻塞发布。正式performance gate仅在记录的dedicated stable runner上，用仓库versioned policy逐case定义primary metric、minimum practical effect与allowed regression；比较必须同时满足effect threshold与bootstrap confidence排除零才失败，并保存candidate/baseline artifacts。任何基准改善都不是correctness gate替代，任何优化合并前仍通过完整tests/sanitizers。

### Invariants

- best-of-N不作为primary report；所有有效repetition保留。
- invalid repetition不混入统计，但必须保留原因和原进程输出。
- baseline与candidate环境metadata不兼容时工具拒绝自动判定，而不是归因给代码。
- baseline更新是显式reviewed artifact/policy动作，不由一次green run自动覆盖。
- throughput、median latency、tail/fairness可能方向不同；每case只按预先声明primary gate，其他仍报告。
- no-regression claim必须链接原始artifact、commit和runner identity。
- benchmark不能证明memory safety、linearizability或硬实时能力。

### Scope and variants

| Environment | Use |
|---|---|
| developer laptop | exploration, no release claim |
| shared PR CI | build/smoke/broad anomaly |
| dedicated stable runner | reviewed regression gate |
| sanitizer runner | correctness only |
| optional oneTBB environment | contextual comparison |

### Rationale

保留原始数据与完整环境使结论可审计；重复/稳健统计降低噪声。只在专用runner设置门槛避免把基础设施抖动误判为代码回归，同时仍保留真正的发布性能纪律。

### Rejected alternatives

- 单次或best-of-N：偏乐观且不可估计噪声。
- 自动删除outlier：可能删除真实tail regression。
- 所有PR共享runner硬5% gate：高flakiness。
- 只保存CSV summary：丢失环境、原始分布和invalid原因。
- 每次自动更新baseline：把回归吸收到新常态。
- Benchmark green替代tests：性能不证明正确性。

### Consequences

- 需要artifact schema、comparison CLI和dedicated runner运维说明。
- 初期没有dedicated runner时只能报告数据，不能做强发布性能声明。
- 数值默认可根据长期噪声通过decision/spec修订，不能在实现中静默改变。

### Non-goals and deferred risks

- 不规定具体CI供应商或硬件采购。
- 不首发跨不同CPU归一化score。
- 不以统计显著但实践影响极小的变化阻塞发布。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受2s warmup/10×1s重复、完整versioned artifact、无自动outlier删除及专用runner双门槛回归策略。
- Code or data evidence: D-141/D-142定义harness/corpus；D-137要求保留histogram buckets。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0040](../../docs/adr/0040-benchmarks-are-verified-reproducible-experiments.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-144 — Core 是 C++20 compiled CMake library，Tier-1 支持 Linux/Windows x64

Status: superseded

Date: 2026-08-26

Supersedes: None

Superseded by: D-167

### Context

并发Runtime包含大量非模板实现、平台线程细节和内部状态，不适合header-only传播编译成本与实现。Chase-Lev的lock-free性质依赖目标atomic能力；若把“可编译”和“lock-free”混为一谈，会在未验证平台做虚假承诺。

### Decision

AstraScheduler是需要C++20、线程与异常支持的compiled library，公共CMake target为`AstraScheduler::AstraScheduler`，安装导出`AstraSchedulerConfig.cmake`和version file；public headers只位于`include/astra/`，实现/第三方依赖不泄漏。默认构建静态库，尊重显式`BUILD_SHARED_LIBS`生成shared variant；tests/examples/benchmarks/tools分别由默认合理的`ASTRA_BUILD_*`选项控制，consumer build默认不下载测试/benchmark依赖。core不要求RTTI，但不支持`-fno-exceptions`/等价模式。

Tier-1 release matrix为64-bit Linux x86_64（GCC 13+与Clang 17+）和Windows x64（MSVC 19.38+/VS 2022 17.8+）；Tier-2为Linux AArch64 GCC/Clang native weak-memory CI，功能目标相同但不作为每个patch的阻塞平台。32-bit、macOS和其他Unix在v1.0前为unsupported/best-effort source portability。若`std::atomic<std::uint64_t>`或所需atomic不满足lock-free capability，构建仍可使用locked Local Deque semantic fallback；`Scheduler::capabilities()`和benchmark metadata必须报告`lock_free_local_deque`，不得因类型名或算法来源宣称lock-free。

### Invariants

- CMake target声明`cxx_std_20`，不静默降级到非标准Coroutine扩展。
- public compile definitions只包含documented feature/version/export macros，不传播warnings-as-errors或sanitizer flags给consumer。
- package install后必须通过独立consumer smoke project的find_package/link/run。
- shared/static使用同一public semantics与test suite。
- platform abstraction集中封装thread naming/diagnostic等非标准功能；调度正确性不依赖特定OS私有API。
- fallback deque保持D-092 endpoint与所有Task语义，只改变capability/performance。
- native AArch64用于memory-order stress；仅QEMU编译成功不能证明weak-memory行为。

### Scope and variants

| Platform | v1.0 status |
|---|---|
| Linux x86_64 GCC/Clang | Tier-1 supported |
| Windows x64 MSVC | Tier-1 supported |
| Linux AArch64 GCC/Clang | Tier-2 supported/tested nightly |
| macOS/other 64-bit | best-effort, not release-gated |
| 32-bit | unsupported |

### Rationale

compiled target隐藏深模块实现并缩短consumer编译；明确tier与minimum toolchain把“设计上portable”与“持续验证支持”分开。locked fallback保留正确性，使lock-free成为可检测能力而非运行前提。

### Rejected alternatives

- header-only：暴露实现、增加ODR/编译成本并难以隐藏平台代码。
- 只支持当前Windows开发机：无法验证portable C++ memory model。
- 所有平台一律“支持”：缺乏CI证据。
- atomic非lock-free就拒绝构建：不必要地丢失semantic fallback。
- 禁用异常支持：公共Outcome/error契约依赖exception_ptr和抛出式get。

### Consequences

- CI与release需要Tier-1矩阵和定期native AArch64资源。
- CMake package、export visibility和consumer smoke属于v0.1起的工程基础。
- Tier/minimum compiler变化是documented support policy变更。

### Non-goals and deferred risks

- 不承诺freestanding、embedded或WebAssembly。
- 不首发C++ modules/package-manager-specific recipe。
- 不保证unsupported平台的performance或Reaper unload行为。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受compiled C++20 CMake library、Linux/Windows x64 Tier-1、AArch64 Tier-2和可报告locked fallback。
- Code or data evidence: D-001要求portable baseline；D-098至D-103定义Chase-Lev capability边界；总设计CI已有GCC/Clang/MSVC。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0041](../../docs/adr/0041-releases-guarantee-source-api-not-cross-toolchain-abi.md), [ADR-0047](../../docs/adr/0047-linux-only-support-and-wsl-development.md)
- Spec destinations: R-092 (superseded by R-111)
- Tickets: Historical AST-052 scope; active replacement uses D-167/R-111
- Tests: Historical Linux/Windows matrix removed by D-167

## D-145 — v1 使用 SemVer 保证源码兼容但不承诺跨版本 C++ ABI

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

C++ ABI受compiler、standard library、CRT、build flags及public STL类型影响。若把v1.0“稳定API”误读为任意二进制可混用，会给Windows/Linux shared library用户制造未定义行为；若0.x也完全不说明变更规则，分版本学习开发又无法跟踪迁移。

### Decision

项目采用Semantic Versioning。`0.x`期间每个minor代表计划里程碑，可以修改public source API/behavior，但必须更新spec decision、migration note和compile/runtime tests；patch只做向后兼容修复，不引入计划性breaking change。自`1.0.0`起，在相同受支持toolchain/build配置下承诺public documented source compatibility：breaking source/semantic change需要major，新增兼容能力用minor，兼容修复用patch；可行时breaking replacement至少提前一个minor deprecated。

v1不承诺跨Astra版本、compiler、stdlib、CRT或build-mode的稳定C++ binary ABI。static library为推荐分发；shared build按exact Astra version与toolchain ABI tag配套，CMake package/config生成并校验version/compiler/runtime关键tag，binary consumer升级时必须rebuild/relink。所有公开符号经export macro控制，internal symbols hidden；public object不得跨不匹配CRT allocator边界销毁。

Metrics snapshot、Trace event/JSON和Benchmark artifact各自拥有独立`schema_version`：reader必须拒绝未知required major并可忽略同major新增optional fields；项目SemVer升级不自动重解释旧schema。公开`ASTRA_VERSION_MAJOR/MINOR/PATCH`及可查询version value，不用preprocessor feature猜测运行库ABI。

### Invariants

- “stable API”在README明确写source/semantic scope，不简写为ABI stable。
- patch release不得改变已批准Outcome、wait、shutdown等observable contract来修复实现方便性。
- 0.x breaking change仍必须有ledger supersession和migration，不允许静默漂移。
- shared package mismatch应在configure/load边界尽早拒绝，不能依赖偶然link success。
- schema major不兼容必须显式error，不伪造默认字段继续分析。
- private implementation类型不进入public headers、inline storage或template diagnostics可依赖布局的位置，除非本就是public value type。

### Scope and variants

| Compatibility | Commitment |
|---|---|
| v1 documented source API | SemVer |
| v1 observable semantics | SemVer |
| same-version supported build | tested |
| cross-version binary ABI | not guaranteed |
| cross-compiler/stdlib/CRT ABI | not guaranteed |
| data/trace schemas | separate version policy |

### Rationale

源码兼容是现代C++库可实现且对用户有价值的稳定边界；跨工具链ABI需要PImpl/C ABI/严格依赖冻结，会显著限制TaskHandle模板与性能。明确exact-version shared policy比模糊承诺安全。

### Rejected alternatives

- v1承诺跨编译器稳定ABI：当前public C++模型无法可靠兑现。
- 永远只发static、不支持shared：限制集成但不能消除同进程配置问题。
- 0.x无迁移纪律：tickets/spec失去追踪价值。
- trace/metrics schema跟随project SemVer隐式变化：离线工具难以兼容。
- 用header version允许加载任意runtime：header/library mismatch危险。

### Consequences

- Release notes必须分Source、Semantic、Schema、Build/ABI变化。
- Package CI覆盖header/library exact-match及故意mismatch failure。
- 若未来需要长期ABI，可在独立major设计C/PImpl boundary，不受当前承诺阻挡。

### Non-goals and deferred risks

- 不定义Linux distribution或Windows installer打包。
- 不承诺C ABI、language bindings或plugin ABI。
- 不在v1.0首发长期支持分支策略。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受SemVer源码/语义稳定、0.x迁移纪律、无跨版本/工具链ABI保证及独立schema version。
- Code or data evidence: 项目计划v1.0稳定public API；TaskHandle/Task/Graph均含C++模板与STL边界。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0041](../../docs/adr/0041-releases-guarantee-source-api-not-cross-toolchain-abi.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-146 — v0.1 至 v1.0 按纵向可运行里程碑交付并设置统一 release gates

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

旧路线把TaskHandle/Cancellation推迟到v0.4，与D-041“首个版本submit返回TaskHandle”冲突；若按组件横向先写所有queue再补生命周期，每个中间版本无法体现最终语义。另一方面把146项决策一次实现成单一版本又违背用户后续按版本拆Ticket的计划。

### Decision

采用以下纵向、每版可构建运行的里程碑：

- Phase 0（untagged scaffold）：CMake package骨架、format/lint、GoogleTest、CI/presets和最小consumer smoke；
- `v0.1.0` Runtime baseline：Global FIFO fixed workers、从首版返回TaskHandle/Outcome、wait/get/exception、cooperative cancellation、External capacity/admission、完整Scheduler shutdown、独立Runtime State、Reaper/Finalization；
- `v0.2.0` locked Work-Stealing：Worker abstraction、per-Worker locked Local queues、routing、local/global fairness、steal/backoff与park epoch；
- `v0.3.0` Chase-Lev：seq_cst oracle、portable production memory order、grow/retention/rebase/fallback与stress/litmus；
- `v0.4.0` DAG：builder/freeze、atomic graph admission、edge policies、failure/cancel propagation、GraphRun/report；
- `v0.5.0` Coroutine + Timer：cold Task/spawn、resume tickets、Astra awaiters、Graph coroutine nodes、yield/cancellation point、Worker-driven sleep timers；
- `v0.6.0` Priority + Deadline：four bands、weighted fairness、TaskOptions、first-start EDF与miss observation；
- `v0.7.0` Observability：Metrics levels/snapshot、TraceCollector/schema、Chrome export与viewer examples；
- `v0.8.0` Benchmark Framework：micro/scenario harness、semantic baselines、artifacts和comparison tooling；
- `v0.9.0` hardening release candidate：Tier-1/2 matrix、sanitizers/stress、package/install、API/schema review、docs/examples、performance evidence；
- `v1.0.0` stable source API：关闭全部已批准P0/P1 spec gap，冻结documented public surface与migration baseline。

每个tag必须是纵向可运行increment，并满足统一Definition of Done：其approved spec rules有Ticket/测试证据；Tier-1 Release build与unit/integration通过；涉及并发的变更通过对应stress及可用sanitizer；public behavior/docs/examples/CMake package同步；Metrics/Trace schema fixture更新；相关benchmark至少build/smoke且性能claim附artifact。后续版本可以提前建立未来feature的private seam，但不得公开未定语义或让当前milestone依赖未交付feature。

### Invariants

- v0.1不得用`std::future`临时public API再于v0.4替换。
- locked/Chase-Lev两个版本保持同一deque endpoint与scheduler semantics，便于oracle比较。
- 每个版本的shutdown/reaper正确性不因后续feature缺失而降级。
- v0.5 timer不依赖v0.6 Deadline；Wake Time/Deadline保持独立。
- Benchmark baseline从早期版本逐步积累，v0.8交付完整framework而非第一次测性能。
- v0.9只做hardening/兼容修订；若发现需要重大语义变化，回到ledger/spec而不为赶v1冻结错误API。
- release tag不得以“测试未来补”绕过Definition of Done。

### Scope and variants

| Milestone class | Breaking source change |
|---|---|
| Phase 0 | unrestricted internal scaffold |
| v0.x minor | allowed with decision + migration |
| v0.x patch | no planned breaking change |
| v0.9 RC | only evidence-driven approved correction |
| v1.x | SemVer source/semantic policy |

### Rationale

路线按可演示的纵向能力递进，同时把最难的生命周期/TaskHandle契约放在基础层，避免后期推翻所有feature。v0.9专门消化跨平台、sanitizer、package和文档证据，使v1稳定不是简单改tag。

### Rejected alternatives

- 旧路线v0.4才引入TaskHandle：首版public API必然重写。
- 一次实现最终所有功能：无法按Ticket/版本验证学习增量。
- 先无锁再locked oracle：难以区分算法与scheduler错误。
- v1.0只以feature checklist判定：缺少兼容/证据/hardening。
- 每个feature独立横向分支长期不集成：组合语义太晚暴露。

### Consequences

- 后续`to-spec`生成全项目规则后，`to-tickets`按此milestone拆纵向tracer bullets及阻塞边。
- 旧设计文档Phase/Git milestone表需要以本决策替换。
- v0.1范围较普通线程池更大，但建立后续所有feature依赖的稳定Runtime substrate。

### Non-goals and deferred risks

- 本决策不指定每个版本日历日期或人日估算。
- 不在设计讨论中直接拆Ticket或分配负责人。
- 不保证v0.x每个minor都发布到公共package registry。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；接受TaskHandle/lifecycle从v0.1起的纵向里程碑、v0.9 hardening和统一release gates。
- Code or data evidence: D-041明确TaskHandle首版；D-001要求locked baseline；用户明确后续按版本拆Tickets。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0041](../../docs/adr/0041-releases-guarantee-source-api-not-cross-toolchain-abi.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-147 — Ready routing 先服从具体 awaiter/Deadline 规则再回落到 publication context

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

审计发现D-090把DAG/resume/timer/event统称为“owner Worker上Local、否则Global”，而后来的D-122明确yield强制Global、D-127明确timer winner强制Global、D-133明确首次开始Deadline work进入Global EDF。D-130/D-133又用“coroutine resume进入Global”概括，可能错误覆盖普通awaiter在owner Worker上完成时的locality。

### Decision

Ready destination按以下优先级解析，越具体的规则优先：

1. v0.1所有Ready仍按D-090进入Global；
2. 从未开始且带Task Deadline的Ready work按D-133进入Global EDF heap；
3. `astra::yield`与Astra timer expiry/stop resume按D-122/D-127强制进入source Runtime普通Global Priority band；
4. External Submission和cross-Runtime publication进入目标/source Runtime普通Global band；
5. same-Runtime Internal Submission进入current owner Local bottom；
6. DAG dependency release、TaskHandle/GraphRun await completion及其他没有专用规则的event/coroutine resume，若publication winner当前就是所属source Runtime Worker，则进入该owner Local bottom，否则进入普通Global band。

任何Deadline Task首次Running后永久离开EDF；其后每次resume根据触发它的具体awaiter规则选择普通Local/Global Priority band，不因历史Deadline一律Global。此决策澄清并在冲突处优先于D-090的通用timer/event措辞、D-130的“timer/coroutine resume”汇总行及D-133的“later resume Global”汇总行，不改变这些决策的其他内容。

### Invariants

- 任何off-owner publisher不得写Local bottom。
- Global/Local destination都使用Task原base Priority普通band，除非是首次Deadline EDF entry。
- 路由不改变Task Identity、Outcome、capacity、shutdown或resume winner。
- yield即使在owner Worker调用也必须Global，以兑现明确公平service opportunity。
- timer即使由owner Worker处理到期也必须Global，以统一cancel/expiry路径并解除原Worker亲和。
- generic await completion在owner Local发布仍不得inline resume；它只发布Ready ticket。
- Trace记录实际source/destination与routing reason，不能从awaiter kind猜测。

### Scope and variants

| Ready cause | Destination after v0.1 |
|---|---|
| External/cross-Runtime submit | ordinary Global |
| same-Runtime Internal submit | owner Local |
| deadline, never started | Global EDF |
| yield | ordinary Global |
| timer fire/cancel | ordinary Global |
| generic await/DAG release on owner Worker | owner Local |
| generic await/DAG release off owner | ordinary Global |

### Rationale

专用awaiter规则表达公平性或lifetime意图，应覆盖一般locality默认；没有特殊需要时保留owner-local能减少Global contention。显式precedence让新增awaiter必须声明路由，而不是依赖模糊“coroutine resume”类别。

### Rejected alternatives

- 所有Coroutine resume一律Global：不必要丢失owner-locality。
- 所有resume按publisher context：破坏yield/timer已批准的Global语义。
- Deadline历史永久强制Global：Deadline只约束首次开始，不应污染后续continuation。
- off-worker随机写Local：破坏Chase-Lev owner-only。

### Consequences

- Resume publication API需要显式`RoutingReason/ReadyDestinationPolicy`，避免调用点自行猜测。
- D-130/D-133的汇总表在spec中按本决策改写，不原样复制冲突行。
- routing tests覆盖每种awaiter在owner/off-worker和deadline-after-first-start组合。

### Non-goals and deferred risks

- 不提供用户自定义affinity或destination override。
- 不保证resume回到上次执行Worker。
- 不改变Priority/Deadline selection order。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；一致性审计后接受专用routing优先、普通awaiter按publisher context回落的统一precedence。
- Code or data evidence: D-090、D-122、D-127、D-130与D-133的路由文字存在可观察冲突；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0026](../../docs/adr/0026-work-stealing-balances-locality-with-global-fairness.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-148 — Process Metrics 只观察 Reaper/Finalization 且查询不初始化服务

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Runtime Metrics由Scheduler Handle查询，但orphan Runtime在Handle消失后仍由Reaper推进；总体设计列出handoff、join与finalization指标，却没有定义Handle外的稳定观测入口。查询若惰性创建或重启Reaper又会违反D-035的不可重启生命周期。

### Decision

提供无参、无副作用的`astra::process_metrics_snapshot()`，返回固定存储、不可变`ProcessMetricsSnapshot`。该低频控制面统计始终开启，不受任一Scheduler的MetricsLevel影响；调用前若Reaper从未初始化，返回`ProcessServiceState::NotStarted`和合法零值，绝不初始化线程/注册表；Finalized后返回保留的最终累计事实，绝不重启服务。

固定字段至少包括累计`runtime_registrations`、`runtime_handoffs`、`runtimes_joined`、`finalization_begin_calls`、`finalization_wait_timeouts`、`finalization_escalations`，以及当前`registered_runtimes`、`pending_runtimes`、`join_ready_runtimes` gauges、ProcessServiceState/FinalizationState、capture steady time、finalization elapsed/complete duration和`saturated`。更新使用Reaper控制面锁或原子但不作为lifecycle同步来源；snapshot每字段安全，若未取得单一控制面锁则沿用D-137 fuzzy标记/区间语义。

### Invariants

- 查询不创建Reaper thread、Runtime registration或FinalizationControl。
- `finalization_wait_timeouts`每次真实返回TimedOut增加，不能把后台未完成自动计超时。
- begin重复调用共享同一finalization但每次API invocation可累计begin_calls；唯一状态转换另由state表示。
- gauges不得因Scheduler Handle消失而提前减；只随真实registration/handoff/join状态变更。
- Finalized snapshot中的累计counter和completion duration在进程余生保持不变，除查询调用数外不新增隐藏行为。
- Process Metrics不聚合每Runtime task counters，避免全局热原子与已销毁Runtime身份问题。
- saturating规则与schema version兼容D-135至D-137。

### Scope and variants

| Service state | Query effect/result |
|---|---|
| never initialized | NotStarted, zero facts, no initialization |
| active/idle | current control-plane snapshot |
| finalizing | progress gauges and elapsed |
| finalized | preserved final snapshot, no restart |
| unrecoverable coordinator failure | existing fail-fast policy; no fabricated snapshot |

### Rationale

独立process snapshot覆盖Scheduler Handle之外的lifecycle，同时低频事件允许默认常开。严格side-effect-free查询保留Reaper一次性边界，避免观测改变被观测系统。

### Rejected alternatives

- 只在Scheduler::metrics_snapshot暴露：orphan/finalization后无Handle。
- process query惰性启动Reaper：观测产生全局线程和生命周期副作用。
- 聚合所有Task metrics：跨Runtime高contention且销毁后口径复杂。
- Finalized后清零：丢失最终诊断事实。
- 每个FinalizationControl独立counter：同一进程完成事实分叉。

### Consequences

- Process control block在Finalized后保留小型只读metrics state到进程结束。
- Metrics文档与tests分开RuntimeMetricsSnapshot和ProcessMetricsSnapshot。
- Trace仍提供高频时序；Process Metrics只提供累计/当前控制面事实。

### Non-goals and deferred risks

- 不提供跨进程监控endpoint或callback。
- 不保存每个已join Runtime的明细历史。
- 不把fail-fast错误转成可恢复metrics状态。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受始终开启、side-effect-free且Finalized后保留的Process Metrics查询。
- Code or data evidence: 总设计§26列出Reaper/Finalization指标；D-035禁止Reaper重启；D-135仅定义Scheduler Runtime snapshot。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0042](../../docs/adr/0042-observability-covers-process-lifecycle-and-wait-edges.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-149 — Metrics/Trace 显式记录同步 wait 与 Coroutine await edge 但不在线检测环

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-050不维护通用wait-for graph，却要求Metrics/Trace帮助诊断Indirect Wait Cycle；D-051要求跨Runtime Trace保留source与remote target。D-136/D-139最终schema尚未列出wait/await事件，因此现状无法兑现早期诊断后果。

### Decision

Basic Runtime Metrics补充固定低基数counter：`task_wait_calls`、`graph_wait_calls`、`wait_for_timeouts`、`same_runtime_helping_waits`、`cross_runtime_helping_waits`、`coroutine_await_registrations`、`direct_self_wait_rejections`与`helping_depth_rejections`。Detailed补充`thread_wait_duration`、`helping_wait_duration`和`coroutine_await_duration`固定log2 histogram；即时已完成等待也计call并记录零/最小bucket，不因没有suspend而消失。

Trace wait category增加固定事件`WaitBegin`、`WaitEnd`和Coroutine `AwaitArmed`、`AwaitTriggered`/`AwaitResumed`。事件携带source RuntimeId/可选source TaskId、target kind（Task/GraphRun/Finalization不混用）、target RuntimeId与logical target id、wait mode（external blocking/same-runtime helping/cross-runtime helping/coroutine）、timeout presence和end reason（completed/timed_out/self_rejected/depth_rejected/source_cancelled/target outcome category）。Helping期间执行的其他Task segment仍归属真实Task/Runtime，wait edge保持外层source Task identity；跨Collector时允许只看到source edge并以target id指向未采集Runtime。

这些事件只用于离线诊断和统计，不构建在线全局wait-for graph、不自动检测/打破Indirect Wait Cycle、不改变timeout/cancellation或Helping策略。Direct Self-Wait在语义拒绝确定后可记录rejection diagnostic，但不得先注册wait edge、进入Helping或改变TaskState。

### Invariants

- target id来自稳定logical identity，不使用Handle/TCB地址。
- WaitBegin/End在零drop trace中按source wait generation配对；drop时不得合成缺失事件。
- Coroutine await trigger与resume分开，显示“事件已完成但continuation仍在Ready queue”的区间。
- `wait_for` TimedOut只增加timeout counter，不取消target或伪造completion。
- wait duration使用source视角steady time；Helping期间包含执行其他Task的wall time，不误称blocked CPU time。
- Metrics不带target id label，保持低cardinality。
- Finalization wait指标的process级counter仍归D-148；Runtime wait schema不重复核算。

### Scope and variants

| Wait form | Metrics/Trace |
|---|---|
| external TaskHandle wait/get | thread wait edge |
| Worker same Runtime | helping edge + nested real segments |
| Worker cross Runtime | source helping edge + remote target id |
| TaskHandle/GraphRun co_await | arm/trigger/resume edge |
| Direct Self-Wait | rejection diagnostic only |
| Indirect cycle | visible offline if captured, never auto-resolved |

### Rationale

显式edge让Chrome Trace解释Helping嵌套、远端依赖和trigger-to-resume delay，同时固定counter可发现等待模式异常。保持纯观测避免为诊断引入复杂、可能自身deadlock的在线全局图。

### Rejected alternatives

- 不记录wait：无法兑现D-050/D-051诊断目标。
- Metrics以target TaskId作label：无界cardinality。
- Runtime在线维护wait-for graph：跨Runtime锁序、动态edge与cycle recovery复杂。
- 发现cycle自动cancel：改变Outcome且无明确victim policy。
- trigger与resume合成一个事件：掩盖Ready queue delay。

### Consequences

- TraceEvent需要wait generation/target fields，Chrome exporter增加flow edge。
- Metrics conservation tests覆盖即时、timeout、Helping、cross-runtime和Coroutine await。
- Viewer可提供可选wait graph/疑似cycle分析，但必须标明trace loss与采集范围限制。

### Non-goals and deferred risks

- 不保证trace loss或未附加Runtime下检测全部cycle。
- 不提供生产自动deadlock detector或watchdog。
- 不记录任意用户mutex/condition-variable等待。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受低基数wait metrics和稳定wait/await trace edges，同时保持无在线cycle detection。
- Code or data evidence: D-050要求观测诊断、D-051要求跨Runtime target identity；D-136/D-139原schema缺少相应字段。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0042](../../docs/adr/0042-observability-covers-process-lifecycle-and-wait-edges.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-150 — std::async 只作显式 launch::async 的粗粒度背景基线

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

总体目标提到与`std::async`比较，但D-142只正式固定in-tree baselines和optional oneTBB。`std::async`不是可配置fixed-worker scheduler，默认launch policy还可选择deferred，因此若直接放进吞吐排名会把不同并发拓扑伪装成等价实现。

### Decision

Benchmark corpus保留`std::async(std::launch::async, ...)` adapter，仅用于粗粒度、相互独立、结果可校验的背景case；永远显式指定`std::launch::async`，不使用implementation-selected default/deferred policy。Artifact记录实现/stdlib、并发future数量、观察到的peak threads（可测时）和没有固定worker-count parity的限制。该adapter不运行递归fork-join、DAG、Cancellation、Coroutine、Priority、Deadline或shutdown feature ranking，也不成为primary regression gate。

固定worker语义的主要普通线程池比较仍是D-142 in-tree mutex Global FIFO baseline；oneTBB仍为可选相近subset。任何README图表必须把Astra/in-tree fixed workers、oneTBB task runtime和`std::async` language facility分组，不用单一“快N倍”标题暗示完全等价。

### Invariants

- 每次async返回的future必须被get/join并验证结果，不能通过遗漏等待缩短timed region。
- explicit launch::async adapter不得回落deferred而不报错。
- 并发数量有case上限，避免为micro task无界创建OS threads拖垮runner。
- setup/timed/drain边界与Astra case明确记录；不相同surface不得作为primary直接比值。
- std::async失败/资源异常使样本invalid，不吞掉后继续少做work。
- 外部baseline版本/环境变化不能更新Astra dedicated regression baseline。

### Scope and variants

| Baseline | Role |
|---|---|
| in-tree Global FIFO fixed workers | primary semantic baseline |
| in-tree locked Work-Stealing | scheduler/Chase-Lev oracle |
| optional oneTBB | ecosystem comparable subset |
| explicit std::async | coarse independent-task context only |

### Rationale

保留`std::async`满足学习和生态背景价值，同时明确其线程拓扑与功能表面不同，避免不公平排名。真正的算法收益仍由同接口in-tree baselines证明。

### Rejected alternatives

- 删除std::async比较：与总体目标不一致且失去教学背景。
- 使用默认launch policy：deferred执行使case语义不确定。
- 把std::async放进micro throughput主排名：通常测线程创建/实现策略而非相同scheduler。
- 人工在std::async外再建线程池限流：已不再是std::async原facility比较。

### Consequences

- Scenario metadata和报告模板增加baseline-kind/equivalence disclaimer。
- std::async case数量保持小且粗粒度，完整corpus运行时间影响有限。
- 性能宣传以同语义baseline为主，外部设施只提供context。

### Non-goals and deferred risks

- 不保证所有stdlib的std::async实现使用相同线程策略。
- 不评价std::execution或未来标准executor。
- 不把std::async资源上限映射成Astra backpressure。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受explicit launch::async仅作粗粒度背景，in-tree fixed worker仍为主基线。
- Code or data evidence: 总设计§1.1列出std::async对比；D-142尚未给出其公平性边界。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0040](../../docs/adr/0040-benchmarks-are-verified-reproducible-experiments.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-151 — Unobserved Exception 诊断服从 Metrics/Trace 启用状态

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

审计发现D-082写成最终释放时“必须增加metrics并尽力trace”，而D-135明确MetricsLevel::Off跳过全部非正确性measurement。若Off仍维护隐藏unobserved counter，就不是真正Off；若没有澄清，实现可能让诊断配置改变Task释放安全。

### Decision

Exception Outcome继续维护幂等`exception_observed`事实，`get()`/TaskHandle co_await按D-082在传播前标记。最终shared completion state释放时，若Failed且未观察：Runtime Metrics为Basic/Detailed时增加D-136稳定字段`unobserved_failures`；Metrics Off时不维护或增加该counter。若附着TraceCollector当前存在Recording capture且unobserved-failure category启用，则尽力发出固定`UnobservedFailure` event；Collector缺失、Stopped、category关闭或buffer drop都允许没有事件。所有配置下均不得log、callback、terminate、改变Outcome或延长Runtime正确性lifetime。

本决策在冲突处澄清D-082的“必须”：必须执行安全的最终观察判定，但诊断输出只在对应观测面启用时产生；D-081的纯诊断、非控制边界不变。字段名以D-136的`unobserved_failures`为稳定schema，早期`tasks_failed_unobserved_total`只是描述性旧名。

### Invariants

- Metrics Off snapshot明确disabled，不能暗藏可查询unobserved counter。
- Trace capture可晚于Task failure但早于shared state最终释放；是否记录由最终释放时capture状态决定，不追溯合成历史事件。
- `exception_observed` tracking本身不得依赖Metrics/Trace state，以避免capture切换竞态改变定义。
- diagnostics failure/drop不阻止shared state析构或Runtime join。
- Metrics与Trace数量可因level/capture/loss不同，不用于彼此强制守恒。
- `state/wait/wait_for`仍不标记Exception observed。

### Scope and variants

| Metrics | Trace at final release | Diagnostic |
|---|---|---|
| Basic/Detailed | recording/category/capacity | counter + event |
| Basic/Detailed | unavailable/disabled/full | counter only |
| Off | recording/category/capacity | event only |
| Off | unavailable | no external diagnostic, safe release |

### Rationale

Observability必须可关闭且不能成为正确性前提。始终跟踪observed bit保留定义稳定，输出则服从各自明确启用状态，兼顾低开销和可诊断性。

### Rejected alternatives

- Off仍维护hidden counter：违反D-135并污染benchmark下界。
- 未观察异常总是Trace：没有Collector/capture时需隐式全局sink。
- 关闭诊断就不跟踪observed bit且运行时动态猜测：capture代际会改变语义。
- 默认stderr/terminate：违反D-081。
- 在Task failure时立即计unobserved：后续get可能正常观察。

### Consequences

- D-082 tests按四种Metrics/Trace组合验证。
- Schema只使用`unobserved_failures`，to-spec不得同时生成两个同义field。
- Trace completeness不保证记录capture开始前已释放的failure。

### Non-goals and deferred risks

- 不公开observed bit或unobserved callback。
- 不持久化Trace关闭期间的pending diagnostic队列。
- 不保证异常终止时flush诊断。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受unobserved诊断遵循Metrics/Trace enablement，Off不维护隐藏counter。
- Code or data evidence: D-082与D-135对Metrics Off存在文字冲突；D-136固定字段名unobserved_failures。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0023](../../docs/adr/0023-unobserved-task-exceptions-are-diagnostic-only.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-152 — GraphReport 获取一次性标记全部真实 Node Exceptions 已观察

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

Graph Node没有per-node TaskHandle，Failed exceptions只由immutable GraphReport暴露。D-082只定义TaskHandle::get观察普通Task异常；若GraphRun从未get_report，节点异常是否计unobserved未定义，若每个Node terminal时立即计又会把后来正常读取report误报为未观察。

### Decision

GraphRun shared state维护幂等`failure_report_observed`，初始false。任意有效GraphRun的`get_report()`在GraphRun完成并返回`const GraphReport&`前将其设为true；D-121 `co_await GraphRun`的`await_resume`返回同一report，也执行相同标记。`state()`、`wait()`、`wait_for()`、request_cancel和Metrics/Trace读取不标记observed。

GraphRun shared state最终释放时，若report包含N个真实Failed Node exception_ptr且`failure_report_observed=false`，按D-151启用规则把Runtime `unobserved_failures`增加N，并尽力为每个Failed Node发`UnobservedFailure` event（携带GraphRunId/NodeId/TaskId）；aggregate Graph state本身不额外计一个synthetic failure。若report已观察则这些Node不计unobserved。Cancelled/RequireSuccess传播节点没有exception_ptr，不计Failed或unobserved。

### Invariants

- 多GraphRun副本/并发get_report最多逻辑标记一次，但每次都返回同一immutable report。
- observation发生在report跨public boundary前，即使调用方随后不检查每个Node也算已获得完整失败集合。
- GraphReport排序/内容不因observed bit改变。
- 最终判断时GraphRun state仍拥有全部exception_ptr和Node identities；不得在Node terminal时提前丢失诊断归属。
- 一个Graph Coroutine Node仍只算一个Failed Node/Task Identity。
- Graph aggregate Failed不制造新的exception或重复unobserved count。
- Metrics Off/Trace unavailable组合严格服从D-151。

### Scope and variants

| Graph observation | Effect |
|---|---|
| get_report | mark all report failures observed |
| co_await returns report | mark observed |
| state/wait/wait_for only | no mark |
| final release, N failures, unobserved | diagnostic count N |
| propagated Cancelled node | no exception diagnostic |

### Rationale

GraphReport是唯一批准的Node exception inspection surface，因此以report跨边界定义观察与TaskHandle::get一致。按真实Failed Node计数保留Task Identity口径且不虚构aggregate exception。

### Rejected alternatives

- Node一失败立即计unobserved：之后get_report无法消除误报。
- GraphRun只计一个unobserved：多个独立失败被压缩。
- wait完成即算observed：调用方未取得report exception_ptr。
- 要求调用方逐Node acknowledge：扩大API并破坏immutable report简洁性。
- Graph aggregate自动rethrow：违反D-112 report不自动传播。

### Consequences

- GraphRun state在最终释放前保留一个observed bit和failed-node count/ids。
- Metrics/Trace tests覆盖多个Failed、Cancelled传播、并发get_report与无Handle最终释放。
- 文档说明“获取report即观察全部异常”，不暗示逐项读取要求。

### Non-goals and deferred risks

- 不公开per-Node acknowledge或observed状态。
- 不提供Graph failure callback/automatic terminate。
- 不改变Graph edge failure propagation。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受get_report/co_await标记全部真实Node exceptions observed，最终按失败Node诊断。
- Code or data evidence: D-108无per-node Handle；D-112唯一GraphReport；D-082仅覆盖TaskHandle路径。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0031](../../docs/adr/0031-graph-failures-cancel-only-required-descendants.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-153 — 公共强类型 RuntimeId、TaskId 与 GraphRunId 关联 API 和 Trace

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

总体设计一直把`TaskHandle::id()`标为候选，而D-139/D-149已经要求Trace与wait edge使用稳定logical TaskId/RuntimeId。若应用日志无法从Handle取得同一ID，就难以关联Trace；若直接返回TCB/coroutine地址，则存在复用、ASLR、隐私和lifetime问题。

### Decision

公开trivially-copyable强值类型`RuntimeId`、`TaskId`和`GraphRunId`，支持default invalid值、`valid()`、equality、three-way/order及`std::hash` specialization；不提供隐式整数/指针转换。`TaskId`和`GraphRunId`逻辑上包含所属RuntimeId与该Runtime内永不复用的nonzero 64-bit sequence，因此在单进程生命周期内全局唯一；`NodeId`继续是Graph定义内的local insertion identity，完整运行节点身份为`GraphRunId + NodeId`。

`Scheduler::runtime_id() const noexcept`返回其RuntimeId；有效`TaskHandle<T>::id() const`返回TaskId，空Handle按D-068在副作用前抛`std::logic_error`；有效`GraphRun::id() const`和`GraphReport::run_id()`返回同一GraphRunId。Metrics snapshot、TraceEvent、Benchmark artifact和diagnostic formatting复用这些public logical types/规范字段，不另造不兼容ID。库提供无分配要求之外的明确文本格式化工具/`operator<<`可放tools层；稳定语义不依赖字符串表现。

RuntimeId/sequence分配采用checked monotonic generation：0保留invalid，gap允许，任何counter耗尽不wrap/reuse。RuntimeId耗尽使新Scheduler在启动Worker前抛`std::overflow_error`；Task/GraphRun sequence耗尽在admission preparation阶段抛`std::overflow_error`并保持D-089完全rollback，不新增SubmissionError枚举。

### Invariants

- Handle复制/移动不改变所关联TaskId；moved-from/empty没有伪造ID。
- Runtime State orphan/handoff/join后已有ID仍保持值语义可比较。
- TaskId不从内存地址、thread id或queue index派生。
- rejected submission可消耗sequence产生gap，但不得产生public Handle/Task lifecycle event。
- Graph Node对应的Runtime TaskId与GraphRunId+NodeId映射稳定记录，但两者不是同一类型。
- ID只提供correlation，不授予控制、查找任意Task或延长lifetime。
- 跨进程重启不承诺ID唯一；artifact还需process/capture context。

### Scope and variants

| Entity/API | Identity |
|---|---|
| Scheduler/Runtime State | RuntimeId |
| TaskHandle | TaskId(RuntimeId, sequence) |
| GraphRun/GraphReport | GraphRunId(RuntimeId, sequence) |
| Graph Node definition | NodeId graph-local |
| running Graph Node | GraphRunId + NodeId + TaskId |
| invalid/default value | sequence/runtime sentinel 0 |

### Rationale

强类型逻辑ID提供日志—Trace—Metrics关联而不泄漏实现地址；组合Runtime scope避免process-wide每Task热原子。checked exhaustion保持“永不复用”在形式上成立，而不是依赖64位永远用不完的假设。

### Rejected alternatives

- 不公开ID：用户无法可靠关联Handle与Trace。
- 返回`uintptr_t`/raw pointer：复用、泄漏和lifetime错误。
- 全进程单Task sequence atomic：所有admission争用全局cache line。
- string UUID：生成/存储/格式化成本高且热路径不必要。
- wrap后复用：同capture/长期Handle可能identity collision。
- ID作为lookup/cancel registry key：扩大安全与lifetime surface。

### Consequences

- Public headers增加小型ID value types/hash，schema直接编码runtime+sequence。
- Tests覆盖copy/move、empty、gap、cross-Runtime inequality和近耗尽injected counter。
- 总体设计中`id()`从候选改为确认Interface。

### Non-goals and deferred risks

- 不提供全局`find_task(TaskId)`、remote control或persistence registry。
- 不承诺ID不可猜测或具备安全token属性。
- 不冻结human-readable字符串为ABI/协议。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受public strong logical IDs，并用runtime-local checked sequence关联Handle/Trace而非地址。
- Code or data evidence: 总设计§15仍把id标候选；D-139/D-149已依赖stable logical IDs；D-089允许preparation rollback。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-154 — Immediate 禁止首次 start 但允许已开始 Coroutine 的 resume segment

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-006/D-080把Immediate表述为不再“启动”未Running Task，而D-119要求Suspended Astra coroutine收到stop后排队resume，在await_resume抛`task_cancelled`并展开frame。若实现把任何Ready resume ticket都当作未Running Task直接Cancelled或禁止执行，取消点和RAII destructor永远无法运行；若把所有resume当新Task又会重复first-start核算。

### Decision

Immediate eligibility必须区分`NeverStartedReady`与`ResumeReady`：从未成功进入Running的Waiting/Ready Task在start/cancel arbitration中发布Cancelled，用户Callable/frame body一次也不执行；已经至少成功first-start的Coroutine Task从Suspended因Astra stop trigger、正常awaiter completion或foreign awaitable自然完成产生的resume ticket仍可由Worker claim并执行resume segment，即使source Runtime已Immediate。resume前stop request保持可见，Astra cancellation-aware awaiter在`await_resume`抛`task_cancelled`；用户可让异常展开为Cancelled，也可捕获后继续，Runtime不强杀或跳过RAII。

已开始Task后续每次suspend仍保留“already started”分类；Immediate下Astra awaiter进入时若已stop应同步/排队到取消点而不建立长期等待，foreign awaitable仍按D-119可能永久Suspended。Graph Coroutine Node使用同一规则。Helping Wait在Immediate下不得first-start任何Task，但可执行已开始coroutine的eligible resume segment，前提是正常scheduler source/priority/arbitration允许；这不构成新admission、External slot或outstanding identity。

### Invariants

- first-start bit/phase是单调事实，不等同当前public TaskState Running。
- NeverStarted Task在Immediate取消不得调用Callable、resume initial-suspended frame或运行其用户body/destructor side effects beyond normal owned object destruction。
- ResumeReady Task不得因当前TaskState Ready被误分类为pre-start取消。
- 每个resume仍需唯一ticket/Worker owner，Immediate不允许inline/concurrent resume。
- stop request后正常return仍按D-056为Value；显式`task_cancelled`逃出才是Cancelled。
- 用户捕获取消并继续属于cooperative模型，可能使Immediate无界；Runtime不反复伪造Cancelled。
- Resume segment继续计D-136 `resume_segments`，不重复`first_starts`或释放External slot。

### Scope and variants

| Immediate work | Eligibility |
|---|---|
| ordinary Task never started | cancel without invoke |
| coroutine initial frame never resumed | cancel without body resume |
| Astra-aware suspended coroutine stop winner | resume to cancellation point |
| already-started coroutine normal event completes | resume with stop visible |
| foreign awaitable never completes | remains Suspended/unbounded |
| Graph coroutine Node already started | same resume rule |

### Rationale

Immediate是“停止接受/首次执行新工作并请求合作停止已开始工作”，不是强制销毁执行栈。允许resume是Coroutine版本的“Running Callable继续到安全点”，保证frame RAII和用户异常处理语义一致。

### Rejected alternatives

- Immediate禁止所有resume：cancellation-aware frame无法展开，必然挂起。
- Suspended直接发布Cancelled并destroy frame：跳过RAII且可能foreign awaitable仍持handle。
- 每个resume算新Task start：重复capacity/metrics/identity并混淆Deadline。
- stop后resume不允许用户catch：C++异常无法被Runtime可靠禁止捕获。
- 为resume创建专用cleanup thread：扩大拓扑与lifecycle。

### Consequences

- Internal scheduling entry携带first-start/resume classification，不能只看public TaskState。
- Immediate drain/worker exit predicate必须把可resume的already-started Task算活动工作。
- Tests覆盖pre-start frame body零执行、Astra cancel unwind、user catch-and-continue、foreign await completion和Helping Immediate。

### Non-goals and deferred risks

- 不保证Immediate有界完成。
- 不强迫用户Coroutine在stop后终止。
- 不允许从未开始Task借“cleanup resume”绕过pre-start cancellation。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受Immediate只禁止first start，已开始Coroutine可resume到合作取消/自然完成。
- Code or data evidence: D-080的“start none”与D-119的stop-trigger resume需要精确区分；D-056规定Running正常返回。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0033](../../docs/adr/0033-suspended-coroutine-cancellation-is-cooperative.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-155 — Scheduler 构造事务成功即 Running，Handle 可复制且不公开 start/restart

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

总体设计同时出现`Created→start()`图和“默认构造后直接submit”示例；D-019允许构造或启动失败但未选public API。公开两阶段start会留下可复制半初始化Handle、并发start、Finalization gate与析构组合；构造即启动则可把所有可失败准备纳入一个RAII事务。

### Decision

公开`explicit Scheduler(SchedulerOptions options = {})`执行同步startup transaction：先验证options，建立/注册Runtime State与D-019预留Reaper handoff能力，分配所有启动必需资源并创建Worker，所有Worker通过startup barrier后一次发布`Running`，构造才返回。任一步骤失败必须阻止用户Callable运行、请求已启动Worker退出并join、撤销Reaper registration/handoff和所有资源后再抛；不存在可观察半启动Scheduler对象。稳定API不提供`start()`、`restart()`或公共Created状态，`SchedulerState`只包含`Running/Stopping/Stopped`。

Finalization registration gate已关闭时构造抛`astra::scheduler_creation_rejected`，其稳定`SchedulerCreationError::FinalizationStarted` reason可程序化识别；非法options继续`std::invalid_argument`，allocation/thread/system failure保持原`std::bad_alloc`/`std::system_error`等类型。startup rollback内部不变量失败按D-040 fail-fast，不能返回携带活动Worker的异常。

`Scheduler`是copyable/movable共享Handle：复制表示同一RuntimeId/Runtime State，不创建Worker或新Runtime；move转移关联并使source为空，`valid() const noexcept`查询关联，空Scheduler的`runtime_id() noexcept`返回invalid RuntimeId，其他public runtime操作在副作用前抛`std::logic_error`。只有最后一个关联Handle释放才执行D-014/D-017的non-Worker同步Graceful或Worker Reaper handoff；任一副本调用shutdown影响共享Runtime，全部副本观察相同state/completion。

### Invariants

- 构造返回的有效Scheduler已经Running且Reaper registration/handoff capability完整可用。
- Worker在Running publication/startup barrier前不得执行用户Task；外部无法在构造返回前取得Handle提交。
- Finalization gate check与registration线性化，不能在gate关闭后漏入新Runtime。
- startup failure后无joinable Worker、Reaper registration、Runtime outstanding work或可观察RuntimeId Handle；ID gap允许。
- 同一稳定Scheduler对象/不同副本的只读、submit和request-style操作按各自协议线程安全；同一对象move/assignment/destruction仍需调用方同步。
- 复制赋值/析构若释放最后Handle可按既有RAII语义阻塞或handoff，不能被误认为廉价无副作用引用计数操作。
- Stopped不可restart；新执行域必须构造新Scheduler，Finalization后构造永久拒绝。

### Scope and variants

| Operation/state | Behavior |
|---|---|
| successful construction | returns valid Running shared Handle |
| invalid options | invalid_argument, no workers |
| finalization already begun | scheduler_creation_rejected(FinalizationStarted) |
| allocation/thread failure | original exception after full rollback |
| copy | same Runtime, no new workers |
| move | source empty |
| empty runtime operation | logic_error before effects |
| Stopped Handle | observable Stopped, no restart |

### Rationale

单阶段RAII启动消除Created/start并发状态和“忘记start”的错误，同时把D-019危险资源准备放在正常异常通道。可复制Handle符合既有“最后Handle”与多组件提交模型，Runtime State继续隐藏真实线程ownership。

### Rejected alternatives

- public Created+start：半初始化共享状态、并发start和析构矩阵膨胀。
- constructor返回Created但submit隐式start：隐藏线程创建/异常和Finalization竞态。
- restart Stopped Runtime：与Reaper registration、Task identity和finalization单调性冲突。
- Scheduler move-only：应用跨组件共享必须另包shared_ptr并重复last-owner问题。
- finalization后构造一个Stopped Scheduler：把创建失败延迟成每次submit失败。
- startup失败detach部分Worker：违反生命周期与资源安全。

### Consequences

- D-087的`SubmissionError::NotRunning`不进入最终stable enum；有效Scheduler从不处于该public状态，空Handle是logic_error。最终enum为`Stopping/Stopped/CapacityExhausted`，startup rejection使用独立creation error。
- Public SchedulerState/示例/状态图删除Created/start。
- Fault-injection测试覆盖每个startup阶段及并发begin_finalization registration race。

### Non-goals and deferred risks

- 不提供异步Scheduler创建或lazy worker launch。
- 不允许运行时改变worker count或options。
- 不承诺Scheduler复制/最后释放lock-free或固定时延。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受constructor-as-start transaction、copyable shared Scheduler Handle与无public start/restart。
- Code or data evidence: 总设计start图与直接submit示例冲突；D-019要求pre-worker可失败准备；D-017/D-018使用last Scheduler Handle语义。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0043](../../docs/adr/0043-scheduler-construction-is-the-startup-transaction.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-156 — Scheduler Running publication 与 Finalization close 形成唯一启动竞态顺序

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-024要求已注册Starting Runtime观察sticky Finalization，D-155要求构造事务成功发布Running。Finalization可以在Runtime注册后、startup barrier完成前线性化；若构造仍开放短暂Running admission窗口就违反D-024，若任何稍后Finalization都追溯使已成功构造失败又没有单一顺序。

### Decision

Scheduler startup transaction的`Running publication`与`begin_finalization`永久关闭registration的线性化点必须形成唯一全序：

- Finalization close先于Running publication：即使Runtime registration记录更早建立，startup必须观察sticky request，在开放任何External Submission或用户Task前中止，停止/join所有barrier内Worker、撤销registration/handoff并让constructor抛`scheduler_creation_rejected(FinalizationStarted)`；Finalization核算等待该rollback完成。
- Running publication先于Finalization close：constructor startup成功，Runtime属于Finalization核算集合；close随后按D-024请求Graceful，故constructor返回前或调用方第一次观察时state已经Stopping是合法竞态结果。成功只保证曾线性化发布Running及返回有效Handle，不保证该瞬时状态保持到方法实际回程。

Running publication同时开放External admission gate并释放Worker startup barrier，不允许分成两个可被Finalization插入的步骤。Constructor线程不因close在Running之后发生而改为抛异常或销毁有效Handle；它返回共享Scheduler，后续submit按实际Stopping/Stopped rejection。

### Invariants

- 不存在Process Finalizing时仍开放External admission的新Runtime窗口。
- startup rollback Worker在barrier前不得执行用户Callable/coroutine。
- rollback registration只有在Reaper/Finalization accounting可观察其移除/完成后才算结束，不能让finalization提前Completed。
- Running-first路径不接收Finalization线性化后的External submission；D-003 gate close与请求顺序继续成立。
- constructor exception与successful Handle二者恰好一个，不返回Stopped/invalid Scheduler伪装成功。
- RuntimeId/registration gap在rollback允许且永不复用。
- Trace可记录StartupAborted或Running→Graceful，但不得同时记录同Runtime成功Running lifecycle和追溯删除；若Running先赢，它是真实历史。

### Scope and variants

| Race winner | Constructor | Runtime/finalization |
|---|---|---|
| Finalization close first | throw creation rejection | startup rollback counted to completion |
| Running publication first | return valid Handle | included, may already be Graceful Stopping |
| resource failure before either | original exception | rollback, no Running |

### Rationale

以Running publication为构造成功线性化点可复用D-024 sticky request并保持并发调用常识：方法返回前状态可被另一个线程合法推进，但不能追溯抹掉已经成功的状态转换。

### Rejected alternatives

- registration早就一律允许Running：Finalization后出现短暂submission窗口。
- Finalization只要在constructor return前发生就让构造失败：方法回程时刻不是稳定线性化边界。
- Starting Runtime直接返回Graceful Stopping Handle：破坏“构造成功即曾Running”并让无任务对象难解释。
- Finalization忽略Starting rollback：可能过早退出coordinator并UAF。

### Consequences

- Startup/registration control需要原子state machine或同一锁域协调close与Running publish。
- D-155文档中的“构造返回Running”解释为startup成功线性化，不是返回瞬间状态快照保证。
- Deterministic race tests在registration后、Running publication前插入begin_finalization。

### Non-goals and deferred risks

- 不保证constructor在Finalization竞态下无阻塞；rollback需要join已创建Worker。
- 不允许用户观察Starting或控制rollback。
- 不改变Finalization核算集合的有限性。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受Running publication与Finalization close全序，close-first回滚、Running-first成功后Graceful。
- Code or data evidence: D-024要求Starting观察sticky request；D-155选择constructor startup transaction。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0043](../../docs/adr/0043-scheduler-construction-is-the-startup-transaction.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-157 — SchedulerOptions 只暴露稳定语义 knob 并用显式推荐 worker helper

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

总体设计的SchedulerOptions片段缺少已批准backpressure、Metrics与Trace字段，也没有worker_count默认/0语义。把active spin、deque capacity、priority weights等每个调优量都公开会冻结平台细节；用0暗示auto又使非法零与自动选择混淆。

### Decision

稳定配置值为：

```cpp
[[nodiscard]] std::size_t recommended_worker_count() noexcept;

struct SchedulerOptions {
    std::size_t worker_count = recommended_worker_count();
    std::size_t external_pending_capacity = 65536;
    ExternalBackpressure external_backpressure = ExternalBackpressure::Reject;
    std::size_t max_helping_depth = 64;
    std::size_t local_burst_limit = 64;
    std::size_t steal_probe_limit = 8;
    MetricsLevel metrics_level = MetricsLevel::Basic;
    std::shared_ptr<TraceCollector> trace_collector{};
};
```

`recommended_worker_count()`取一次`std::thread::hardware_concurrency()`提示，返回0时fallback为1，始终返回至少1；它不是CPU quota/affinity保证。用户显式设置的`worker_count == 0`、`external_pending_capacity == 0`、`max_helping_depth == 0`、`local_burst_limit == 0`或`steal_probe_limit == 0`均在D-155 startup分配/注册前抛`std::invalid_argument`。未知enum值同样拒绝。Scheduler构造复制/移动捕获options快照；调用方随后修改原对象或shared_ptr变量不改变Runtime配置，Collector对象本身按D-138共享。

Active backoff pause/yield次数、Priority 8:4:2:1、deadline burst 8、Chase-Lev initial/grow capacity、timer heap internals、Metrics histogram buckets和notification batching不进入SchedulerOptions；它们由稳定决策固定或保持内部benchmark-tuned。新增public knob必须证明不同值需要用户策略控制，而非只是实现调优。

### Invariants

- `SchedulerOptions{}`始终产生非零worker_count，即使hardware_concurrency报告0。
- 默认helper每次调用可随当前环境提示不同，不成为跨机器determinism保证；artifact记录最终resolved options。
- option validation先于Reaper registration、RuntimeId publication和Worker creation。
- `external_pending_capacity`只约束D-083 External Pending，不是总内存/queue容量。
- backpressure Block的Worker caller restriction不因option改变。
- TraceCollector为空表示Trace disabled；Metrics Off与Trace独立。
- Runtime snapshot/benchmark metadata报告resolved immutable options而非调用方对象地址。

### Scope and variants

| Setting | Public/tunable |
|---|---|
| workers/external capacity/backpressure | yes |
| helping/local burst/steal probe bounds | yes |
| Metrics level/Trace collector | yes |
| priority weights/deadline burst | fixed stable policy |
| spin/deque/timer internals | private benchmark-tuned |
| dynamic worker scaling | unsupported |

### Rationale

小而完整的options surface覆盖资源边界、已承诺公平数值和观测成本；显式helper避免0的双重含义。把纯实现knob留在内部允许跨平台优化而不制造source compatibility负担。

### Rejected alternatives

- `worker_count=0`表示auto：和非法零/零Worker语义混淆。
- worker_count无默认：最小示例冗长且总体示例不成立。
- external capacity 0合法：没有任何普通方式seed Internal work，产生不可用Runtime。
- 全部性能参数公开：配置爆炸且用户无法合理选择。
- 运行中修改options：需要动态Worker/queue reconfiguration和口径混合。
- Trace用raw pointer：lifetime不安全；shared Collector是已批准capability。

### Consequences

- Public options、docs、CMake examples和artifact metadata使用同一字段集合。
- Tests覆盖hardware_concurrency=0 seam、每个zero/invalid enum和options mutation after construction。
- 未来新增NUMA/affinity/dynamic scaling需要新的架构决策，不在generic map中偷渡。

### Non-goals and deferred risks

- 不保证recommended_worker_count最适合容器quota、SMT或混合负载。
- 不支持环境变量隐式覆盖public options。
- 不首发custom allocator/PMR配置。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受精简稳定SchedulerOptions、显式recommended worker helper和零值前置拒绝。
- Code or data evidence: D-078/D-084/D-091/D-093/D-135/D-138已分别定义字段；总体Options片段不完整。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0044](../../docs/adr/0044-scheduler-options-expose-policy-not-implementation-tuning.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-158 — TraceOptions 固定有界默认容量与默认 category 集

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-138要求预分配固定容量并在overflow时drop，但没有字段/default，导致实现无法预算多Worker capture内存，测试也无法稳定触发loss。把所有steal attempt默认开启会在空闲/高争用Runtime迅速耗尽buffer并掩盖更有价值的生命周期事件。

### Decision

公开bitmask `TraceCategory`与：

```cpp
struct TraceOptions {
    std::size_t events_per_worker = 16'384;
    std::size_t external_control_events = 65'536;
    std::size_t events_per_reaper_producer = 4'096;
    TraceCategory categories = TraceCategory::Default;
};
```

`Default`启用Task lifecycle、queue/claim/steal-success、Wait/Await、Coroutine、Graph、Timer/Deadline和Runtime/Reaper lifecycle；逐次`StealAttempt`及其他明确标记Verbose的高频事件默认关闭，用户可显式加入。三个capacity必须大于0，未知category bit或checked总buffer大小溢出在`start_capture`任何状态改变前抛`std::invalid_argument`/`std::length_error`；allocation failure保持Collector Stopped及上一份已完成snapshot有效并抛原`std::bad_alloc`。

`TraceCollector::start_capture(TraceOptions options = {})`在所有当前registered producers所需buffer成功预分配后才发布新Recording generation。Recording期间新Scheduler附加同Collector时，必须在其D-155 startup/Worker barrier前为全部producer预留buffer；失败使Scheduler startup rollback，不能悄悄让该Runtime部分无buffer。capacity按event slots而非bytes定义，Snapshot记录实际event record size、每producer capacity、category mask和loss。

### Invariants

- capacity 乘producer/event-size使用checked arithmetic，不wrap/overcommit后越界。
- Default不等于All；关闭Verbose不是drop，metadata必须显示category mask。
- Worker数量增加只在Scheduler startup可失败阶段扩展Collector注册，不在Worker emit热路径分配。
- 同Collector多Runtime各Worker获得独立producer buffer；external/control buffer为Collector capture共享有界MPMC入口。
- Reaper producer容量按实际注册的collector/coordinator source建立，不能借Worker buffer写多producer数据。
- `start_capture`强异常安全：失败不清除上一Stopped snapshot或进入半Recording。
- 用户增大容量承担内存成本，Runtime不自动扩容或阻塞等待空间。

### Scope and variants

| Source/category | Default |
|---|---|
| each Worker buffer | 16,384 events |
| shared external/control | 65,536 events |
| each Reaper producer | 4,096 events |
| Task/Wait/Coroutine/Graph/Timer/Runtime | enabled |
| steal success | enabled |
| every steal attempt / verbose | disabled |

### Rationale

默认每Worker容量在常见event record大小下保持可控，同时共享入口能容纳外部burst；高频attempt显式opt-in保护因果主线。slots而非bytes让schema实现可报告真实内存并保持API简单。

### Rejected alternatives

- 无默认由实现任意选择：跨版本测试与内存不可预测。
- 默认All：steal storm迅速造成loss。
- capacity 0表示unbounded/disabled：双重魔法语义且可能无界。
- 自动grow：emit/capture期间分配和内存失控。
- 新Runtime附加失败但继续无buffer：Trace completeness scope难解释。

### Consequences

- Trace docs给出`workers × capacity × sizeof(TraceEvent)`预算示例，artifact记录实际值。
- loss tests可用很小非零capacity确定触发；standard tests用足够容量要求zero drop。
- 若长期数据表明默认不合适，修改属于documented behavior/config default change。

### Non-goals and deferred risks

- 不承诺默认容量适合所有长capture。
- 不提供按EventKind独立动态quota或compression。
- 不首发streaming drain来复用slot。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受16K/64K/4K trace容量、Default非Verbose categories与强异常安全start_capture。
- Code or data evidence: D-138只固定bounded/drop策略而未给public TraceOptions数值。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0039](../../docs/adr/0039-trace-capture-is-bounded-and-exported-offline.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-159 — 受支持进程只加载一个 AstraScheduler implementation instance

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-021称Reaper为process-wide，D-144又默认static library。若同一个static AstraScheduler archive分别链接进多个DSO/plugin，每个映像会拥有独立内部singleton、Finalization gate、ID allocator和coordinator，语言层无法自动合并成真正进程唯一服务；跨CRT传递public C++对象也可能不安全。

### Decision

受支持部署要求每个OS进程恰好加载一个AstraScheduler implementation instance：普通可执行程序可直接链接单份static library；若多个DSO/plugin都要调用AstraScheduler，它们必须共同依赖并解析到同一个exact-version shared AstraScheduler library instance。把static archive复制进两个或更多动态模块、加载同库多个私有namespace/copy，或跨不匹配CRT/stdlib边界传递Scheduler/TaskHandle/GraphRun属于unsupported configuration。

“process-wide Reaper/Finalization”、RuntimeId/TaskId进程内唯一和`process_metrics_snapshot()`均以该single-instance supported precondition为范围。`begin_finalization()`只控制调用落入的implementation instance；库不尝试通过OS named mutex、shared memory、symbol interposition或跨DSO registry发现/合并非法重复实例。Shared library unload继续要求D-038真实Finalization Completion且所有Astra对象已释放。

### Invariants

- package文档分别给出single-executable static与multi-DSO shared topology。
- CMake exported shared target使用visibility/export和exact-version/toolchain checks，不能让plugin各自fallback vendored static copy而不警告。
- static duplicate instances之间即使类型名/版本相同，也不承诺Handle互操作、共同finalization或ID uniqueness。
- Runtime不得用进程全局OS资源强行协调未知其他copy，避免版本/卸载deadlock。
- Benchmark/test声称process-wide行为时必须确认single implementation instance。
- single shared instance仍遵循D-145无跨版本/toolchain ABI保证。

### Scope and variants

| Link topology | Support |
|---|---|
| executable + one static Astra copy | supported |
| executable/DSOs → one shared Astra instance | supported exact-version |
| two DSOs each embed static Astra | unsupported |
| multiple private loader namespaces/copies | unsupported |
| cross-CRT/stdlib Handle transfer | unsupported |

### Rationale

C++ static linkage的singleton作用域是link image而非抽象OS process。明确single-instance deployment诚实维护process-level契约；多DSO用户使用单shared library即可获得真正共享控制面，而无需设计脆弱跨版本IPC。

### Rejected alternatives

- 宣称static多DSO仍process-wide：事实不成立。
- OS named singleton/共享内存合并：复杂版本、异常、指针和unload协议。
- 全面禁止shared或plugin使用：不必要限制受控单实例拓扑。
- 每个实例各自finalize但仍允许跨实例Handle：ownership/allocator/CRT不安全。
- 仅README footnote不进spec：部署边界会影响安全与Finalization语义。

### Consequences

- Release/package smoke增加multi-DSO single-shared-instance示例；duplicate-static只作为unsupported diagnostic文档。
- D-153的进程唯一ID承诺成立于Supported Configuration single-instance前提。
- 应用plugin架构在链接策略阶段就必须选择共享Astra runtime。

### Non-goals and deferred risks

- 不提供跨进程/跨instance scheduler federation。
- 不检测所有自定义loader namespace或静态复制情况。
- 不承诺不同Astra major共享同一进程。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受single implementation instance作为process-wide Reaper/ID/metrics的支持前提。
- Code or data evidence: D-021定义process-wide singleton；D-144默认static/shared可选；C++ static library在多个DSO中会形成独立内部状态。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0041](../../docs/adr/0041-releases-guarantee-source-api-not-cross-toolchain-abi.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-160 — Scheduler status 以 state 与 shutdown mode 的成对快照公开

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

生命周期由SchedulerState与ShutdownMode两个维度组成。若只给独立`state()`/`shutdown_mode()`，调用方连续读取时可能跨越Running→Stopping或Graceful→Immediate，拼出从未同时存在的组合；完全不公开则Metrics外无法轻量观察共享Handle状态。

### Decision

公开trivially-copyable：

```cpp
struct SchedulerStatus {
    SchedulerState state;
    ShutdownMode shutdown_mode;
};

[[nodiscard]] SchedulerStatus Scheduler::status() const;
```

`status()`在Runtime lifecycle同步域内返回一次线性化、非阻塞、无副作用快照；合法组合为`Running+None`、`Stopping+Graceful/Immediate`、`Stopped+Graceful/Immediate`，Stopped保留导致最终停止的最后模式。D-155 startup不暴露Created/Starting。空/moved-from Scheduler在读取全局状态前抛`std::logic_error`；有效Handle的status不等待Shutdown Completion、不Helping、不触发start/shutdown或Metrics capture。

稳定core不再提供可能鼓励check-then-act的`is_running()`、`is_stopped()`或独立mode getter；调用方可检查snapshot字段，但submit/shutdown仍必须依赖自身原子协议并处理竞态。调用返回后状态可立即单向推进，旧snapshot不成为admission capability。

### Invariants

- 同一次status不返回`Running+Graceful`、`Stopping+None`等撕裂组合。
- Graceful→Immediate升级后新snapshot不再返回Graceful；旧值允许已经持有。
- Stopped publication仍只在Worker join/Shutdown Completion边界后，status不得提前伪造。
- 多Scheduler副本观察同一Runtime lifecycle；Handle复制不产生独立status。
- status实现不能获取会与Worker join/user task形成锁环的长锁。
- Metrics Snapshot中的state/mode可复用同一status读取，但其余metrics仍是fuzzy。
- status不替代FinalizationControl/process metrics状态。

### Scope and variants

| Snapshot | Meaning |
|---|---|
| Running, None | admission gate may be open at linearization point |
| Stopping, Graceful | draining closure |
| Stopping, Immediate | no first-start; cooperative stop active work |
| Stopped, Graceful/Immediate | completed with retained final mode |

### Rationale

成对snapshot是最小且不撕裂的diagnostic interface。避免便利bool方法可以提醒用户：观察Running后submit仍可能因并发shutdown拒绝，真正的原子结果来自submission API。

### Rejected alternatives

- 独立state/mode getters：可能组合出不存在的状态。
- `is_running()`作为submit预检查：经典check-then-act竞态。
- status等待稳定/完成：把纯观察变成同步操作。
- 暴露Starting/Created：违反D-155 constructor transaction。
- Stopped把mode重置None：丢失最终策略诊断。

### Consequences

- Public lifecycle examples使用`status()`；tests枚举唯一合法pair和并发单调性。
- SubmissionError仍是调用方恢复依据，status只辅助诊断/UI。
- Trace/Process Metrics可报告相同state/mode vocabulary。

### Non-goals and deferred risks

- 不提供订阅state变化的callback或condition wait。
- 不保证snapshot跨多个Runtime事务一致。
- 不公开内部Starting/JoinReady阶段为SchedulerState。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目owner授权当前窗口采用推荐方案；审计后接受state+mode线性化SchedulerStatus并拒绝独立check-then-act getters。
- Code or data evidence: 总设计§23定义两个维度却没有查询API；D-012定义mode单向升级，D-155删除Created。仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0045](../../docs/adr/0045-scheduler-status-is-a-paired-snapshot.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-161 — Graph 节点公共标识统一为 graph-local NodeId

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-105、D-137、D-139 与 D-153 已使用 `NodeId` 表达 Graph 定义内的 insertion identity，但 D-112、D-123 的早期文字曾写作 `GraphNodeId`。若把两者解释为不同公共类型，Graph builder token、validation witness、GraphReport failure entry、TraceEvent 和 Coroutine Node 绑定将产生没有语义收益的转换与命名分叉。

### Decision

稳定公共类型只保留 trivially-copyable 强值类型 `NodeId`；D-112 与 D-123 中的 `GraphNodeId` 是同一概念的早期拼写，后续 spec、API、测试与文档一律规范化为 `NodeId`，不声明 `GraphNodeId` alias 或第二种类型。`TaskGraph::emplace(...)` 与 `emplace_coroutine(...)` 返回所属 builder 的 `NodeId`；`add_edge(NodeId from, NodeId to, ...)` 只接受同一 builder 产生的 token，foreign token 由 D-105 拒绝。

`NodeId` 的 default 值 0 为 invalid，合法值是 graph 定义内按插入顺序分配的 nonzero checked monotonic sequence；支持 `valid()`、equality、order/three-way 与 `std::hash`，不隐式转换为整数、索引或指针。值耗尽时 node insertion 在修改 builder 前抛 `std::overflow_error`，不 wrap/reuse，允许因失败留下 sequence gap。NodeId 仅在所属 graph 定义/GraphRun context 内有身份意义；完整运行节点关联仍是 `GraphRunId + NodeId + TaskId`。

### Invariants

- 同一 builder 内不同 Node 的 NodeId 不相等且插入顺序稳定；freeze 不重编号。
- 不同 graph 中数值相同的 NodeId 不代表同一 Node；跨 graph 身份必须携带 GraphRunId 或定义上下文。
- cycle witness、GraphReport failure entries、TraceEvent 与 diagnostic formatting 复用同一 `NodeId` 类型。
- Graph coroutine Node 绑定同一 NodeId 和 TaskId，不生成额外 graph-node identity。
- NodeId 不授予跨 graph lookup/control，也不延长 builder、FrozenGraph 或 GraphRun lifetime。
- invalid NodeId 不能用于成功 add_edge、report entry 或已 admission Node 的 TraceEvent。

### Scope and variants

| Context | Stable identity |
|---|---|
| Graph definition node | `NodeId` |
| Validation witness | ordered `NodeId` sequence |
| Running node | `GraphRunId + NodeId + TaskId` |
| Historical `GraphNodeId` spelling | documentation synonym only; no public symbol |

### Rationale

单一 graph-local 强类型使 builder、validation、runtime report 与 observability 使用同一词汇，同时通过 GraphRunId/上下文明确作用域，避免伪造 process-global Node identity。

### Rejected alternatives

- 同时公开 `NodeId` 与 `GraphNodeId`：制造无意义转换和 overload 歧义。
- 公开 `using GraphNodeId = NodeId`：永久保留重复词汇并扩大 source API。
- 让 NodeId process-global：增加热路径全局协调，且与 GraphRun execution identity 重复。
- 使用 `size_t`/vector index：暴露存储布局，invalid 与 foreign token 无法可靠区分。

### Consequences

- D-112 的 report 排序与 failure entry、D-123 的 Coroutine Node 绑定在正式 spec 中改写为 `NodeId`。
- Graph API 测试覆盖 invalid、foreign、stable insertion order、checked exhaustion 与跨 Graph 数值碰撞。
- 文档和示例不得再引入 `GraphNodeId` 公共名称。

### Non-goals and deferred risks

- 不公开按 NodeId 查询任意 live Node 的 Runtime registry。
- 不承诺 NodeId 跨不同 Graph 定义或进程重启唯一。
- 不在首版提供用户自定义 Node 名称/字符串 label。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 接受审计推荐，将 Graph Node 公共标识统一为 `NodeId`，旧 `GraphNodeId` 仅作为历史文字处理。
- Code or data evidence: D-105/D-137/D-139/D-153 已使用 NodeId，而 D-112/D-123 出现 GraphNodeId；仓库当前没有实现代码，适合在 spec 前消除命名分叉。

### Traceability

- ADR: [ADR-0030](../../docs/adr/0030-task-graphs-are-validated-single-shot-executions.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-162 — SchedulerCapabilities 精确报告实际 Local Deque backend

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-144 要求 `Scheduler::capabilities()` 报告 `lock_free_local_deque`，但单个 bool 无法区分 v0.1 尚未启用 Local Queue、v0.2/portable fallback 使用 locked queue、以及 v0.3 实际启用 lock-free Chase-Lev。若 capability 只按版本或类型名推断，benchmark artifact 和诊断会把未运行的算法错误标记为 lock-free。

### Decision

公开稳定枚举 `enum class LocalDequeBackend : std::uint8_t { None, Locked, ChaseLevLockFree };`，以及不可由用户 aggregate-initialize 的 trivially-copyable 值类型 `SchedulerCapabilities`。它至少提供：

```cpp
[[nodiscard]] LocalDequeBackend local_deque_backend() const noexcept;
[[nodiscard]] bool lock_free_local_deque() const noexcept;
```

其中便捷 bool 当且仅当 backend 为 `ChaseLevLockFree` 时返回 true。有效 `Scheduler::capabilities() const` 返回该 Runtime 在 startup transaction 中解析并冻结的 immutable snapshot；查询非阻塞、无副作用、不等待 Worker，Stopping/Stopped 后保持原值。空/moved-from Scheduler 在读取 Runtime 前抛 `std::logic_error`，因此外层 `capabilities()` 不标记 noexcept。

backend 必须报告实际执行路径：v0.1 Global-only 为 `None`；v0.2 locked Work-Stealing 为 `Locked`；v0.3+ 只有生产 Local Deque 的所有正确性所需原子/布局条件满足且实际选择 Chase-Lev 时才为 `ChaseLevLockFree`，否则为 `Locked`。Metrics、Trace metadata 与 Benchmark artifact 复制同一枚举值并可派生 `lock_free_local_deque`，不得各自重新探测。

### Invariants

- capability snapshot 在一个 Runtime 生命周期内不可变化，不受 shutdown/finalization 影响。
- `ChaseLevLockFree` 必须意味着正常 Local push/pop/steal backend 不获取互斥锁；resize allocation 与非热路径不改变该标志含义。
- locked fallback 保持 D-092 的 endpoint semantics，capability 变化不允许改变 Task/Shutdown/Outcome 契约。
- `None` 不等价于 locked；它明确表示该里程碑/runtime 没有 per-Worker Local Deque。
- capability 是描述而非控制接口；调用方不能请求运行时切换 backend。
- 新增 capability 必须以向后兼容 accessor/枚举策略演进，不允许用户依赖 public struct layout。

### Scope and variants

| Actual runtime path | Reported backend | lock_free_local_deque |
|---|---|---|
| v0.1 Global FIFO only | None | false |
| locked Work-Stealing / fallback | Locked | false |
| validated production Chase-Lev | ChaseLevLockFree | true |

### Rationale

枚举把算法存在性与 lock-free 属性分开，immutable per-Runtime snapshot 又确保工具记录的正是该次执行路径。accessor 型值对象避免未来为新增 capability 破坏 aggregate source/layout 使用。

### Rejected alternatives

- 只返回 bool：无法区分 absent 与 locked fallback。
- 根据项目版本推断：同版本可因平台 atomic capability 选择不同 backend。
- 每次查询重新执行 atomic lock-free probe：可能与 startup 实际选路不一致。
- 允许运行中切换 backend：使 queue ownership、quiescence 和 benchmark 解释显著复杂化。

### Consequences

- v0.1、v0.2、v0.3 的 integration tests 分别断言 None、Locked 与平台相关的最终 backend。
- Benchmark artifact 和 Trace capture metadata 记录规范枚举名/数值及派生 bool。
- README 的 lock-free claim 必须附 capabilities/benchmark artifact 证据。

### Non-goals and deferred risks

- 不承诺整个 Runtime lock-free；该能力只描述 Local Deque backend。
- 不公开具体 atomic instruction、memory-order 或 resize 次数为 capability。
- 不允许 capability 绕过 unsupported platform policy。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 接受审计推荐，以实际 backend 枚举而不是模糊 bool/版本推断表达 Local Deque capability。
- Code or data evidence: D-001、D-097 至 D-103 和 D-144 分别定义 Global baseline、locked oracle/Chase-Lev/fallback 与报告要求；仓库当前没有实现代码。

### Traceability

- ADR: [ADR-0041](../../docs/adr/0041-releases-guarantee-source-api-not-cross-toolchain-abi.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-163 — TraceCapture 显式 stop 产出 Snapshot，活动析构只中止并丢弃

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-138 固定了 `TraceCapture` 为 move-only active-capture capability，但把“未 stop 析构时丢弃还是保留可取结果”留成实现选择。若析构后仍保留隐式 snapshot，调用方没有明确对象取得它且下一次 capture 的清理语义含混；若析构不停止 recording，Collector 会永久拒绝下一次 `start_capture()`。

### Decision

`TraceCapture` 是显式事务守卫，具有 empty/moved-from、Recording、Stopped 三种对象状态。公共 `TraceSnapshot TraceCapture::stop()`：首次调用按 D-138 线性化关闭该 capture、等待已进入的固定 emit 临界区退出并返回 immutable snapshot；同一非空 Capture 重复/并发 stop 共享一次 stop transaction，每次返回引用同一 immutable backing state 的可复制 `TraceSnapshot`。`TraceSnapshot` 自身不拥有 Collector 的控制权，复制只共享只读数据，可在 Collector 和 Runtime 销毁后继续离线导出。

活动 `TraceCapture` 析构执行 noexcept abort：与 stop 使用同一 disable/quiescence 协议，禁止新事件并等待已进入 emitter 退出，然后丢弃该 generation 的全部 event data/loss result，使 Collector 回到 Stopped；它不构造、缓存或暴露隐式 Snapshot。Stopped 或 empty/moved-from Capture 析构为 no-op。对 empty/moved-from Capture 调用 `stop()` 在任何 Collector 副作用前抛 `std::logic_error`；`valid()`/`recording()` 提供无副作用对象状态查询。

### Invariants

- 每个 capture generation 恰好由 explicit stop 或 destructor abort 关闭一次。
- stop 与 destructor 不得并发作用于同一个 C++ 对象；对象 reassociation/destruction 需调用方同步。
- abort 完成后同一 Collector 可启动下一代 capture，上一代 late emitter 不能写入新代。
- explicit stop 返回的全部 Snapshot 副本观察相同 event/loss/options/origin 数据。
- abort 不执行 JSON export、文件 I/O、用户 callback、日志或 heap allocation。
- stop/abort 可因被 OS 挂起的已入 emit 临界区而延迟，不承诺硬实时上限，也不等待 Task/Runtime completion。

### Scope and variants

| Capture action | Collector result | Data result |
|---|---|---|
| explicit first stop | Stopped | immutable Snapshot |
| repeated stop | Stopped | same backing Snapshot |
| active destructor | Stopped | generation discarded |
| moved-from destructor | unchanged | no-op |

### Rationale

显式 commit/隐式 abort 与常见事务守卫一致：只有调用方明确停止才声明一次测量有效，忘记 stop 不会永久占用 Collector，也不会制造无处领取的隐式结果。共享 immutable backing 让幂等 stop 与离线多消费者兼容。

### Rejected alternatives

- 析构隐式 stop 并缓存结果在 Collector：下一代 start 的覆盖/领取协议复杂，且忘记 stop 看似成功。
- 析构不结束 capture：Collector 可能永久卡在 Recording。
- Snapshot move-only：重复 stop、多导出器与并发分析需要额外用户封装。
- destructor detach emitter 后立即复用 buffer：late write 可污染新 generation 或形成 UAF。

### Consequences

- 示例必须显式保存 `auto snapshot = capture.stop()` 后再导出。
- tests 覆盖 explicit stop、重复 stop、move、active destructor abort、late emitter generation isolation 和 Snapshot 独立 lifetime。
- benchmark 中 trace run 若 Capture 被异常展开析构，该次测量被丢弃而 Collector 可复用。

### Non-goals and deferred risks

- 不提供 abort 后恢复或读取 partial snapshot 的接口。
- 不让 TraceCapture 析构自动写 Chrome JSON。
- 不把 `TraceSnapshot` 作为可靠审计日志；仍受 D-140 loss 语义约束。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 接受审计推荐，以 explicit stop/implicit abort 固定 TraceCapture RAII 与 Snapshot lifetime。
- Code or data evidence: D-138 已要求 move-only Capture、可重复 generation、stop quiescence和析构noexcept，但保留了丢弃/保留分支；D-140只允许Stopped Snapshot导出。

### Traceability

- ADR: [ADR-0039](../../docs/adr/0039-trace-capture-is-bounded-and-exported-offline.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-164 — Header 与已链接 Library 使用独立无副作用 Version 查询

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-145 要求公开项目版本宏与可查询 version value，但未区分编译 consumer 时看到的 header 版本和进程实际链接/加载的 compiled library 版本。只提供宏无法诊断手工链接错误，只提供动态函数又不能用于 compile-time feature/version guard。

### Decision

公开 trivially-copyable aggregate value：

```cpp
struct Version {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
    friend constexpr auto operator<=>(const Version&, const Version&) = default;
};

[[nodiscard]] constexpr Version header_version() noexcept;
[[nodiscard]] Version library_version() noexcept;
[[nodiscard]] std::string_view library_version_string() noexcept;
```

`header_version()` 由同一 public header 内 `ASTRA_VERSION_MAJOR/MINOR/PATCH` 十进制宏构造，适用于 constant evaluation；`library_version()` 与 `library_version_string()` 是 compiled library 导出符号，返回该 binary 的版本。string view 指向进程期静态只读存储，包含规范 SemVer 字符串并可带 prerelease/build metadata，调用方不得释放；结构化 Version 只表达 compatibility core 三元组。

三个查询均不读取/创建 Scheduler、Runtime State、Reaper、Metrics或Trace，不分配、不加锁、不失败。CMake package/config 的 exact-version/toolchain 检查仍是受支持配置的主要 mismatch 防线；手工绕过 package 时可比较 header/library value 诊断，但库不在任意 API 首次调用时自动 terminate，也不承诺错误组合可安全继续。

### Invariants

- 同一次安装产物的 header_version 与 library_version 三元组必须完全相等。
- `library_version_string()` 在进程生命周期内地址和内容稳定，且其三元组与 library_version 一致。
- 版本查询在 Finalization 前后结果相同，不启动任何线程或服务。
- schema versions 不复用项目 Version；Metrics/Trace/Benchmark 继续独立演进。
- 预发布 build 的 SemVer precedence 不能只靠 Version 三元组判断；需要时使用规范字符串/构建 metadata。
- `ASTRA_VERSION_*` 只表示 header，不得被 benchmark artifact 当成已链接 binary 版本。

### Scope and variants

| Surface | Reports | Evaluation |
|---|---|---|
| ASTRA_VERSION_* | included headers | preprocessor |
| header_version() | included headers | constexpr C++ |
| library_version() | linked binary | runtime, side-effect-free |
| library_version_string() | exact SemVer text | runtime, static view |

### Rationale

分开报告 header 与 binary 让正常 CMake 用户尽早拒绝 mismatch，也为手工集成、benchmark artifact 和崩溃诊断提供事实来源，而不把版本检查耦合进 Runtime lifecycle。

### Rejected alternatives

- 只提供宏：无法知道实际加载 binary。
- 只提供 runtime 字符串：不能 constexpr 比较且容易错误解析。
- 每次 Scheduler 构造才检查：版本诊断不应依赖创建线程/Reaper。
- mismatch 时 library 自动 terminate：可能在更早的 ABI 边界已产生未定义行为，且破坏无副作用查询。

### Consequences

- CMake consumer smoke 校验 header/library version 相等，并单独测试故意 mismatch 在 configure/link 前失败。
- Benchmark artifact 同时记录规范 library version string 与 structured triple。
- release pipeline 从单一 version source 生成宏、CMake version file 与 library symbols。

### Non-goals and deferred risks

- 不提供跨版本 binary ABI compatibility negotiation。
- 不把 git dirty/hash 强制塞入 SemVer core；可作为 optional build metadata。
- 不用 version 查询替代 feature capability API。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 接受审计推荐，分别固定 header 与 linked-library 的无副作用版本查询。
- Code or data evidence: D-144确定compiled library/CMake package，D-145要求version macros、queryable value和exact-version shared policy；此前没有稳定函数签名。

### Traceability

- ADR: [ADR-0041](../../docs/adr/0041-releases-guarantee-source-api-not-cross-toolchain-abi.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-165 — submit 从 v0.1 起 decay-capture 并一次性 rvalue invoke move-only 工作

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

总设计早期允许 v0.1 用 `std::function<void()>`，但 D-075 已要求 move-only Result，D-089 讨论 Callable/参数 capture，D-104 又明确 Graph Node 支持 move-only Callable。若普通 submit 首版只接受 CopyConstructible target，后续替换 type erasure 会改变 overload viability、参数消费与失败副作用，破坏 D-146 的纵向稳定 Runtime substrate。

### Decision

从 v0.1.0 起，`submit/try_submit` 对 `F&&` 与 `Args&&...` 分别以 `std::decay_t` 按完美转发构造一次 owned capture；lvalue 因而复制，rvalue 因而移动，显式 `std::reference_wrapper` 保留调用方选择的引用语义。所有 stored component 必须可从对应 forwarded argument 构造、可析构，并且整个 work item/TCB 可由 Runtime 通过非抛出的 ownership transfer 持有；不要求 Callable、参数或最终 type-erased work CopyConstructible。

实际用户调用恰好一次，并按以下 stored-rvalue expression 判断 D-059 的 ordinary-first 规则与 D-074/D-075 result：

```cpp
std::invoke(std::declval<std::decay_t<F>&&>(),
            std::declval<std::decay_t<Args>&&>()...)
```

仅当该 ordinary expression 不可调用时，再测试/选择首参数注入 `std::stop_token` 的同一 stored-rvalue expression。Worker claim 后通过 move stored callable/arguments 执行；因此支持 `operator()&&`、`unique_ptr` 参数及其他 one-shot state。只提供 lvalue-qualified调用或需要真实引用参数的用户必须显式使用 `std::ref`/wrapper。capture construction 在 admission success 前执行并按 D-089 回滚；用户 invocation/result construction 异常按 D-045 发布 Exception Outcome。

内部实现可使用自定义 move-only type erasure、templated TCB 或其他表示，但稳定路径不得因使用 `std::function` 而拒绝合规 move-only work。SBO阈值、allocator与task object布局不是public contract。

### Invariants

- 成功 accepted work 对每个 owned capture 恰好构造一次、销毁一次，用户 invocation 最多一次。
- submission rejection/capture exception 不执行 Callable；被移动的调用方 rvalue 不承诺恢复原值。
- admission commit 后不得再执行可能抛出的用户 copy/move来完成queue publication。
- Internal/External routing、backpressure、TaskId与Outcome不因 Callable copyability改变。
- `std::ref(x)` 明确允许异步非 owning引用，`x` 的 lifetime/data-race责任仍由调用方承担。
- Graph普通`emplace`复用同一 decay-capture/one-shot invocation原则；Coroutine `Task` frame ownership仍服从D-114/D-115而不二次capture。

### Scope and variants

| Input | Stored/invoked behavior |
|---|---|
| copyable lvalue | decay-copy once, invoke stored rvalue once |
| move-only rvalue | move-capture once, invoke stored rvalue once |
| `std::ref(x)` | store wrapper; invoke referenced object |
| lvalue-only callable without wrapper | compile-time rejected |
| capture construction throws | original exception, no accepted Task |

### Rationale

类似 `std::thread` 的 decay-owned one-shot模型与异步生命周期、move-only现代C++所有权及强 admission transaction一致；它还让 invocation trait 与真实执行表达式完全相同，不依赖实现中的偶然lvalue调用。

### Rejected alternatives

- v0.1 `std::function<void()>`：强加copyability并迫使后续public行为迁移。
- 保存转发引用：submission返回后容易悬垂，ownership不可证明。
- stored callable总按lvalue调用：排除明确one-shot `operator()&&` 并与参数move消费不对称。
- capture失败后恢复用户rvalue：一般C++ move不可逆，无法保证。

### Consequences

- v0.1需要move-only work holder或等价templated execution state，而非copy-only queue payload。
- compile-time tests覆盖copyable/move-only/lvalue-only/ref-wrapper、ordinary-vs-token overload和result matrix。
- 文档删除“首版可以std::function”的建议，SBO仅作为后续内部优化。

### Non-goals and deferred risks

- 不提供通用 borrowed-lifetime验证。
- 不固定内联存储大小、allocator propagation或type-erasure ABI。
- 不保证用户 capture destructor 抛异常时恢复；析构异常遵循C++规则。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 接受审计推荐，从首个 Runtime 版本统一 move-only decay capture 与 one-shot rvalue invocation。
- Code or data evidence: D-059留空capture规则，D-075要求move-only result，D-089要求capture/admission强保证，D-104要求move-onlyGraph Callable；总设计§11仍有copy-only std::function旧建议。

### Traceability

- ADR: [ADR-0019](../../docs/adr/0019-submit-returns-task-handle-from-the-first-release.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-166 — 同 Runtime Worker 的同步 Scheduler shutdown 统一抛 logic_error

Status: accepted

Date: 2026-08-26

Supersedes: None

Superseded by: None

### Context

D-009 与 D-011 已要求同 Scheduler Worker 调用同步 `shutdown_now()`/`shutdown()` 在任何副作用前被拒绝，但未固定公共 C++ 表示。返回 bool 会改动现有 void 风格，专用 error type只承载一种不可恢复的调用上下文错误；与 Direct Self-Wait 和 Worker Finalization wait 使用不同异常又会扩大无价值的错误分类。

### Decision

公开 `void Scheduler::shutdown()` 与 `void Scheduler::shutdown_now()`。当前正在该 Scheduler Runtime 上执行 Task 的 Worker调用任一方法时，必须在读取/改变 lifecycle mode、关闭admission、发布stop、取消Task、认领join或进入等待前抛 `std::logic_error`。`what()` 文本只用于诊断，不是稳定程序化契约；调用方只依赖异常动态类型。

其他 Scheduler 的 Worker 对目标 Runtime 仍属于目标的非Worker调用方，执行既有同步完成语义；普通应用线程同样不受此异常分支影响。空/moved-from Scheduler 继续按D-155在任何Runtime读取前抛 `std::logic_error`，但“empty Handle”与“self-shutdown”不增加公开reason enum。

### Invariants

- caller identity检查先于所有关停副作用和Stopped幂等分支；同Runtime Worker即使目标已Stopped也不应存在正常执行上下文，但若内部测试构造该状态仍优先拒绝。
- 异常逃出用户Callable时按普通Task异常边界成为该Task的Exception Outcome。
- self-shutdown失败不创建/加入Shutdown Completion，不改变其他并发shutdown调用结果。
- 方法不标记`noexcept`；非Worker合法路径仍只在真实Completion后返回。
- 不提供从Worker发起的同义异步shutdown request API。

### Scope and variants

| Caller | Target Runtime | Result |
|---|---|---|
| same Runtime Worker | same Scheduler | throw `std::logic_error` before effects |
| other Runtime Worker | target Scheduler | legal nonWorker synchronous shutdown |
| ordinary thread | target Scheduler | legal nonWorker synchronous shutdown |
| empty Scheduler Handle | none | `std::logic_error` before Runtime access |

### Rationale

`logic_error`准确表达“同步join API从被join集合内部调用”的调用上下文违约，并与项目现有self-wait规则一致；不新增reason enum可保持surface最小。

### Rejected alternatives

- 返回bool/status：改变void同步完成契约并鼓励忽略错误。
- 静默异步请求：同一方法按caller改变返回保证。
- 专用`self_shutdown_error`：没有可恢复分支，不值得新增稳定类型。
- `std::terminate()`：用户Callable中的可诊断调用错误不应成为进程级不变量故障。

### Consequences

- API/compile tests固定两个方法为非noexcept void。
- runtime tests覆盖same/other Runtime Worker与普通线程caller矩阵及零副作用。
- 文档明确catch异常后当前Task可继续，未catch则形成Failed Outcome。

### Non-goals and deferred risks

- 不提供错误消息稳定性或错误码。
- 不检测跨Runtime同步shutdown形成的应用级等待环。
- 不新增Worker-safe异步per-Scheduler shutdown命令。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 已授权当前窗口采用推荐最终方案；审计后统一选择 std::logic_error 表达同Runtime Worker self-shutdown。
- Code or data evidence: D-009/D-011已固定副作用前拒绝，D-049/D-034/D-035对同类self-wait使用logic_error；仓库当前无实现代码。

### Traceability

- ADR: [ADR-0046](../../docs/adr/0046-self-shutdown-is-a-logic-error.md)
- Spec destinations: Pending
- Tickets: Pending
- Tests: Pending

## D-167 — Core 保持 compiled CMake library，最终支持平台收窄为 Linux-only

Status: accepted

Date: 2026-08-27

Supersedes: D-144

Superseded by: None

### Context

D-144 把 Linux x86_64 与 Windows x64 同列为 Tier-1，但项目 owner 随后明确 AstraScheduler 是 Linux Runtime，开发与最终交付都不再以 Windows/MSVC 为支持目标。由于 D-144 同时承载 compiled-library/package、平台 Tier 与 atomic fallback，本决策完整重述仍然保留的构建契约，并替换其平台矩阵，避免“Windows 可编译”被误读为受支持配置。

### Decision

AstraScheduler继续是需要C++20、线程与异常支持的compiled CMake library，公共target为`AstraScheduler::AstraScheduler`；public headers仅安装到`include/astra/`，默认static、显式`BUILD_SHARED_LIBS`可生成shared，tests/examples/benchmarks/tools继续由`ASTRA_BUILD_*`控制，core不要求RTTI且不支持`-fno-exceptions`。

最终Supported Configuration仅包含64-bit Linux：Tier-1为Linux x86_64上的GCC 13+与Clang 17+；Tier-2为native Linux AArch64上的GCC/Clang weak-memory验证。Windows、MSVC、macOS、其他非Linux OS及32-bit目标均为unsupported，不进入release gate，也不得在README、package metadata、benchmark artifact或`SchedulerCapabilities`中宣称受支持。非Linux上的偶然编译成功不产生兼容、正确性、性能或生命周期保证。

在受支持Linux平台上，若所需64-bit atomic capability不足，构建仍可使用Locked Local Deque semantic fallback；`Scheduler::capabilities()`与benchmark metadata必须报告实际backend，不能因版本、类型名或算法来源虚报lock-free。

### Invariants

- Tier-1 release build、unit/integration、package consumer与常规CI只以Linux x86_64 GCC/Clang为支持门禁。
- Tier-2必须使用native Linux AArch64定期执行weak-memory stress；只做cross-compile或QEMU编译不能替代该证据。
- 不生成或维护Windows/MSVC专用release job、package承诺、兼容层或平台抽象验收。
- CMake target声明`cxx_std_20`，不静默降级到非标准Coroutine扩展。
- public compile definitions不得传播warnings-as-errors、sanitizer或内部选项给consumer。
- static/shared继续使用相同public semantics与test suite，安装后必须通过独立Linux consumer的find_package/link/run。
- Locked fallback只改变backend capability/performance，不改变Task、Scheduler、DAG或Coroutine语义。

### Scope and variants

| Platform | v1.0 status |
|---|---|
| Linux x86_64 GCC 13+/Clang 17+ | Tier-1 supported/release-gated |
| Native Linux AArch64 GCC/Clang | Tier-2 supported/periodic weak-memory validation |
| Windows/MSVC | unsupported |
| macOS/other non-Linux OS | unsupported |
| 32-bit Linux or non-Linux | unsupported |

### Rationale

Linux-only范围与项目定位和实际开发环境一致，能把有限CI与并发验证预算集中到GCC/Clang、sanitizer和native AArch64 weak-memory证据，避免在没有持续MSVC/Windows测试时制造虚假Tier-1承诺。compiled target、package隔离和Locked fallback仍保留原有工程价值。

### Rejected alternatives

- 保留Windows/MSVC Tier-1但日常只在WSL开发：缺少持续本机构建与并发证据，Tier-1声明不诚实。
- 把Windows降为best-effort：项目owner已明确最终支持平台仅限Linux，best-effort仍会造成维护预期。
- 改成header-only以追求跨平台：暴露实现、增加ODR/编译成本且不解决未验证平台问题。
- atomic非lock-free就拒绝Linux构建：会不必要丢失Locked semantic fallback。

### Consequences

- 删除所有Windows/MSVC release要求，并修订Spec、ADR、总设计、里程碑与AST-052/AST-053等Tickets。
- CI矩阵聚焦Linux x86_64 GCC/Clang，另保留native Linux AArch64的定期Tier-2工作流。
- 非Linux兼容代码可被移除或不实现；未来若重新支持其他OS，必须新增决策、Spec规则、Tickets和持续验证资源。

### Non-goals and deferred risks

- 不承诺Windows、macOS、BSD、Android、freestanding、embedded或WebAssembly。
- 本决策不选择具体Linux发行版、包管理器或容器镜像。
- 不改变D-159的single implementation instance部署边界或D-145的非跨工具链ABI政策。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确确认“开发和最终支持平台都仅限 Linux”，并要求完成文档、规格与Tickets修订。
- Code or data evidence: 当前CI入口运行于ubuntu-latest；仓库当前没有Windows实现或MSVC验证证据。

### Traceability

- ADR: [ADR-0047](../../docs/adr/0047-linux-only-support-and-wsl-development.md)
- Spec destinations: R-101, R-110, R-111; R-092 historical supersession; Non-goals and Compatibility
- Tickets: AST-002, AST-004, AST-022, AST-027, AST-052, AST-053, AST-055（Linux-only范围修订已获批准）
- Tests: AST-052/AST-053实现Linux-only CI/package matrix、native AArch64 weak-memory与non-Linux support-claim audit

## D-168 — 本机开发、验证与基准命令统一在 WSL Linux 内执行

Status: accepted

Date: 2026-08-27

Supersedes: None

Superseded by: None

### Context

即使最终目标是Linux，Agent或开发者仍可能从Windows工作区误用PowerShell原生CMake、MSVC、Windows Ninja或混用同一build cache，产生与目标平台无关的结果。项目位于Windows `D:\code\cppStudy\AstraScheduler`，在本机WSL中对应`/mnt/d/code/cppStudy/AstraScheduler`，因此需要把“宿主机文件位置”和“开发命令执行环境”明确分开。

### Decision

所有项目开发命令——包括configure、build、unit/integration、format、lint、package consumer、sanitizer、stress、benchmark与release verification——必须在本机WSL的Linux用户空间内执行，canonical working directory为`/mnt/d/code/cppStudy/AstraScheduler`。Windows PowerShell/cmd只能用于启动WSL或非开发性的宿主编排，不得直接调用MSVC、Windows-native CMake/Ninja、测试二进制或benchmark作为项目证据。

WSL/Linux构建目录必须使用明确的Linux专用路径并与任何宿主原生产物隔离；不得在Windows native与WSL之间复用CMake cache、object、library、executable或sanitizer artifact。仓库级`AGENTS.md`和开发文档必须保存这些约束，使新Agent无需聊天上下文即可执行正确命令。

### Invariants

- 标准命令入口使用`wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && ..."`或已经位于WSL shell内的等价命令。
- 不把PowerShell中成功运行Python文档脚本当成Linux build/test证据；相应脚本也应从WSL运行。
- build目录命名必须明确属于WSL/Linux，例如`build/wsl-gcc-debug`、`build/wsl-clang-asan`。
- 不复用由Windows CMake generator创建的cache或binary directory。
- CI仍在原生Linux runner执行；WSL约束只描述本机开发入口，不把WSL本身变成发布运行时依赖。

### Scope and variants

| Environment | Allowed role |
|---|---|
| Local WSL Linux | 全部开发、构建、测试、sanitizer、stress、benchmark与package验证 |
| Native Linux CI/runner | release与持续验证证据 |
| Windows PowerShell/cmd | 启动WSL、宿主文件查看或非开发编排 |
| Windows-native compiler/build/test | 禁止作为项目开发或验证路径 |

### Rationale

统一执行环境能避免生成器/cache污染，并保证本机反馈与Linux-only支持矩阵一致。允许宿主shell只做WSL入口，使现有Windows桌面工具仍可协作，而不会把Windows本身提升为构建目标。

### Rejected alternatives

- 文档只写“推荐WSL”：无法阻止Agent继续产生Windows原生构建证据。
- Windows与WSL双构建：与Linux-only范围冲突并增加无收益维护矩阵。
- 在同一build目录切换generator/toolchain：CMake cache和产物不可安全复用。
- 要求所有源文件搬入WSL虚拟磁盘：用户已确认使用当前本机工作区；本决策只固定执行环境和隔离边界。

### Consequences

- 新增项目级`AGENTS.md`与Linux/WSL开发指南，并在release milestone文档中引用。
- 后续Ticket的RED/verification命令必须以WSL/Linux形式记录和执行。
- 本机性能数据必须标明WSL环境；最终release性能声明仍需按Benchmark规则记录完整环境并可在native Linux复核。

### Non-goals and deferred risks

- 不固定WSL发行版、WSL版本或宿主Windows版本。
- 不把WSL列为最终用户运行时依赖或Supported Configuration类型。
- 不定义开发容器、Nix或其他可复现环境方案。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: 项目 owner 明确要求所有开发使用本机WSL，并确认开发和最终支持平台都仅限Linux。
- Code or data evidence: 工作区宿主路径为`D:\code\cppStudy\AstraScheduler`，WSL drvfs映射约定对应`/mnt/d/code/cppStudy/AstraScheduler`。

### Traceability

- ADR: [ADR-0047](../../docs/adr/0047-linux-only-support-and-wsl-development.md)
- Spec destinations: R-112, Testing Decisions
- Tickets: AST-001（R-112门禁修订已获批准）
- Tests: `R112WslDevelopmentGateTests`覆盖WSL命令、开发文档与build-cache隔离

## D-169 — v1.1 收回误暴露实现并以深模块边界重构内部运行时

Status: accepted

Date: 2026-08-30

Supersedes: None

Superseded by: None

### Context

v1.0 的安装头文件同时承载了 documented public API 与实现桥接，导致 `astra::detail` 类型、`*_internal` 访问器、原始 coroutine frame handle、Graph 节点存储、Scheduler 测试注入函数和共享状态写入口都可被普通 consumer 直接调用。现有 API freeze 又按整文件哈希和全部 `astra` 动态符号冻结，把这些实现细节误分类为稳定接口，并曾直接重写 v1.0 golden manifest。结果是内部不变量跨越多个头文件和调用方，Scheduler、Graph 与 Coroutine 难以独立演进。

### Decision

v1.1 保持已文档化的 Scheduler、TaskHandle、TaskGraph/GraphRun、Task/awaiter、Metrics/Trace 的 source 与 observable semantics，不把 `astra::detail`、名称含 `_internal` 的入口、原始 coroutine handle、共享状态 mutator、测试注入入口及未文档化的 Graph 存储布局视为 public compatibility surface。上述实现桥接必须私有化或迁入非安装的 internal header。

API freeze 改为冻结明确维护的 documented public interface 与 consumer compile contract；已发布版本 manifest 不得被后续提交改写。TaskId 由 Runtime/Scheduler 状态分配，不能由 public header 中的进程静态原子生成。Graph 的运行状态机由内部 `GraphExecution` 深模块拥有，Scheduler 仅提供 admission、ready publication、timer 与 observability 所需窄桥接。测试注入只通过非安装的 `src/test_seam.hpp` 暴露，public consumer tests 不得拥有 `src/` include path。

### Invariants

- documented public 调用的返回值、异常、取消、等待、优先级、deadline、metrics 与 trace 语义保持不变。
- `tools/api_manifest/v1.0.0.json` 是不可变发布证据；v1.1 使用独立 manifest。
- public consumer 无法构造或修改 Task shared state、FrozenGraph node storage、GraphRun completion list 或 Scheduler fault injection state。
- TaskId 的唯一性、单调性与 overflow 行为继续满足 R-100，但分配所有权属于 Scheduler Runtime。
- GraphExecution 的依赖传播、取消、terminal publication 和 report snapshot 形成单一内部不变量边界。
- internal tests 可以使用私有 seam；public tests 只编译安装式 public headers 与 target。

### Rationale

稳定接口应描述 consumer 能依赖的能力，而不是偶然出现在安装头文件中的表示。把复杂协议放入拥有其状态的深模块，可减少朋友关系、重复 admission 回滚和跨模块可写状态，同时让 API freeze 对真正的兼容性变化敏感。

### Rejected alternatives

- 继续冻结整头文件哈希：任何私有实现调整都被当成 public breaking change，同时无法区分 accidental surface。
- 继续把所有 `astra` 动态符号当成 public API：模板桥接和隐藏不彻底的 detail 符号会永久污染兼容清单。
- 仅用注释声明 internal：consumer 仍能调用，编译器和测试无法执行边界。
- 一次性重写全部 Scheduler 实现：并发语义回归面过大；采用可独立验证的窄边界重构。

### Consequences

- 项目版本进入 v1.1.0，并产生新的 semantic API manifest。
- public header 中仍可能保留模板实现必需的 detail 声明，但它们不进入 documented compatibility allowlist，且不得提供可写控制入口。
- 现有直接依赖 accidental surface 的 consumer 源码可能无法继续编译；该表面从未被文档化或承诺。
- 重构按 API/test seam、Task identity/admission、GraphExecution、Scheduler internal decomposition 顺序实施并分别验证。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: project owner 要求按全项目封装性审查意见修改项目，并明确不要求使用额外 skills。
- Code evidence: `docs/封装性改进建议.md` 记录了 public/internal inventory、逃逸入口与目标边界。

### Traceability

- Spec destinations: R-113 through R-117
- Tickets: AST-057
- Tests: semantic API manifest、public consumer boundary audit、TaskId/admission tests、GraphRun state/cancellation tests 与全量 WSL build/test

## D-170 — 运行时协议类型以 compiled TaskControlBlock 离开 installed headers

Status: accepted

Date: 2026-08-30

Supersedes: None

Superseded by: None

### Context

D-169 / AST-057 已将误暴露入口改为 private，并允许 public header 因模板实例化仍保留 detail 声明。审查表明该例外使 `TaskSharedStateBase`、`AwaitHandshake`、invoker 模型与 metrics hook 仍作为 installed interface 的一部分：consumer 只要 include 即可命名并构造它们。`TaskHandle<T>::get()` 与 `co_await` 在 consumer 翻译单元实例化，因此只要协议类型的完整定义留在安装头中，1B（仅搬非模板到 src/）无法真正把协议移出 installed headers。

### Decision

本轮采用 compiled TaskControlBlock（方案 1A）：installed headers 只保留结果格（值 `T` / 异常 / 终态）以及公开 awaitable 的薄包装。mutex、完成回调、rescheduler、timer 注册、handshake 状态机与 invoker 执行协议编入库实现，不得再以完整类型出现在安装头中。

拒绝仅搬非模板协议到不安装 src 头作为本轮完成定义（1B），也拒绝只加 negative compile probes（1C）。1B 至多作为 1A 的中间提交，不构成完成标准。

第三方 Astra-aware awaiter 与 `astra::detail` 协议类型不属于 documented public surface；从安装头移除它们是收回 unsupported 入口，不是扩张 documented semantics。

本决策收紧 D-169 中「模板实现可保留 detail 声明」的例外，不整体取代 D-169。

### Invariants

- documented public 的 Scheduler / TaskHandle / Task / GraphRun / yield / sleep 可观察语义不变。
- package consumer 翻译单元不能完成类型 `astra::detail::TaskSharedStateBase`、`AwaitHandshake` 或等价协议类型。
- 任意 `T` 的 `TaskHandle<T>::get()` 仍可在 consumer 侧实例化，结果格留在安装头或通过类型擦除桥接，不得把整份运行协议放回安装头。
- `tools/api_manifest/v1.0.0.json` 仍不可改写。
- public tests 不得依赖被移出的协议类型；internal tests 可通过非安装头继续验证 handshake / TCB。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| documented consumer | installed public headers | 只见结果格与公开 awaitable |
| internal tests / library TU | 非安装 src 头与编译单元 | 可见 TaskControlBlock 与 handshake 协议 |
| 误用 astra::detail 的外部代码 | unsupported | 可能无法继续编译 |

### Rationale

结果格必须对任意 `T` 可见，运行协议不必。把协议编进库，才能让 installed interface 显著小于 implementation，并通过 deletion test。

### Rejected alternatives

- 1B 只搬非模板到 src/：`TaskSharedState<T>` 与 await_suspend 仍会把协议留在安装头。
- 1C 只加 probes：不增加 depth，删除探针后泄漏仍在。
- 把 `get()` 改为库内显式实例化白名单：无法支持 consumer 的任意 `T`。

### Consequences

- 共享库导出符号与 R-110 封闭集必须随协议内收而更新。
- AST-033/034 等 handshake 测试必须保持 internal，并改 include 非安装头。
- 版本号是否上调、公开 `operator co_await` 的 awaiter 类型名是否保留，由后续决策单独确定。

### Non-goals and deferred risks

- 不在本决策中深化 GraphExecution 或拆 Scheduler::Impl。
- 不在本决策中引入正式的第三方 awaiter 扩展 interface。
- 协程帧所有权与 resume 代际语义保持现有规则，只改变这些规则的存放位置。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: owner 在架构审查候选 1 的 1A/1B/1C 中明确选择 1A（compiled TaskControlBlock）。
- Code or data evidence: `include/astra/task_handle.hpp`、`include/astra/coroutine.hpp` 仍完整定义协议类型；`tools/api_manifest/public_contract.cpp` 不使用这些类型。

### Traceability

- ADR: [ADR-0048](../../docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md)
- Spec destinations: R-118, R-113, R-114
- Tickets: Pending
- Tests: Pending（encapsulation negative probes + 现有 TaskHandle/coroutine 行为测试）

## D-171 — 协议类型内收后项目 VERSION 升至 1.2.0

Status: accepted

Date: 2026-08-30

Supersedes: None

Superseded by: None

### Context

D-170 将改变 installed headers 中出现的类型以及 shared 库的 detail 导出集，但 documented public 可观察语义不变。CMake package 使用 exact-version：若 VERSION 仍为 1.1.0，会出现同一版本号对应两套安装面。D-169 已声明误用 `astra::detail` 的 consumer 不在兼容承诺内，因此这不是 documented source 的 major break。

### Decision

实施 D-170 时将 `project(AstraScheduler VERSION …)` 升到 **1.2.0**。consumer 模板的 `find_package` 钉住值随单一版本源更新。

不保持 1.1.0，也不升到 2.0.0。`tools/api_manifest/v1.0.0.json` 与 `v1.1.0.json` 均不可改写；1.2.0 使用新的 semantic manifest。

### Invariants

- documented public observable semantics 不因本次版本上调而改变。
- 1.2.0 的 exact-version 钉住值必须与 CMake project VERSION、installed `version.hpp` 宏一致（R-093）。
- 已发布的 v1.0.0 与 v1.1.0 API manifest 不得被改写。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| documented public surface | 1.2.0 | 与 1.1.0 语义兼容，安装面不再完整暴露协议类型 |
| exact-version consumer | 钉住 1.1.0 的 find_package | 对 1.2.0 安装 configure 失败，必须更新钉住值 |
| unsupported detail 使用者 | 任何版本 | 不构成兼容承诺 |

### Rationale

minor 上调区分安装面，避免重演「同一版本两套 header」；不是 major，因为收回的是 D-169 已排除的 unsupported 入口。

### Rejected alternatives

- 保持 1.1.0：exact-version 无法区分两套安装面。
- 升到 2.0.0：把 unsupported `astra::detail` 误当成已承诺 public source。

### Consequences

- `tests/consumer/CMakeLists.txt`、corpus/package 检查中的版本钉住值随门禁更新到 1.2.0。
- release evidence 写入 `docs/release/1.2.0/`，不覆盖 1.0.0 / 1.1.0 历史证据。
- benchmark corpus 的 `astra_version` 按现有 baseline 规则处理（允许已记录且不新于当前 VERSION）。

### Non-goals and deferred risks

- 不在本决策中冻结 1.2.0 的完整 symbol allowlist 内容，那随 D-170 实现产生。
- 不重新定义 SemVer 对 0.x 的历史规则。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: owner 在 1.1.0 / 1.2.0 / 2.0.0 中明确选择升到 1.2.0。
- Code or data evidence: 当前 `CMakeLists.txt` 为 `VERSION 1.1.0`；`check_cmake_package.py` 要求 consumer exact-version 与 project VERSION 一致。

### Traceability

- ADR: [ADR-0048](../../docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md)
- Spec destinations: R-122
- Tickets: Pending
- Tests: `R093VersionContractGates`、`check_api_freeze` 新 v1.2.0 manifest

## D-172 — 公开 awaitable 以薄包装留在 installed headers

Status: accepted

Date: 2026-08-30

Supersedes: None

Superseded by: None

### Context

C++20 `co_await` 要求 consumer 翻译单元能看见 awaiter 的 `await_ready` / `await_suspend` / `await_resume`。D-170 禁止协议类型进入安装头，但不自动禁止编译器看见一个不含协议字段的 awaiter。documented 操作是 `co_await` TaskHandle/GraphRun、`yield()`、`sleep_for`/`sleep_until`、`cancellation_point`，不是 awaiter 类型名。

### Decision

公开 awaitable 的 awaiter **以薄包装完整类型保留在 installed headers**。`await_suspend` 只调用 compiled TaskControlBlock / handshake，不得在安装头中展开协议状态机。

薄包装类型及其成员不得暴露 `AwaitHandshake`、`TaskSharedState` / TaskControlBlock、mutex/cv、rescheduler、timer registrar 等协议入口。documented compatibility surface 是上述 `co_await` 操作，不承诺 awaiter 的名字、嵌套类型或布局。

### Invariants

- `co_await` TaskHandle（仅左值）、GraphRun（仅左值）、`yield()`、`sleep_for`/`sleep_until`、`cancellation_point` 的可观察挂起/恢复/取消语义不变。
- 安装头中的 awaiter 不得让 consumer 完成类型或调用 `AwaitHandshake` 与 TaskControlBlock 协议。
- 不把第三方自定义 awaiter 列为 documented 扩展面。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| documented 操作 | public_contract 与 public tests | 继续 `co_await yield()` 等写法 |
| awaiter 类型名 | installed headers | 允许存在薄包装；不进入 documented allowlist |
| handshake / TCB | 库实现与 internal tests | 完整协议，不安装 |

### Rationale

薄包装满足语言对 awaiter 完整性的要求，同时把协议留在 compiled module。改命名空间 alone 不够；整条 await 类型擦除本轮回归面过大。

### Rejected alternatives

- 仅把 awaiter 改到 `astra::detail` 但仍带协议字段：泄漏仍在。
- 本轮整条 await 路径类型擦除、安装头无 awaiter 体：`coroutine_handle` 与帧移交回归面大，非完成 1A 所必需。

### Consequences

- `YieldAwaiter` / `SleepAwaiter` / TaskHandle 与 GraphRun 的 co_await awaiter 需要改成不持有协议类型的公开成员。
- internal handshake 测试继续走非安装头，不通过薄包装去探协议字段。

### Non-goals and deferred risks

- 不在本决策中规定薄包装是 `astra::` 还是 `astra::detail` 名字。
- 不规定 TaskHandle 结果格的具体类型名。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: owner 在薄包装 / 只改命名空间 / 整条类型擦除中明确选择薄包装。
- Code or data evidence: `include/astra/coroutine.hpp` 中 `SleepAwaiter` 现持有 `shared_ptr<AwaitHandshake>`；`TaskHandleAwaiter` 调用 `shared_state_internal()`。

### Traceability

- ADR: [ADR-0048](../../docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md)
- Spec destinations: R-120
- Tickets: Pending
- Tests: public coroutine/spawn/await 行为测试；encapsulation probes 禁止完成 handshake 类型

## D-173 — TaskHandle 结果格为 private nested 模板，安装头不再出现 TaskSharedState 名

Status: accepted

Date: 2026-08-30

Supersedes: None

Superseded by: None

### Context

D-170 要求任意 `T` 的 `TaskHandle<T>::get()` 仍能在 consumer 侧实例化，因此值/异常/终态不能全部编进库；同时禁止把运行协议放回安装头。继续使用安装头中的 `astra::detail::TaskSharedState<T>` 名字会保留旧泄漏标识。把结果也类型擦除进 TCB 则无法自然保持 `get()` 的 `const T&` 与任意 `T`。

### Decision

安装头中的结果存储是 `TaskHandle<T>`（及 `TaskHandle<void>`）的 **private nested 模板**，只承载值 `T`（或 void）、异常与终态观察所需状态。consumer 不能命名或构造该 nested 类型作为独立入口。

安装头不再提供可被 consumer 完成的 `TaskSharedState` / `TaskSharedStateBase` 类型名。运行协议留在 compiled TaskControlBlock。

### Invariants

- `TaskHandle<T>::get()` 仍返回 `const T&`（void 特化不返回值）；失败重抛、取消抛 `task_cancelled`；仅左值可调用。
- 空/moved-from 句柄行为不变。
- package consumer 不能完成类型 `astra::detail::TaskSharedStateBase` 或 `astra::detail::TaskSharedState<T>`。
- 结果格不得包含 mutex、回调列表、rescheduler、timer hook 或 handshake。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| TaskHandle&lt;T&gt; | 有值结果 | private nested 存 `T` |
| TaskHandle&lt;void&gt; | 无值结果 | private nested 无 `T` 存储，get() 只同步终态 |
| TaskControlBlock | 库实现 | 协议与同步，不出现在安装头完整定义中 |

### Rationale

结果格必须对任意 `T` 可见且随 TaskHandle 封装；保留 `TaskSharedState` 名称等于把泄漏名字半正式化。

### Rejected alternatives

- 继续叫 `detail::TaskSharedState<T>` 但砍成只有结果：旧名字仍在安装面。
- 结果也类型擦除进 compiled TCB：任意 `T` 的 `const T&` 与寿命模型回归面过大。

### Consequences

- `submit`/`spawn` 模板在 consumer TU 构造结果格，并把类型擦除或编译期桥接到 TCB。
- internal tests 若直接命名 `TaskSharedState` 必须改为非安装头或改走 TaskHandle 公共面。

### Non-goals and deferred risks

- 不在本决策中规定 nested 类型的标识符拼写。
- 不规定 callable invoker 模板是否仍留在安装头（另决策）。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: owner 在 private nested 结果格 / 保留 TaskSharedState 名 / 结果类型擦除中明确选择 private nested。
- Code or data evidence: 当前 `TaskSharedState<T>` 定义于 `include/astra/task_handle.hpp` 并被 public `TaskHandle` 持有。

### Traceability

- ADR: [ADR-0048](../../docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md)
- Spec destinations: R-119
- Tickets: Pending
- Tests: TaskHandle get/wait/cancel 现有测试；encapsulation 禁止完成 `TaskSharedState*` 类型

## D-174 — submit/emplace 安装头只保留 F 信封，协议不进 invoker 模板

Status: accepted

Date: 2026-08-30

Supersedes: None

Superseded by: None

### Context

`submit(F)` 与 `TaskGraph::emplace(F)` 必须在 consumer TU 接住任意可调用类型 `F`。当前安装头中的 `TaskInvokerModel` / `GraphTaskInvokerModel` 在 `execute()` 内联 try_start、metrics、set_value 与 execution context，把运行协议嵌进了 installed interface。D-170 要求协议进 compiled TaskControlBlock；D-173 已把结果格留在 TaskHandle。

### Decision

安装头只保留 **F 信封**：模板存储 `F`（及是否接受 `stop_token` 的调用形态），`execute()` 只调用 `F` 并把返回值交给结果格或 compiled TCB。admission、try_start、metrics、helping、timer、handshake 不得出现在该信封的安装头实现中。

不在 header 缝上把 `F` 擦成 `std::function`。不保留当前大 invoker 模板作为完成形态。

### Invariants

- 可调用对象约束不变：`f(args...)` 或 `f(stop_token, args...)`；不得返回裸引用；结果可移动。
- Graph 节点可调用对象仍必须返回 void。
- 只移动、不可拷贝的 `F` 仍可提交，不因改用 `std::function` 而被拒绝。
- F 信封不得让 consumer 触达 TaskControlBlock 协议或 `AwaitHandshake`。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| submit / try_submit / spawn | 任意结果 T | 信封调用 F 后写入 TaskHandle 结果格 |
| TaskGraph::emplace / emplace_coroutine | 节点返回 void | 信封只执行节点体；依赖传播仍在 GraphExecution |

### Rationale

与结果格同一原则：类型相关留下，协议编进库。`std::function` 会改变 callable 约束并多一次分配。

### Rejected alternatives

- header 缝 `std::function` 擦除：排斥 move-only F，并改变提交成本。
- 保持现有大 invoker 模板：违反 D-170。

### Consequences

- `include/astra/scheduler.hpp`、`graph.hpp`、`task_handle.hpp`、`coroutine.hpp` 中 invoker `execute()` 必须去掉协议内联。
- R-110 导出集将随 invoker 从 inline 弱符号变为更少的协议导出而变化（细节由后续门禁决策约束）。

### Non-goals and deferred risks

- 不在本决策中规定信封类型的标识符。
- 不在本决策中完成 GraphExecution 深化（D-170 Non-goals）。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: owner 在 F 信封 / std::function 擦除 / 保持大 invoker 中明确选择 F 信封。
- Code or data evidence: `include/astra/task_handle.hpp` 的 `TaskInvokerModel::invoke_impl` 与 `include/astra/graph.hpp` 的 `GraphTaskInvokerModel::execute` 当前内联协议。

### Traceability

- ADR: [ADR-0048](../../docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md)
- Spec destinations: R-121
- Tickets: Pending
- Tests: submit/spawn/graph emplace 现有行为测试；move-only callable 覆盖必须保留或补上

## D-175 — 1.2.0 共享库用 version script 只导出 documented allowlist

Status: accepted

Date: 2026-08-30

Supersedes: None

Superseded by: None

### Context

D-170 把运行协议编进库后，`TaskSharedStateBase` 与 metrics hook 不再需要出现在 consumer 可链接的 dynsym 上。当前 `check_cmake_package.py` 把全部 `astra` 动态符号做成封闭集，约 56 个 `astra::detail` 被当成必须导出。若 1A 完成后仍导出这些符号，consumer 仍可能按符号调用内部协议。R-110 要求隐藏内部符号。

### Decision

实施 D-170 的同一验收包含 Linux 共享库 **version script / hidden-by-default**：默认不导出，只导出 documented public allowlist（与 v1.2.0 semantic manifest / `public_contract.cpp` 对齐的符号）。

`check_cmake_package.py` 的封闭集改为该 allowlist，不再要求 `astra::detail` 协议符号存在。header 模板若仍引用被隐藏的协议符号，视为 D-170 未完成，应链接失败而不是把符号加回导出表。

不把「只改封闭集两栏、detail 仍导出」当作本轮完成定义。

### Invariants

- shared `libAstraScheduler.so` 的 `nm -D --defined-only` 中，`astra::detail` 协议类型与 `record_metrics_*` / handshake / TCB mutator 不得出现在默认导出。
- documented 符号（Scheduler、TaskHandle 非模板入口、Finalization、Trace、version 查询等）必须可被独立 consumer 链接。
- v1.0.0 / v1.1.0 manifest 仍不可改写。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| shared install | R-110 consumer | 只能链接 documented 符号 |
| static install | 同一套源 | 无 dynsym 面；仍不得在安装头暴露协议类型 |
| internal tests | 可链库内可见性或 test runtime | 不依赖已隐藏的 public dynsym 去测协议 |

### Rationale

协议离开安装头却仍导出，封装是假的。version script 与 1A 是同一条验收，不是后续可选项。

### Rejected alternatives

- 本轮只把封闭集改成 documented ∪ bridge：detail 仍可 `nm` 到，consumer 仍能打内部符号。
- 封闭集原样保留现有 detail 清单：与 D-170 冲突。

### Consequences

- GCC/Clang 需要显式 version script 或 `-fvisibility=hidden` 加 documented `ASTRA_EXPORT`。
- `check_api_freeze.py` 已过滤 detail，应与新导出集一致。
- 残留 header 对隐藏符号的引用会变成 1A 的红灯，而不是把门禁放宽。

### Non-goals and deferred risks

- 不承诺跨 toolchain ABI（R-093 / D-145 仍有效）。
- 不在本决策中列出 1.2.0 allowlist 的逐个 mangled 名字。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: owner 在 version script 只导出 documented allowlist / 仅改封闭集两栏 中明确选择前者，并作为 1A 同一验收。
- Code or data evidence: `tools/check_cmake_package.py` 当前 `expected_symbols` 含大量 `_ZN5astra6detail...`。

### Traceability

- ADR: [ADR-0048](../../docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md)
- Spec destinations: R-123
- Tickets: Pending
- Tests: `check_cmake_package.py` 导出 allowlist；encapsulation 与独立 consumer 链接

## D-176 — 按完整不变量抽出 AdmissionController、TimerQueue、RuntimeMetrics

Status: accepted

Date: 2026-08-30

Supersedes: None

Superseded by: None

### Context

`Scheduler::Impl` 仍在单一翻译单元中同时拥有 admission、ready 队列、park handshake、timer、metrics、graph run 与 helping wait。架构审查第 4 点给出三种拆法。D-170 曾把拆 Impl 列为 v1.2.0 非目标。owner 现要求实施第 4 点 A 方案。

### Decision

从 `Scheduler::Impl` 抽出三个内部深模块，每个拥有一组完整不变量：

- `AdmissionController`：外部 pending slot、backpressure、阻塞等待与 rollback。
- `TimerQueue`：register/cancel、到期收集、最早唤醒、关停时全部取消。
- `RuntimeMetrics`：分片所有权、record hooks、snapshot 累加。

禁止只持有 `Scheduler::Impl*` 并转发方法的浅 wrapper。本轮不抽出 ReadyQueues、steal round 或 park handshake。

### Invariants

- documented public admission、timer、metrics 可观察语义不变。
- 新模块必须拥有自己的状态（slot 计数/堆/分片），不得把 Impl 当唯一成员。
- ReadyQueues/steal/park 仍由 Impl 拥有。
- GraphExecution 深化不在本决策范围。

### Scope and variants

| Variant | Applies | Different behaviour |
|---|---|---|
| AdmissionController | 外部 slot 与 backpressure | 内部提交仍豁免容量 |
| TimerQueue | Worker timer | 最早到期通过回调唤醒 runtime，不拥有 park |
| RuntimeMetrics | Off/Basic/Detailed | Off 仍为零开销投影 |

### Rationale

按不变量拆才能通过 deletion test。只拆 TU 或同时抽 park/steal 会把弱内存证据面和浅转发一起带进来。

### Rejected alternatives

- 只拆翻译单元不新增 interface（4B）：知识仍在 Impl。
- 同时抽 ReadyQueues+steal+park（4C）：弱内存/TSan 面过大，放后面。
- Impl* 转发包装：deletion test 失败。

### Consequences

- Impl 变为组合这三个模块并继续拥有 lifecycle 与 ready 队列。
- 现有 admission/timer/metrics 测试必须保持绿。

### Non-goals and deferred risks

- 不深化 GraphExecution（架构审查第 2 点）。
- 不抽 ReadyQueues/steal/park。
- 不把 submit/spawn 收成单一 compiled admit lease（第 3 点）。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: owner 要求实现架构审查第 4 点 A 方案：先 AdmissionController / TimerQueue / RuntimeMetrics，禁止浅 wrapper。
- Code or data evidence: `src/admission_controller.{hpp,cpp}`、`src/timer_queue.{hpp,cpp}`、`src/runtime_metrics.{hpp,cpp}`；`Scheduler::Impl` 组合这三个模块并继续拥有 ready 队列与 park。

### Traceability

- ADR: None
- Spec destinations: R-124
- Tickets: AST-064, AST-065, AST-066
- Tests: admission/timer/metrics 现有行为测试；encapsulation 禁止新模块持有 Impl*

## D-177 — GraphExecution 通过 Runtime Port 独立并按所有权重组内部源码

Status: accepted

Date: 2026-08-30

Supersedes: None

Superseded by: None

### Context

D-176 已按完整不变量抽出 AdmissionController、TimerQueue 与 RuntimeMetrics，但明确延期 GraphExecution、ReadyQueues、steal 与 park。当前 `scheduler.cpp` 仍约 2500 行；`GraphExecution::run()` 虽有独立类型，仍定义在该文件并直接读取 `scheduler.impl_`。`src` 的 Runtime、Task、Graph、Lifecycle、Observability 文件也全部平铺，目录未表达所有权。

### Decision

GraphExecution 必须通过非安装的窄 `GraphRuntimePort` 使用 identity、admission、ready publication、timer 与 graph metrics 能力，不得访问 `Scheduler`、`Scheduler::Impl` 或其字段。Graph admission reservation 使用单一 RAII lease，在 root publication ownership 转移前统一回滚 slots 与 active-run accounting。Graph 协议迁入独立 `graph_execution.cpp`，依赖传播、node publication、terminal 与取消入口由 GraphExecution 实例方法拥有。

Scheduler 内部进一步抽出 Worker loop 与 non-owning Runtime registry/diagnostic routing；Scheduler facade 保留 public API 校验和委托，Runtime State 继续拥有 lifecycle、queues 与组件组合。最后以纯 rename 提交按 runtime/task/graph/lifecycle/observability/scheduling/testing 重组 `src`；内部头保持不安装，单一产品 target 保持不变。

### Invariants

- R-069 至 R-079、R-084 至 R-088 与全部 shutdown/ownership observable semantics 不变。
- GraphRuntimePort 是 internal seam，不进入 install/public manifest。
- Graph reservation 在 commit 前只有 RAII lease 一个 rollback owner；commit 后由 node terminal/completion 路径拥有 accounting。
- Runtime registry 只做 non-owning lookup，不延长 Runtime lifetime。
- Worker loop 抽取不改变 claim、steal、priority、park、helping 或 weak-memory ordering。
- 目录移动只改变路径、CMake 与 internal include，不与行为变化混合。

### Rationale

先建立能力 seam 再迁移实现，能避免把大型 Impl 定义扩散到更多翻译单元。按协议所有权拆分可使目录结构反映真实依赖，而不是制造字段转发 wrapper。

### Rejected alternatives

- 直接把 Impl 定义移入共享内部头：扩大字段耦合并增加重编译面。
- 先移动目录再拆职责：产生大量 rename 噪声且所有权仍不清晰。
- GraphExecution 继续 friend Scheduler：类型独立但实现仍依赖 private representation。
- 为每个子目录建立 object/static library：当前没有独立链接边界，只增加构建复杂度。

### Consequences

- 新增 GraphRuntimePort、GraphAdmissionLease 与 graph_execution.cpp。
- scheduler.cpp 删除 Graph protocol，并逐步把 worker/registry 协议迁出；wait/await diagnostics 路由的剩余拆分由 AST-071 跟踪。
- CMake source paths 和 internal test include 更新；install surface 不变。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: owner 要求先记录 `docs/src目录与Scheduler模块化优化方案.md`，随后明确要求按该方案优化项目。
- Code or data evidence: `scheduler.cpp` 约 2541 行；`GraphExecution::run()` 当前位于其中并读取 `scheduler.impl_`。

### Traceability

- Spec destinations: R-125 through R-128
- Tickets: AST-067 through AST-071
- Tests: Graph/admission/coroutine suites、Debug/ASan/TSan、semantic API、encapsulation、package consumer

## D-178 — RuntimeState 与 ReadyQueues 按状态所有权深化

Status: accepted

Date: 2026-08-31

Supersedes: None

Superseded by: None

### Context

D-177 已迁出 GraphExecution、Worker loop 与 non-owning Runtime registry，但 `Scheduler::Impl` 仍直接拥有 lifecycle、global/local ready queues、worker counters、timer/admission/metrics 组合和 diagnostics producer context。`worker_loop.hpp` 仍以模板访问这些字段，`scheduler.cpp` 因而仍接近 2000 行，Runtime State 与 ready-work ownership 没有形成可删除、可单测的内部边界。项目 owner 现明确要求继续完成 `RuntimeState/ReadyQueues/diagnostics` 深层拆分。

### Decision

新增非安装 `ReadyQueues` 深模块，完整拥有 Global EDF/FIFO bands、per-Worker local queues、weighted claim、bounded steal、immediate cancel cleanup 与 queue inspection；它可以通过显式 `RuntimeMetrics&` 记录队列指标，但不得持有 `Scheduler::Impl*` 或生命周期字段。

新增非安装 `RuntimeState`，作为一个 Scheduler Runtime 的身份、resolved options、status、AdmissionController、TimerQueue、RuntimeMetrics、ReadyQueues、worker synchronization/thread collection 与 Trace producer attachment 的唯一组合所有者。`Scheduler::Impl` 收敛为共享所有权外壳与 GraphRuntimePort adapter，不再复制 Runtime 状态。

AST-071 的 diagnostics 拆分在 RuntimeState seam 稳定后实施：registry 保存 non-owning diagnostics port，`runtime_diagnostics` 负责 wait/await Metrics 与 Trace 路由，不得读取 `Scheduler::Impl` 字段。Worker loop 改为针对 RuntimeState 的非模板实现单元。

### Invariants

- Ready Task Ownership 从 publication 到 claim/cancel 仍恰好一个 owner；queue move/resize 不复制逻辑所有权。
- Global EDF、priority calendar、local LIFO、steal FIFO、local burst limit 与 external slot release 时点不变。
- Runtime State 生命周期仍由 `shared_ptr<Scheduler::Impl>`、Worker handoff 与 Reaper 协议控制；内部模块不得延长该生命周期。
- startup transaction、shutdown escalation、Drain Work Closure、Join Ready 与 packed status memory ordering 不变。
- diagnostics registry/port 是 non-owning；Runtime 注销后 lookup 必须安全失败为 no-op。
- 不改变 public API、Metrics/Trace schema、安装清单或 Supported Configuration。

### Rationale

先让 ReadyQueues 拥有完整队列不变量，再把生命周期组合收进 RuntimeState，可以避免创建只转发字段的 façade，也避免把原始 `Scheduler::Impl` 整体搬进共享头。diagnostics 最后依赖稳定 port，可消除二次适配。

### Rejected alternatives

- 仅把 `Scheduler::Impl` 文本剪切到 `runtime_state.hpp`：字段耦合与重编译面不变，不构成深模块。
- ReadyQueues 保存 `Impl*` 并转发原方法：无法通过 deletion test。
- RuntimeState 自行取得 shared ownership：会与现有 Scheduler Handle/Reaper ownership 重叠。
- 同时改变 queue 算法或调度权重：扩大 weak-memory 与性能语义风险。

### Consequences

- 新增 `src/runtime/ready_queues.{hpp,cpp}` 与 `src/runtime/runtime_state.{hpp,cpp}`。
- `worker_loop` 成为针对 RuntimeState 的 compiled implementation，而非字段 duck-typing 模板。
- `scheduler.cpp` 只保留 Scheduler façade、Impl ownership adapter 与等待/helping入口；diagnostics 路由迁入 observability。
- AST-072、AST-073 先于 AST-071 完成。

### Evidence

- Confirmation authority: project owner（user）
- Confirmation summary: owner 明确要求进行更深层的 `RuntimeState/ReadyQueues/diagnostics` 拆分。
- Code or data evidence: `src/runtime/scheduler.cpp` 约 1985 行；队列字段与 lifecycle/diagnostics 仍同处 `Scheduler::Impl`。

### Traceability

- Spec destinations: R-129, R-130；R-127 由 AST-071 完成剩余 diagnostics 范围
- Tickets: AST-072, AST-073, AST-071
- Tests: queue ordering/steal/park/helping/shutdown/wait diagnostics；Debug/ASan/TSan/package/encapsulation

## Open Questions

- 无（本轮 1A grilling 已关闭：D-170..D-175）。
- 2026-08-26 全项目功能覆盖审计已完成。后续新需求或实现证据若改变语义，必须新增/取代决策而不得静默修改已接受规则。



