# AstraScheduler Runtime

AstraScheduler 的统一领域语言，用于明确任务从何处进入运行时，以及生命周期操作所覆盖的工作集合。

## Language

**Task Handle**:
由 `submit()` 返回、标识一次已接受任务并承载其结果与控制能力入口的可复制共享 capability；所有副本表示同一个 Task Identity，具体操作由对应规范定义。
_Avoid_: `std::future`, Future wrapper

**Unobserved Task**:
已经被 Runtime 接受、但当前没有任何存活 Task Handle 的任务。
_Avoid_: Orphan Runtime, abandoned task, detached task

**Terminal Outcome**:
Task 进入终态后发布的不可变完成事实，恰好表示 Value、Exception 或 Cancelled 三类结果之一。
_Avoid_: Consumed result, transient return, TaskState

**Task Lifecycle State**:
已接受 Task 在 Waiting、Ready、Running、Suspended 与三个不可逆终态之间的公开生命周期投影；它不包含 Handle 是否为空、取消请求或队列算法瞬态。
_Avoid_: Handle state, queue state, stop flag

**Eligible Task**:
已满足依赖、时间和 suspension 条件，并且当前 Runtime lifecycle 允许被 Worker 开始或恢复的 Ready Task。
_Avoid_: Merely queued task, accepted task, Running task

**Timer Wake Time**:
Coroutine 定时等待所使用的 `steady_clock` eligibility 下界；Runtime 不会因该 timer 在此时刻之前使 Task Ready，但到时后的调度与执行仍是 best-effort。
_Avoid_: Task Deadline, execution time, hard-real-time wakeup

**Base Priority**:
Task 在 admission 时解析并在整个生命周期保持不变的 Low、Normal、High 或 Critical 调度提示；它只影响 Ready work 的加权选择，不产生抢占或实时保证。
_Avoid_: OS thread priority, dynamic boost, authorization level

**Priority Service Opportunity**:
某个 Ready source 在加权日历中选择一个 Priority band并尝试成功claim工作的机会；它表达相对服务份额，不表达墙钟延迟或完成顺序。
_Avoid_: Execution deadline, CPU reservation, strict priority

**Task Deadline**:
TaskOptions 中显式携带的 `steady_clock` 绝对时刻，表示该 Task 希望不晚于此时首次成功进入 Running；它参与同 Priority band 的 best-effort EDF 排序但不触发等待、取消或抢占。
_Avoid_: Timer Wake Time, completion timeout, cancellation time

**Deadline Miss**:
带 Task Deadline 的任务首次 start_time 晚于其 Deadline 的不可变观测事实；Runtime记录该事实但不改变 Task 的 Priority、执行或 Terminal Outcome。
_Avoid_: Task failure, automatic cancellation, rejection

**Metrics Snapshot**:
从一个Runtime的分片观测状态聚合出的不可变值；每个字段有效且来自capture区间，但并发运行时不承诺所有字段对应同一全局线性化瞬间。
_Avoid_: Transactional snapshot, scheduler checkpoint, metrics reset

**Quiescent Metrics Point**:
Runtime没有并发admission、状态转换或Worker执行时用于验证counter守恒关系的观测边界。
_Avoid_: Arbitrary concurrent snapshot, queue-empty observation

**Trace Collector**:
显式附加到一个或多个Runtime、以固定容量内存buffer记录versioned events并管理重复capture代际的共享观测capability；它不拥有或驱动Runtime生命周期。
_Avoid_: Logger, runtime owner, background file writer

**Trace Completeness**:
某次capture在已启用category内没有collector drop或schema corruption的属性；它不表示进程中每个Runtime都已附加，也不表示关闭的category被记录。
_Avoid_: Audit guarantee, all-process coverage, enabled category set

**Benchmark Case**:
具有版本化workload、明确timed region、输入参数、正确性checksum和primary metric的一项可重复实验定义。
_Avoid_: Ad-hoc timing loop, demo, correctness test

**Benchmark Artifact**:
保存环境、构建、case schema、全部原始repetition、统计摘要、校验与invalid诊断的版本化结果；性能结论必须可追溯到该artifact。
_Avoid_: Best number, screenshot, unversioned CSV summary

**Supported Configuration**:
被项目明确列入Tier、持续构建测试并由release gate覆盖的OS、architecture、compiler、standard library与build-mode组合；仅能编译不等于受支持。
_Avoid_: Theoretically portable, best-effort platform, one successful build

**Source Compatibility**:
相同受支持toolchain/build配置下，documented public C++ source和observable semantics按SemVer保持兼容；它不表示跨版本、compiler、stdlib或CRT的binary ABI兼容。
_Avoid_: ABI stability, header/runtime mismatch, schema compatibility

**Ready Routing Precedence**:
Ready publication先应用具体awaiter或Deadline的destination规则，再回落到publisher是否为所属Runtime owner Worker的Local/Global默认；它只决定队列来源，不改变eligibility或Priority。
_Avoid_: Worker affinity, inline resume, priority selection

**Wait Edge**:
从外部调用方或source Task到Task/GraphRun target的一次同步wait或Coroutine await关系；Trace可记录其logical identities用于离线诊断，但Runtime不把它维护为在线全局依赖图。
_Avoid_: DAG edge, ownership edge, live deadlock detector

**Logical Runtime Identity**:
由强类型RuntimeId以及以其为scope的TaskId/GraphRunId表达的进程内稳定关联值；它与对象地址、线程ID和控制capability无关。
_Avoid_: Raw pointer, security token, global task registry

**NodeId**:
Graph 定义内按插入顺序分配的强类型局部节点标识；完整运行节点关联为 GraphRunId + NodeId + TaskId。历史文字 GraphNodeId 不是公共类型或 alias。
_Avoid_: GraphNodeId, raw node index, process-global node identity

**Local Deque Backend**:
Runtime startup 时冻结的实际 per-Worker Local Queue 实现能力：None、Locked 或 ChaseLevLockFree；只描述 deque backend，不声称整个 Runtime lock-free。
_Avoid_: Infer from version, algorithm-name lock-free claim, runtime backend switching

**Scheduler Startup Transaction**:
从配置验证、Reaper registration/handoff预留和Worker创建到Running publication的同步构造事务；成功返回有效共享Handle，失败则在抛出前完整回滚且不暴露Starting状态。
_Avoid_: Public start phase, lazy start, restart

**Resolved Scheduler Options**:
Scheduler startup复制并验证后的不可变policy/configuration快照；Metrics与Benchmark报告它，调用方随后修改原Options不改变Runtime。
_Avoid_: Live configuration object, implementation tuning map, environment override

**Astra Implementation Instance**:
一个link image中承载Reaper、Finalization gate、ID allocator和process metrics的具体AstraScheduler实现副本；Supported Configuration要求一个进程只有一个这样的实例。
_Avoid_: Namespace, Scheduler Runtime, multiple vendored static copies

**Scheduler Status Snapshot**:
一次线性化同时观察SchedulerState与ShutdownMode的不可变值；它可立即过时且不授予后续submission或shutdown的原子资格。
_Avoid_: Admission token, two independent getters, lifecycle wait

**Steal Round**:
空闲 Worker 在进入下一阶段 backoff 前，对一组有界且不重复 victim Local Deque 执行的任务窃取探测。
_Avoid_: Infinite steal loop, global scan, batch steal

**Park Handshake**:
Worker 在阻塞休眠前登记意图并二次检查 work publication generation、可执行来源和退出条件的协议，用于消除检查为空与实际休眠之间的丢唤醒窗口。
_Avoid_: Timed polling, blind condition wait, sleep flag

**Scheduling Reference**:
一个 Ready Task 从成功 publication 到唯一 claim、start 或取消清理之间由调度层持有的逻辑生命周期责任；buffer resize 产生的物理 cell 副本不复制该责任。
_Avoid_: Handle reference, buffer-cell ownership, raw pointer lifetime

**Frozen Task Graph**:
经过结构验证、Node 与 Edge 不再可变，并携带 move-only Node Callable 等待一次 GraphRun 消费的 DAG 定义。
_Avoid_: Reusable graph template, running graph, mutable builder

**Graph Run**:
一个 Frozen Task Graph 被某个 Scheduler 成功原子接受后形成的单次执行实例，拥有全部 Node 的运行状态并以所有 Node Terminal 为完成边界。
_Avoid_: Frozen graph, root task, partial graph

**Unbounded Wait**:
没有 timeout、只在目标真实完成事实发布后结束的同步观察；它本身不改变取消或关停策略，并可能永久阻塞。
_Avoid_: Infinite wait, blocking attempt, implicit timeout

**Helping Wait**:
Scheduler Worker 等待同 Runtime 的另一个任务时，通过继续调度 Eligible Task 推进目标完成的嵌套等待方式；它保留当前 Callable，但不把 Worker 专用于条件等待。
_Avoid_: Blocking wait, compensation thread, busy wait

**Direct Self-Wait**:
当前正在执行的 Task 通过表示同一 Task Identity 的 Handle 等待自己的 Terminal Outcome。
_Avoid_: Helping Wait, recursive task, indirect dependency cycle

**Indirect Wait Cycle**:
两个或更多正在执行的 Task 通过 Task Handle 等待边形成的动态闭环，其中每个 Task 的完成都传递依赖环中下一个 Task。
_Avoid_: Direct Self-Wait, DAG cycle, temporary wait chain

**Cross-Runtime Helping Wait**:
一个 Scheduler Worker 等待另一 Runtime 的 Task 时，继续帮助自己的源 Runtime 而只观察目标 Runtime Terminal Outcome 的等待方式。
_Avoid_: Cross-runtime stealing, foreign worker, blocking source worker

**Task Cancellation Request**:
针对一个已接受 Task 的显式停止意图；它与 Task start 线性化竞争，并根据 Task 是否已经 Running 产生终态取消或 cooperative stop request。
_Avoid_: Force kill, Handle destruction, Scheduler shutdown

**Cancellation Signal**:
以 `astra::task_cancelled` 逃出 Callable、由 Task execution boundary 识别并转换为 Cancelled Terminal Outcome 的显式协作退出信号。
_Avoid_: Ordinary exception, stop request, thread cancellation

**Internal Submission**:
调用方正在同一个 Scheduler 的执行上下文中运行任务时，向该 Scheduler 发起的任务提交。
_Avoid_: Child submission, worker submission

**External Submission**:
不属于 Internal Submission 的任务提交，包括应用线程提交以及另一个 Scheduler 的 Worker 发起的提交。
_Avoid_: User submission, outside submission

**External Pending Capacity**:
一个 Runtime 对已接受但尚未首次进入 Running 的 External Submission 所设的准入配额；Internal Submission 与已开始 Task 不占用该配额。
_Avoid_: Queue memory limit, total task limit, Internal capacity

**Drain Work Closure**:
Graceful shutdown 必须纳入核算的传递工作集合，由关停前已接受的任务及其获准的 Internal Submission 组成。
_Avoid_: Queue snapshot, pending queue

**Shutdown Mode**:
Scheduler 处于 `Stopping` 生命周期状态时所采用的关停策略，取值为 Graceful 或 Immediate；它与 SchedulerState 是不同维度。
_Avoid_: Stop flag, shutdown type

**Shutdown Completion**:
一次 Scheduler 关停过程中由所有非 Worker 关停调用共同观察的完成边界；它在全部 Worker 退出并 join、`Stopped` 发布后达成。
_Avoid_: Per-caller shutdown, queue-empty signal

**Scheduler Handle**:
应用持有并用于访问一个 Scheduler Runtime 的前台所有权对象；它的生命周期可以早于对应 Runtime State 的生命周期结束。
_Avoid_: Runtime object, thread-pool state

**Runtime State**:
与 Scheduler Handle 生命周期解耦、由执行路径与回收路径共享持有的 Scheduler Runtime 身份；它持续存在直至不再有 Worker 访问且线程回收完成。
_Avoid_: Scheduler Handle, raw scheduler pointer

**Reaper**:
不属于目标 Scheduler Worker 集合的生命周期协调者，接管失去 Scheduler Handle 后仍存活的 Runtime State，并负责安全完成线程 join 与最终回收。
_Avoid_: Worker cleanup, detached worker

**Reaper Service**:
进程内承载 Reaper 角色的共享控制面服务，由一个不属于任何 Scheduler 的专用协调线程驱动，并被所有 Scheduler Runtime 共同使用。
_Avoid_: Per-Scheduler reaper, worker pool, cleanup task

**Reaper Finalization**:
永久关闭新 Scheduler Runtime 注册，并最终终结进程级 Reaper Service 的一次性生命周期边界。
_Avoid_: Scheduler shutdown, idle timeout, Reaper restart

**Finalization Completion**:
全部纳入 Runtime 完成关停、Reaper 回收工作清空且 coordinator 已退出并 join 后达成的进程级完成边界；等待超时不等于该边界达成。
_Avoid_: Registration closed, begin returned, wait timeout

**Finalization Control**:
由 `begin_finalization()` 返回、用于观察或升级同一次进程级 Reaper Finalization 的共享 capability；它不是 Reaper Service 或 Runtime State 的生命周期所有者。
_Avoid_: Finalization owner, global waiter, RAII finalizer

**Finalization Escalation**:
在 Reaper Finalization 期间由调用方显式触发的进程级策略升级，将全部已核算且尚未完成的 Runtime 单向请求为 Immediate；它不是完成信号，也不承诺终结时间有界。
_Avoid_: Timeout cancellation, force kill, per-handle shutdown

**Pending Runtime State**:
已移交给 Reaper、但尚未达到 Join Ready 的 Runtime State；Reaper 维持其存活，但不阻塞等待其活动任务终结。
_Avoid_: Pending task, queued task, Stopped runtime

**Join Ready**:
Runtime 的全部 Worker loop 已不可逆地进入终止收尾、不再执行或调度用户任务，因而 Reaper 可以开始 join 的单调边界；它不等同于 `Stopped` 或 Shutdown Completion。
_Avoid_: Queue empty, Stopped, Shutdown Completion
