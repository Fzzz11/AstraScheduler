# AstraScheduler：现代 C++20 高性能任务调度运行时设计文档

> 项目定位：基于 **C++20** 实现一个以 **Work Stealing** 为调度核心，支持 **任务图 DAG、协作式取消、Future/异常传播、Coroutine 调度、运行时追踪与 Benchmark** 的现代任务调度运行时。
>
> 目标不是再写一个“固定线程 + 全局队列”的普通线程池，而是完成一个具有明确架构、可解释并发模型、可验证性能收益、可继续演进的工程级并发项目。
>
> 文档关系：本文提供跨版本总体架构与学习路线；已经确认的行为约束以 [Runtime 决策台账](./.scratch/astra-scheduler-runtime/decision-log.md) 和 [ADR](./docs/adr/) 为权威来源。后续版本 spec 与 Tickets 必须引用稳定的 `D-xxx`，不得只依赖本文示例或聊天上下文。

---

## 1. 项目目标

### 1.1 核心目标

AstraScheduler 主要解决以下问题：

1. 降低传统全局任务队列在线程数增加时产生的锁竞争。
2. 通过 Work Stealing 实现 Worker 之间的动态负载均衡。
3. 支持任务依赖关系，使线程池升级为 Task Runtime。
4. 支持任务返回值、异常传播、取消与生命周期管理。
5. 支持 C++20 Coroutine，使阻塞式任务模型进一步扩展为可挂起任务模型。
6. 提供运行时指标与 Trace，能够“看见”调度器如何工作。
7. 使用系统化 Benchmark 与普通线程池、`std::async`、oneTBB 等方案进行对比。

### 1.2 项目最终定位

项目最终不应描述为：

> 一个基于 C++ 实现的线程池。

而应描述为：

> **AstraScheduler is a C++20 task scheduling runtime featuring work stealing, DAG execution, cooperative cancellation, coroutine scheduling, runtime tracing and benchmark-driven optimization.**

中文可表述为：

> 基于 C++20 实现的高性能任务调度运行时，以 Chase-Lev Work-Stealing Deque 为核心调度结构，支持 DAG 任务依赖、Future/异常传播、协作式取消、Coroutine 调度、运行时追踪以及多工作负载性能评测。

---

# 2. 设计原则

整个项目遵循以下原则。

## 2.1 调度核心与任务抽象分离

线程池只负责“线程”，调度器负责“任务什么时候在哪执行”。

因此不要让 `Worker`、`Task`、`Scheduler`、`Future` 相互强耦合。

建议分层：

```text
Application
    │
    ▼
Task / TaskGraph / Coroutine API
    │
    ▼
Scheduler
    │
    ├── Global Injection Queue
    ├── Local Work-Stealing Deque
    ├── Priority / Deadline Policy
    └── Wake-up Policy
    │
    ▼
Worker Runtime
    │
    ▼
OS Threads
```

---

## 2.2 正确性优先于无锁

不要为了“无锁”而无锁。

项目中推荐：

- Local Queue：实现 Chase-Lev Work-Stealing Deque。
- Global Injection Queue：第一版允许使用 mutex + queue。
- DAG 元数据：使用原子计数器 + 小粒度锁。
- Metrics：使用原子变量或 thread-local 聚合。

只有当 Benchmark 证明某个锁确实成为瓶颈时，再进行无锁优化。

---

## 2.3 每个高级特性都必须能够解释“为什么需要”

例如：

| 功能 | 解决的问题 |
|---|---|
| Work Stealing | Worker 负载不均衡、全局队列竞争 |
| DAG | 任务之间存在依赖，不应手写 future.get() 链 |
| Coroutine | I/O 等待期间不长期占用 Worker |
| Cancellation | 长任务、超时任务需要协作停止 |
| Priority | 真实系统中的任务并非完全同优先级 |
| Trace | 并发调度问题难以仅通过日志分析 |
| Benchmark | 证明设计收益，而不是口头声称高性能 |

---

# 3. 功能范围划分

为了避免项目失控，将功能分成三个等级。

## 3.1 Core：基础里程碑必须完成

这些功能决定项目是否成立。

- [ ] 固定 Worker Group
- [ ] Global Injection Queue
- [ ] Per-Worker Local Queue
- [ ] Chase-Lev Work-Stealing Deque
- [ ] Worker 随机/轮询 Victim Stealing
- [ ] 泛型 `submit()`
- [ ] Future / Result
- [ ] 异常传播
- [ ] Graceful Shutdown + Shutdown Completion
- [ ] Scheduler Handle / Runtime State 生命周期分离
- [ ] 进程级 Reaper Service + Finalization Control
- [ ] 基础 Metrics
- [ ] Benchmark Framework
- [ ] 单元测试 + 并发压力测试

---

## 3.2 Advanced：v1.0 前必须完成

这些功能在早期里程碑之后纵向加入，但同样属于已批准的 v1.0 最终范围，不是可随意删除的候选项。

- [ ] Task Graph / DAG
- [ ] Dependency Counter
- [ ] Task Handle
- [ ] Cooperative Cancellation
- [ ] `std::stop_token`
- [ ] Immediate Shutdown + Finalization Escalation
- [ ] C++20 Coroutine
- [ ] Chrome Trace Event 导出
- [ ] Priority Scheduling
- [ ] Deadline Scheduling

---

## 3.3 Optional：有时间再做

这些功能容易把项目拖得过大，不建议一开始实现。

- [ ] Dynamic Worker Scaling
- [ ] CPU Affinity
- [ ] NUMA-aware Scheduling
- [ ] Lock-free Global Queue
- [ ] Timer Wheel
- [ ] Async Socket / io_uring
- [ ] Distributed Runtime

其中 **Adaptive Worker Scaling 不应作为第一优先级**。对于 CPU-bound 场景，线程数量动态增加通常没有意义，反而可能增加上下文切换。

---

# 4. 总体系统架构

```text
                         User Application
                                │
         ┌──────────────────────┼──────────────────────┐
         │                      │                      │
         ▼                      ▼                      ▼
      submit()             Task Graph              spawn()
   Callable/Future           DAG                  Coroutine
         │                      │                      │
         └──────────────────────┼──────────────────────┘
                                ▼
                     ┌─────────────────────┐
                     │  Scheduler Handle   │
                     └─────────┬───────────┘
                               │ shared lifetime
                               ▼
                     ┌─────────────────────┐
                     │    Runtime State    │◄──────────────┐
                     └─────────┬───────────┘               │
                               │                           │
                External Task  │                     handoff / join
                               ▼                           │
                  ┌────────────────────────┐               │
                  │ Global Injection Queue │        ┌──────┴─────────┐
                  └────────────┬───────────┘        │ Reaper Service │
                               │                    │ 1 coordinator  │
            ┌──────────────────┼───────────────────┐└────────────────┘
            ▼                  ▼                   ▼
      ┌────────────┐     ┌────────────┐      ┌────────────┐
      │  Worker 0  │     │  Worker 1  │      │  Worker N  │
      ├────────────┤     ├────────────┤      ├────────────┤
      │ LocalDeque │     │ LocalDeque │      │ LocalDeque │
      └──────┬─────┘     └──────┬─────┘      └──────┬─────┘
             │                  │                   │
             └──────────── Work Stealing ──────────┘
                               │
                               ▼
                     Execute / Suspend / Resume
                               │
              ┌────────────────┼─────────────────┐
              ▼                ▼                 ▼
           Future          Task Graph         Coroutine
           Result          Release Next       Resume
                               │
                               ▼
                    Metrics + Chrome Trace
```

`Scheduler Handle` 是用户入口，不是 Worker Runtime 的唯一生命周期载体。`Runtime State` 被 Handle、Worker 执行路径和 Reaper 回收路径安全共享；这使最后一个 Handle 即使在本 Scheduler Worker 上析构，也不会造成 self-join 或 use-after-free。Reaper Service 是进程级控制面，不执行用户任务，也不参与 work stealing。

---

# 5. 核心调度思想

## 5.1 为什么不能只使用一个全局任务队列

传统线程池：

```text
Worker 0 ─┐
Worker 1 ─┤
Worker 2 ─┼── mutex ── Global Queue
Worker 3 ─┤
Worker N ─┘
```

线程数量较少时问题不明显，但当任务很短、Worker 很多时，多个线程会频繁竞争同一把锁。

典型问题：

- mutex contention
- cache line bouncing
- Worker 唤醒后仍需要争锁
- 微任务场景下调度成本接近甚至超过任务执行成本

---

## 5.2 Work Stealing 模型

每个 Worker 拥有自己的 Local Queue：

```text
Worker 0 → LocalDeque 0
Worker 1 → LocalDeque 1
Worker 2 → LocalDeque 2
Worker 3 → LocalDeque 3
```

Worker 优先执行自己的任务。

```text
Local Queue 非空
      │
      ▼
   pop local
      │
      ▼
   execute
```

自己的任务为空：

```text
Local Queue empty
      │
      ▼
select victim
      │
      ▼
steal from victim
```

---

# 6. Chase-Lev Work-Stealing Deque

这是整个项目最值得深入实现的核心数据结构。

## 6.1 基本模型

一个 LocalDeque 只有一个 Owner，但可以有多个 Thief。

```text
                     top
                      │
      thief steal → [A][B][C][D] ← owner push/pop
                                  │
                                bottom
```

Owner：

```text
push_bottom()
pop_bottom()
```

Thief：

```text
steal_top()
```

---

## 6.2 为什么 Owner 从 bottom 操作

Owner 通常优先执行最近生成的任务：

```text
Task A
  ├── Task B
  └── Task C
```

如果 Worker 正在执行 A，然后生成 B、C，优先执行刚产生的任务通常具有更好的：

- cache locality
- stack/data locality
- 工作集局部性

因此 Owner 使用近似 LIFO 策略。

---

## 6.3 为什么 Thief 从 top 偷

Thief 从另一端读取：

- 避免和 Owner 高频操作同一端。
- 偷较旧的任务，减少与 Owner 当前局部工作集冲突。
- 适合递归任务的负载扩散。

---

## 6.4 核心成员

初始版本可以设计为：

```cpp
class WorkStealingDeque {
public:
    void push(Task* task);
    Task* pop();
    Task* steal();

private:
    std::atomic<std::size_t> top_{0};
    std::atomic<std::size_t> bottom_{0};
    CircularBuffer buffer_;
};
```

这里真正需要深入研究的是：

- `top` 与 `bottom` 的内存序。
- 最后一个元素时 Owner 与 Thief 的竞争。
- CAS 为什么必要。
- resize 后旧 Buffer 的生命周期。
- 为什么不能简单释放旧 Buffer。

Phase 3 先实现并保留内部 seq_cst oracle，再启用基于 Lê 等人 2013 portable C11 证明路径的 C++20 production variant；不依赖 x86/ARM 手写汇编。Production 固定 push 的 cell→release fence→bottom publication、pop/steal 的 seq_cst fence 与 last-item/top CAS，array 的 consume 读取提升为 acquire。任何进一步弱化 memory order 都需要新的证明、litmus/stress evidence 与决策。

Buffer 只按 power-of-two 双倍增长，运行期不 shrink、不回收、不复用旧地址；所有 generation 保留到 Worker/deque quiescent 且 Runtime join 后统一释放，因此历史 buffer 总容量小于 active capacity 的两倍。Local resize 失败时，已接受 Ready Task 进入内嵌 intrusive link 的 allocation-free Global fallback，不得丢失或 inline 执行。

每个逻辑 Ready entry 只拥有一个 Scheduling Reference；resize cell copy 不复制 ownership，losing thief 在 top CAS 成功前不得解引用 raw TCB pointer。Deque 内部区分 Success、Empty、Retry。`uint64_t` index 在高水位前进入 active-thief 双检保护的 quiescent rebase，避免 wrap/ABA；因此 lock-free 只描述支持平台上的正常 fast path，resize/rebase 和带锁 fallback不作虚假保证。

C++ 实现不得直接对空状态执行 unsigned `bottom - 1`：`bottom == 0` 先返回 Empty。Logical size、`capacity - 1` 与 doubling 全部 checked；在 `size >= capacity - 1` 时 grow，始终留一个 unused cell。Capacity/grow 无法安全扩大时保持原 deque 不变并走 Global fallback。

---

# 7. 任务来源分类

任务来源应该区分为两类。

## 7.1 External Submission

用户线程调用：

```cpp
scheduler.submit(task);
```

此时调用线程不是 Worker。

任务进入：

```text
Global Injection Queue
```

在启用 Work Stealing 的版本中，off-Worker 的 DAG release、Coroutine resume 与 event/timer wake 同样走 Global Injection Queue。

然后由空闲 Worker 获取。

---

## 7.2 Internal Submission

Worker 在执行任务过程中继续产生任务：

```cpp
scheduler.submit(child_task);
```

如果当前调用线程属于 Scheduler 的 Worker，则新任务优先进入：

```text
Current Worker LocalDeque
```

同Runtime Worker上的same-Runtime Internal Submission进入owner LocalDeque；普通DAG release、TaskHandle/GraphRun await completion等无专用规则的Ready publication在owner Worker上走Local、off-Worker走Global。专用规则优先：`yield`和timer resume强制ordinary Global，首次Deadline work进入Global EDF。v0.1.0为保留全局队列基线，所有Ready Task仍统一进入Global Injection Queue。

这样可以避免每次都访问 Global Queue。

因此需要：

```cpp
thread_local WorkerContext* current_worker;
```

判断调用者是否为内部 Worker。

---

# 8. Worker 调度循环

推荐的 Worker 主循环逻辑：

```text
while scheduler is running
        │
        ▼
1. 在 local burst 未到上限时 pop LocalDeque
        │
   success? ───── yes ───→ execute
        │ no
        ▼
2. poll Global Injection Queue（每 64 个连续 local task 强制一次）
        │
   success? ───── yes ───→ execute
        │ no
        ▼
3. steal from another Worker
        │
   success? ───── yes ───→ execute
        │ no
        ▼
4. backoff / sleep
        │
        ▼
5. wait for notification
```

对应伪代码：

```cpp
void Worker::run() {
    while (!scheduler_.should_stop()) {
        Task* task = local_queue_.pop();

        if (!task)
            task = scheduler_.try_pop_global();

        if (!task)
            task = scheduler_.try_steal(*this);

        if (task) {
            execute(task);
            continue;
        }

        scheduler_.wait_for_work(*this);
    }
}
```

---

# 9. Stealing 策略

不建议固定：

```text
Worker 0 永远偷 Worker 1
Worker 1 永远偷 Worker 2
```

容易造成热点。

基础 victim policy 固定为：

```text
Worker-private pseudo-random victim permutation
```

即：

```cpp
victim = random_worker_except_self();
```

一次 steal round 默认最多探测：

```text
min(N - 1, 8) 个互不重复 Victim
```

通过 `SchedulerOptions::steal_probe_limit` 调整，必须大于 0；N=1 不产生 steal attempt。Phase 2 带锁 Local Deque 与 Phase 3 Chase-Lev 都保持 owner bottom push/pop、thief top steal 的相同语义。

而不是无限自旋。

未来可以研究：

- random victim
- round-robin victim
- power-of-two choices
- topology-aware victim

并通过 Benchmark 比较。

---

# 10. Worker 空闲策略

Worker 没任务时不能无限 busy loop。

推荐采用分阶段 Backoff：

```text
Stage 1: 短暂 spin
        ↓
Stage 2: yield
        ↓
Stage 3: sleep / condition_variable
```

原因：

- 任务可能很快到达，立即 sleep 会增加唤醒延迟。
- 一直 spin 又会浪费 CPU。

可设计：

```cpp
enum class IdleState {
    Spinning,
    Yielding,
    Sleeping
};
```

这一模块可以作为一个独立 Benchmark 主题。

active backoff 必须有界，之后进入可通知 park；具体 pause/yield 次数保持内部 benchmark-tuned，不进入稳定 SchedulerOptions。正确性使用统一 publication epoch：publisher 按 `publish state/work → advance epoch → notify`，Worker 在 park intent 前后双检 queue、Shutdown 与 epoch，只有仍无工作且 generation 未变时才阻塞；固定宽度 epoch 饱和后禁用 park 或进入等价无 ABA slow path。

单个 Ready publication 至少唤醒一个 parked Worker，batch 按可并行 work 逐步唤醒；owner-local publication 也通知以允许 steal。Shutdown mode/exit 等要求所有 Worker 重检的控制面变化使用 notify-all。通知 fanout 是性能提示，epoch/predicate 才是无丢失唤醒的正确性来源。

---

# 11. Task 抽象

不建议把核心任务永久固定为：

```cpp
std::function<void()>
```

因为它可能产生：

- type erasure 开销
- heap allocation
- 小任务下较明显的额外成本

从 v0.1.0 起，`submit/try_submit` 就必须 decay-capture Callable 与参数并以 stored rvalue 恰好调用一次，支持 move-only target、move-only 参数和 `operator()&&`；真实引用必须显式使用 `std::ref`。因此稳定任务路径不能用 copy-only `std::function<void()>` 缩窄能力。内部可以使用自定义 move-only type erasure 或 templated TCB，Small Function Optimization、inline size 与 allocator 留作不改变语义的后续优化。

Task 的逻辑结构：

```cpp
struct Task {
    TaskId id;
    TaskState state;
    Priority priority;
    MoveOnlyWork work; // 概念性内部类型，不进入 public API
};
```

实际工程中建议把运行时元数据与 Callable 分离。

---

# 12. Task 状态机

稳定公共状态固定为：

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

状态变化：

```text
Waiting ───── dependencies ready ─────→ Ready
                                         │
                                         ▼
                                      Running
                                    /    │    \
                                   /     │     \
                                  ▼      ▼      ▼
                            Suspended Succeeded Failed
                                │
                              resume
                                │
                                └────────→ Ready

Waiting / Ready
      │
   cancel
      │
      ▼
  Cancelled
```

成功提交的普通独立 Task 最早可观察为 `Ready`；`Created`、enqueue publication、queue claim、start/cancel arbitration 和 Outcome publishing 都是内部瞬态，不进入稳定公共枚举。Value/Exception/Cancelled Terminal Outcome 分别与 `Succeeded/Failed/Cancelled` 同一次 completion publication 对外可见，终态发布后不可逆。Cancellation Request 本身不是状态，Running Task 收到 stop request 后仍保持 `Running`，直至真实退出发布终态。

`TaskHandle::state()` 返回非阻塞、无副作用的线性化快照；非终态返回后可以立即过时，不能用于原子 check-then-act。Coroutine 的详细 Ready/Running/Suspended 转移以及 Suspended 取消在 Coroutine 章节继续固定。

---

# 13. submit() API

目标接口：

```cpp
astra::Scheduler scheduler;

auto task = scheduler.submit([](int a, int b) {
    return a + b;
}, 10, 20);

std::cout << task.get();
```

Runtime admission rejection通过`submission_rejected::reason()`稳定区分`Stopping/Stopped/CapacityExhausted`；空/moved-from Scheduler属于`logic_error`，Finalization后创建Scheduler则抛独立`scheduler_creation_rejected(FinalizationStarted)`。配置、allocation与capture construction异常保持原类型。需要把预期submission rejection作为值处理时使用：

```cpp
template<class T>
using SubmissionResult =
    std::variant<TaskHandle<T>, SubmissionError>;

auto result = scheduler.try_submit(work);
```

`try_submit()` 无论配置何种 Backpressure 都不等待容量；只把 runtime admission rejection 放入 error alternative，其他异常仍可抛出。`submit()`/`try_submit()` 共享强异常安全 admission transaction：成功必定已经形成被 Runtime 核算且不可丢失的 Task，失败完全回滚 Runtime slot/outstanding/publication，绝不返回 orphan Handle。Task 可以由正常 Worker 在 submit 返回前开始或完成，但这不属于 CallerRuns。

需要支持：

```text
void
int
double
std::string
move-only object
user-defined type
```

从 v0.1.0 起，公开返回类型固定为：

```text
TaskHandle<T>
```

早期内部实现仍可以使用：

```text
packaged_task / promise
```

但`std::future<T>`不作为稳定公共返回类型。Cancellation、TaskState、TaskGraph与Coroutine统一扩展同一个`TaskHandle<T>`；其复制、结果观察、等待和取消语义均由决策台账固定。

---

# 14. 异常传播

任务执行不能因为一个异常导致 Worker 线程退出甚至 `std::terminate()`。

```cpp
auto future = scheduler.submit([] {
    throw std::runtime_error("task failed");
});
```

Worker 必须捕获 Callable 逃逸出的任意 C++ 异常，并通过 `std::exception_ptr` 把它保存为共享、不可变的 Exception Outcome；异常不能越过 Worker entry。任务值获取操作会按原始动态类型重新抛出，多个 Handle 可以重复观察同一个异常事实：

```cpp
try {
    task.get();
} catch (const std::exception& e) {
    // receive task exception
}
```

非`std::exception`类型的C++异常也通过`std::exception_ptr`保存和重抛。`wait()`只观察完成，`task_cancelled`表示取消Outcome，未观察异常只按启用状态进入Metrics/Trace。

统一结果模型可以抽象为：

```text
Task Result
   │
   ├── Value
   ├── Exception
   └── Cancelled
```

Task 进入终态后，上述三类之一会成为共享、不可变的 Terminal Outcome。所有有效 `TaskHandle<T>` 副本重复观察同一个 Outcome，观察不会消费或改写它。

Value Outcome 通过 `TaskHandle<T>::get() const &` 返回共享 `const T&`；`TaskHandle<void>::get() const &` 返回 `void`，两个 rvalue overload 均删除。基础 Interface 不提供消费式 `take()`。返回引用的生命周期依赖至少一个 Handle 继续持有完成状态，不能从临时 Handle 取得引用后跨越其生命周期保存。

普通非AstraScheduler Worker在Task未完成时调用`get()`，会执行Unbounded Wait：没有timeout，只在真实Terminal Outcome发布后返回或传播对应Outcome，并且等待本身不请求取消。它是最基础、完成语义最强的同步接口；Worker使用Helping Wait，有界同步使用`wait_for`，Astra coroutine使用cancellation-aware `co_await`。

Failed Task 的异常即使从未`get()`，也不触发terminate、默认日志、用户回调、级联取消或终态改写。第一次`get()`/TaskHandle await传播Exception Outcome前幂等标记`exception_observed`；completion shared state最终释放时若仍未观察，只在Metrics Basic/Detailed增加`unobserved_failures`，并在活动Trace capture可用时尽力发diagnostic。Metrics Off/Trace disabled不维护隐藏输出；`state()/wait()/wait_for()`不算观察原始异常。

同Scheduler Worker等待该Runtime的另一个未完成Task时采用Helping Wait：外层Callable保留在栈上，Worker继续通过同Runtime的正常调度路径执行Eligible Task，直到目标Terminal Outcome发布。v0.1.0从Global Injection Queue帮助执行，后续版本复用normal local/global/steal顺序；不创建补偿线程。Direct Self-Wait拒绝，一般Indirect Wait Cycle不在线检测。

当前Task对表示自身Task Identity的任一Handle副本调用`get()`属于Direct Self-Wait，必须在进入等待、Helping或改变状态之前抛出`std::logic_error`。若Callable不捕获它，该异常按普通用户异常规则成为当前Task的Exception Outcome。跨Runtime等待只帮助source Runtime，绝不执行target Runtime work。

Runtime 不维护通用动态 wait-for graph，也不保证检测 A waits B、B waits A 一类 Indirect Wait Cycle；这种环可以让相关 Task 永久 Helping Wait。需要执行前无环保证的显式依赖使用 TaskGraph/DAG cycle detection。Metrics 与 Trace 可以帮助诊断等待链，但启发式诊断不改变 Task 终态。

一个Scheduler Worker等待另一Runtime的Task时采用Cross-Runtime Helping Wait：只通过正常调度路径帮助自己的源Runtime，并观察目标Terminal Outcome；不得执行、窃取或内联目标Runtime的任务。跨Runtime等待环不保证检测；Helping始终服从source Runtime当前Graceful/Immediate eligibility，target shutdown不转移ownership。

---

# 15. Task Handle

`TaskHandle<T>` 是 `submit()` 从首个版本起返回的稳定公共任务抽象：

它可复制、可移动；所有有效副本表示同一个Task Identity和同一个完成状态，复制不会重新提交或重新执行任务。最后一个Handle销毁只放弃观察与控制capability，不隐式取消任务；已经接受的Unobserved Task仍由Runtime推进到终态。并发只读/取消调用安全，结果通过左值`get()`共享观察。

```cpp
auto handle = scheduler.submit(...);
```

确认的基础能力为：

```cpp
handle.wait();
handle.get();
handle.request_cancel();
handle.state();
handle.id();
```

设计：

```cpp
template <typename T>
class TaskHandle {
public:
    TaskHandle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    const T& get() const &;
    const T& get() const && = delete;
    void wait() const;

    template<class Rep, class Period>
    [[nodiscard]] TaskWaitResult
    wait_for(std::chrono::duration<Rep, Period> timeout) const;

    void request_cancel() const noexcept;
    [[nodiscard]] TaskState state() const;
    [[nodiscard]] TaskId id() const;
};
```

`TaskHandle<void>`的`get() const &`返回`void`，同样禁止rvalue调用。`get()`、`wait()`、`wait_for()`、`request_cancel()`、`state()`与`id()`均为确认Interface。公共强类型`RuntimeId`、`TaskId(RuntimeId, sequence)`和`GraphRunId`复用到Handle、Metrics、Trace和GraphReport；它们不暴露地址、不提供全局lookup/control，0为invalid且sequence不wrap复用。

Callable 的裸引用结果在编译期拒绝；显式 non-owning 结果使用 `std::reference_wrapper`，owning 结果使用智能指针等值类型。`submit()` 支持 void、copyable 和 move-only object result，去除结果的顶层 cv；首个稳定 API 拒绝完全 immovable object result。`get()` 仅允许左值 Handle，避免临时 Handle 返回悬垂引用；保留/复制 Handle 就是结果 owning view，不另设 `take()` 或独立 Result View。

TaskHandle 具有显式空状态：默认构造为空，move 转移 Task 关联并使源 Handle 为空，`valid()` 只查询是否关联 Task。空 Handle 上的结果、等待、状态与身份操作在任何副作用前抛出 `std::logic_error`；`request_cancel() noexcept` 对空 Handle 是无副作用 no-op，绝不把空状态伪装成 Cancelled 或 Completed。

同一 Handle 对象未被并发 move/赋值/swap/析构时，多个线程可在同一对象或不同副本上并发调用全部只读方法和 `request_cancel()`。同一 Handle 的 reassociation/lifetime mutation 与成员访问仍需调用方同步；这不影响其他副本。多个等待者共享同一次 completion publication，不丢失完成，但 Runtime 不承诺唤醒顺序、公平性或从唤醒到运行的最大延迟。

稳定公共 API 不再增加 `try_get()`、`exception()`、variant-like `OutcomeView` 或独立 Result View。`state()` 负责非阻塞类别观察，`wait/wait_for` 负责完成同步，左值 `get()` 负责唯一的 Value/Exception/Cancelled 传播，复制 Handle 负责延长结果生命周期。

`void wait() const` 只同步到真实 Terminal Outcome，Value、Exception、Cancelled 都正常返回且不消费结果；随后 `get()` 仍能完整观察 Outcome。非 Worker 执行 Unbounded Wait，同/跨 Runtime Worker 复用 Helping Wait，Direct Self-Wait 抛出 `std::logic_error`。

`wait_for(timeout)` 返回 `TaskWaitResult::Completed` 或 `TaskWaitResult::TimedOut`，同样不传播或消费 Outcome；超时不取消 Task。它使用 `steady_clock`、非正 duration 即时观察和唯一的完成—期限线性化顺序。正 duration 下 Worker 继续 Helping Wait，但 helped Callable 非抢占，因此 deadline 是下一次观察边界而不是硬实时返回上限，实际回程可能越过 timeout。

每个Worker的Helping Wait嵌套由`SchedulerOptions::max_helping_depth`限制，默认64且必须大于0；超过阈值会在启动下一层帮助前抛出`astra::helping_depth_exceeded`，不改变目标Task。Helping loop始终服从source Runtime eligibility：Graceful只推进Drain Work Closure；Immediate不first-start任何新Task，但允许already-started Coroutine的resume segment到合作取消/自然完成；跨Runtime等待绝不借Shutdown绕过ownership。

---

# 16. Cooperative Cancellation

线程池无法安全“强杀”正在执行的 C++ 函数。

因此采用协作式取消：

```cpp
scheduler.submit([](std::stop_token token) {
    while (!token.stop_requested()) {
        do_work();
    }
});
```

`submit(F, Args...)` 优先选择普通 `F(Args...)` invocation；只有普通形式不可调用而 `F(std::stop_token, Args...)` 可调用时，才把该 Task 的 token 注入首参数。两种形式均可调用时不注入，避免 generic Callable 因隐藏参数改变行为。

取消：

```cpp
handle.cancel();
```

内部：

```text
TaskHandle
    │
    ▼
stop_source.request_stop()
    │
    ▼
Task observes stop_token
```

区分：

单 Task Cancellation Request 与 Task start 必须形成唯一线性化顺序。请求先胜出时，尚未进入 `Running` 的已接受任务发布 `Cancelled` Terminal Outcome；start 先胜出时，只向 Running Task 发布 cooperative stop request。Terminal Task 上的重复请求是无副作用 no-op。

公共命令固定为 `void request_cancel() const noexcept`。它可以由任意应用线程通过不同 Handle 副本并发调用，只在请求可靠发布后返回，不等待 Task Terminal Outcome；调用方通过结果/等待 Interface 观察最终完成。

Running Task 的 stop request 只表达意图：Callable 正常返回仍发布 Value，普通异常仍发布 Exception；只有显式 cooperative cancellation signal 才发布 Cancelled。Runtime 不得在 Callable 返回后仅根据 `stop_requested()` 覆盖已经产生的结果。

公开 `astra::task_cancelled` 同时承担 Cancelled Outcome 的 `get()` 报告和 cooperative Cancellation Signal。`get()` 在 Cancelled 上重复抛出该类型；它若未被 Callable 捕获并逃出 execution boundary，则 Task 发布 Cancelled 而不是 Exception。Callable 捕获后正常返回仍发布 Value。

`void astra::throw_if_stop_requested(std::stop_token token)` 提供显式安全点：本次观察到 stop request 时抛出 `task_cancelled`，否则立即返回。它只使用传入 token，不读取 TLS，也不产生新的取消请求。

### 未执行任务

可以直接从逻辑层标记为：

```text
Cancelled
```

### 正在执行任务

只能发送取消请求：

```text
Running → stop requested → Task cooperative exit
```

---

# 17. Task Graph / DAG

这是项目从 ThreadPool 升级为 Runtime 的关键部分。

## 17.1 示例

```text
        A
       / \
      B   C
       \ /
        D
        │
        E
```

含义：

```text
B depends on A
C depends on A
D depends on B and C
E depends on D
```

---

## 17.2 API 示例

```cpp
auto A = graph.emplace(load_data);
auto B = graph.emplace(parse_data);
auto C = graph.emplace(validate_data);
auto D = graph.emplace(process_data);
auto E = graph.emplace(save_result);

A.precede(B, C);
B.precede(D);
C.precede(D);
D.precede(E);

auto frozen = std::move(graph).freeze();
auto run = scheduler.run(std::move(frozen));
run.wait();
```

`TaskGraph` 是 caller-serialized 的 move-only builder；`emplace(...)`/`emplace_coroutine(...)` 返回 graph-local 强类型 `NodeId`，`freeze() &&` 消费 builder 并验证 foreign node、self edge、duplicate edge 与 cycle，Cycle error 携带确定 NodeId witness。NodeId 的 0 值 invalid，合法值按插入顺序 checked-monotonic 分配且 freeze 不重编号；历史文字 `GraphNodeId` 不构成第二个公共类型。`FrozenTaskGraph` 结构不可变且只能被 `run()` 消费一次，从而支持 move-only Callable 而不引入 graph clone/factory 语义。空 graph 合法且立即完成。

External GraphRun 原子按全部 Node 占用 External Pending slot，因为 Waiting Node 同样占内存；`node_count > capacity` 即使 Block 也立即拒绝，避免等待不可能条件。same-Runtime Internal Graph 豁免 external slot，但所有 Node 仍属于 Drain Work Closure。Graph admission all-or-nothing。

Node 是返回 `void` 的控制任务，不提供自动 typed dataflow或 per-node TaskHandle；数据通过显式共享对象/外部 typed Task传递。Edge 默认 `RequireSuccess`，也可显式 `AfterCompletion`。Failed/Cancelled 只把 RequireSuccess descendants 传播为 Cancelled，independent branch与 AfterCompletion continuation继续；全图取消必须显式 `GraphRun::request_cancel()`。

GraphRun是copyable共享capability，提供state/wait/wait_for/get_report/request_cancel。全部Node Terminal时发布按NodeId排序的不可变GraphReport；聚合状态优先级Failed > Cancelled > Succeeded，report保留所有真实Failed exception_ptr但不自动重抛。`get_report()`或GraphRun await返回report时标记全部真实Node exceptions已观察；仅wait/state不标记，最终未观察诊断按真实Failed Node计数。Graph Node等待所属GraphRun属于direct self-run并抛logic_error；其他Worker复用source Helping Wait。

---

## 17.3 Dependency Counter

每个 TaskNode：

```cpp
struct TaskNode {
    std::atomic<std::size_t> unresolved_dependencies;
    std::vector<TaskNode*> successors;
};
```

例如 D：

```text
D dependencies = 2
```

B 完成：

```text
2 → 1
```

C 完成：

```text
1 → 0
```

此时：

```text
D becomes Ready
     │
     ▼
enqueue(D)
```

核心算法：

```cpp
for (auto* successor : task->successors) {
    if (successor->remaining.fetch_sub(1) == 1) {
        schedule(successor);
    }
}
```

实际 publication 顺序必须是 predecessor Terminal Outcome 先完整发布，再为每条唯一 edge 写 disposition 并 decrement；只有观察到 1→0 的线程成为 successor release owner，并以 acquire 汇合全部 predecessor 结果后恰好一次决定 Ready 或传播终态。GraphRun 只在全部 Node Terminal 后完成。

---

# 18. DAG 正确性检查

TaskGraph 至少需要考虑：

- 环检测
- 重复边
- Graph 生命周期
- Node 生命周期
- Graph 运行期间是否允许修改

固定规则：

> Graph 构建完成后以 consuming `freeze() &&` 形成单次执行 FrozenTaskGraph，运行期间禁止修改。

例如：

```cpp
graph.freeze();
scheduler.run(graph);
```

这能大幅减少并发状态复杂度。

---

# 19. C++20 Coroutine Integration

Coroutine 是进阶模块，不建议早于 Work Stealing 和 DAG。

目标接口：

```cpp
Task<int> async_job() {
    auto value = co_await some_operation();
    co_return value * 2;
}
```

Scheduler：

```cpp
auto task = async_job();
auto result = scheduler.spawn(std::move(task));
```

`astra::Task<T>` 是cold、move-only、single-shot frame owner：`initial_suspend = suspend_always`，函数调用不执行body。未spawn Task析构destroy初始suspended frame；spawn成功才把frame转给Runtime并返回统一TaskHandle，rejection/构造失败保留source Task有效可重试。`try_spawn`与try_submit一样不等待capacity。

Promise `final_suspend() noexcept` always-suspend。co_return/unhandled_exception先构造TCB Terminal Outcome，唯一resume owner到达final suspend后发布completion，再由Runtime恰好一次destroy frame；TaskHandle结果独立于frame存活。

---

# 20. Coroutine 调度模型

```text
Worker
  │
  ▼
resume coroutine
  │
  ├──── not suspended ───→ finish
  │
  └──── co_await
           │
           ▼
       Suspended
           │
      event completed
           │
           ▼
      enqueue handle
           │
           ▼
         Ready
           │
           ▼
      another Worker
           │
           ▼
         resume
```

核心思想：

> Coroutine Handle 本质上也可以作为一种 Ready Task 被 Scheduler 调度。

这样 Callable Task 和 Coroutine Resume Task 最终能够进入统一调度层。

每次resume segment仍属于同一Task Identity：Ready ticket被唯一Worker claim后才能resume，普通await提交Suspended，completion只竞争把该suspension generation重新发布为一个Ready ticket，绝不inline/concurrent resume。内建awaiter使用register→suspend commit→arm/trigger handshake覆盖completion与await_suspend竞态。

Cancellation对Suspended coroutine仍是cooperative：Astra内建awaiter把stop作为并列trigger，stop胜出时在source Runtime排队resume，并由await_resume抛`task_cancelled`；任意foreign awaitable没有统一注销协议，Runtime只能设置stop request并等待自然resume，可能永久Pending，不能强毁frame或伪造Cancelled。Immediate只禁止NeverStarted work的first start；已经开始的Coroutine仍可resume到取消点/自然完成并执行RAII，用户捕获取消后继续也必须被允许。

左值TaskHandle和GraphRun可在Astra-managed Task内`co_await`，rvalue deleted以保护共享引用生命周期。Await不阻塞Worker，target completion只在source Runtime发布resume ticket；TaskHandle传播同一Value/Exception/Cancelled，GraphRun返回aggregate GraphReport。Direct self-await/self-run在注册前拒绝。Task<T>本身不可直接co_await，必须显式spawn取得Handle。

`co_await astra::cancellation_point()`只检查stop且不挂起；`co_await astra::yield()`在无stop时真正suspend并强制把同一Task发布到source Global Queue，给其他工作service opportunity。核心API不再增加wait_until、stop-token blocking wait或任意completion callback。

TaskGraph通过显式`emplace_coroutine(Task<void>&&)`把cold frame作为同一个void Node执行；不创建child Task或额外slot，Suspended/Resume与Graph cancel/report复用同一Node状态。

Coroutine定时等待提供`astra::sleep_for(duration)`和仅接受`steady_clock::time_point`的`astra::sleep_until`。非正duration/已到期时间不真实挂起，但仍作为cancellation point。实际等待使用Runtime State拥有的indexed min-heap；Worker在scheduler loop和park前处理到期项，park deadline取最近Wake Time，新最早项通过work epoch通知parked Worker。项目不为timer创建额外线程。

到期与stop通过同一suspension generation竞争唯一winner，胜者只向source Runtime Global Queue发布一次resume ticket；stop获胜会O(log n)主动删除timer，避免长期tombstone。Timer Wake Time仅是“不早于此时重新Ready”的best-effort eligibility边界，不是Priority/Deadline中的Task Deadline。Timer属于原Task及Drain Work Closure，不重复占slot/outstanding；Graceful保留原Wake Time，Immediate请求取消，因此长期sleep可能使无界关停长期等待。

---

# 21. Priority Scheduling

Priority采用四级不可变base scheduling hint：

```cpp
enum class Priority : std::uint8_t {
    Low,
    Normal,
    High,
    Critical
};
```

Global Injection Source和每个Worker Local Source都按四个band分区：

```text
Critical Queue
High Queue
Normal Queue
Low Queue
```

Local的每个band都是独立的locked/Chase-Lev owner-bottom、thief-top deque，Global每个band保持MPMC FIFO。D-091的Local burst/Global probe仍是外层source fairness；在获准探测的source内部用`Critical:High:Normal:Low = 8:4:2:1`的work-conserving加权日历选择band，空band立即fallback。这样Critical获得更高service share，而Low持续Eligible时仍有正向service opportunity。

无options的External Task为Normal；same-Runtime Internal Task默认继承current Task base Priority，显式`TaskOptions`覆盖默认。Priority在admission时固定，Callable、Coroutine resume与Graph Node统一使用；TaskHandle不支持动态reprioritize或wait-time donation。Priority只选择下一个Ready Task，绝不抢占Running Callable/resume segment，也不映射为OS thread priority或实时权限。

---

# 22. Deadline Scheduling

Deadline是`TaskOptions`中的可选强类型首次开始目标，只接受`steady_clock`：

```cpp
TaskOptions options{
    .priority = Priority::High,
    .deadline = TaskDeadline::after(10ms),
};
auto handle = scheduler.submit(options, task);
```

`TaskDeadline::after`在factory调用时就固定绝对时间；它不是延迟执行，Task在Ready后可以提前运行。首次成功进入Running时采样start time，晚于Deadline即记录miss。Deadline不继承、不动态修改；Coroutine只在首次resume判断，Graph Node即使因依赖等待也使用自己的绝对Deadline。

带Deadline且尚未开始的Ready Task统一进入Runtime-wide、按Priority分区的indexed EDF heap，排序为`(Deadline, AdmissionSequence)`；它不进入Local Chase-Lev。成功首次start后永久退出EDF，后续Coroutine resume回到普通非EDF Priority band，具体Local/Global目的地由awaiter routing决定（yield/timer强制Global，普通owner-Worker completion可Local）。start前取消通过indexed erase清理，不保留tombstone。

Priority加权日历先选band，EDF只在同band内优先。一个band最多连续选择8个Deadline Task，随后若普通Global FIFO非空必须给它一次service opportunity。miss只进入Metrics/Trace，Task仍运行，不自动boost、cancel、throw、reject或改变Outcome。

注意：

> Deadline 不意味着 AstraScheduler 是硬实时系统。

只能定义为：

```text
best-effort deadline-aware scheduling
```

没有抢占、可调度性分析、最大lateness或完成期限保证，避免在README中声称“实时线程池”。

---

# 23. Scheduler 生命周期

生命周期不是一个 `bool stop_`，而是由公开生命周期状态和单调 Shutdown Mode 共同表达。以下是已确认的跨版本契约；实现可以改变内部原子变量和锁，但不能改变状态边界。

## 23.1 SchedulerState 与 Shutdown Mode

```cpp
enum class SchedulerState {
    Running,
    Stopping,
    Stopped
};

enum class ShutdownMode {
    None,
    Graceful,
    Immediate
};

struct SchedulerStatus {
    SchedulerState state;
    ShutdownMode shutdown_mode;
};
```

`Scheduler::status()`一次线性化、非阻塞且无副作用地返回成对快照，避免独立getter产生撕裂组合。合法pair仅为`Running+None`、`Stopping+Graceful/Immediate`和`Stopped+最后mode`；返回后可立即过时，不应用作submit前check-then-act。稳定core不增加`is_running()`等便利接口。

基本状态转换：

```text
Scheduler(options) startup transaction
   │ success publishes Running
   ▼
Running shared Handle
   │ shutdown() / shutdown_now() / Handle 析构 / Finalization
   ▼
Stopping + Graceful/Immediate
   │ work closure 终结或取消完成
   │ all Worker loops exit
   │ each Worker joined exactly once
   ▼
Stopped
```

关键不变量：

- `Stopping` 与 Shutdown Mode 是两个维度；不能只靠一个 stop flag 推导完整语义。
- Graceful 可以被 Immediate 单向升级，Immediate 永远不能降级。
- `Stopped` 是吸收状态。之后的 `shutdown()`/`shutdown_now()` 都是成功、无副作用的立即返回，不创建第二个关停世代。
- `Scheduler(SchedulerOptions)`同步完成配置、Reaper/handoff预留、Worker startup barrier与Running publication，失败完整rollback后抛出；不公开`Created/start/restart`。Running publication若先于Finalization close则构造成功但Handle可立即观察到Graceful Stopping，反之startup在开放用户工作前回滚并抛creation rejection。
- 同一个Scheduler Runtime不支持restart。需要新的执行域时构造新Scheduler；进程级Reaper Finalization开始后永久拒绝新构造。

## 23.2 Scheduler Handle 与 Runtime State

Scheduler 的前台对象使用 Handle 语义：

```text
Scheduler Handle
      │ shared capability
      ▼
Runtime State ◄──── Worker execution references
      ▲
      └──────────── Reaper handoff ownership
```

- Handle 析构不等于 Runtime State 立即销毁。
- Scheduler Handle可复制/移动；复制共享同一RuntimeId/Runtime State，move后source为空。只有最后一个关联Handle释放触发生命周期后备，任一副本shutdown影响全部副本。
- 非 Worker 上销毁最后一个活动 Handle：按 RAII 同步加入或发起关停，直到全部 Worker join。
- 同 Scheduler Worker 上销毁最后一个 Handle：不能 self-wait/self-join；必须以 `noexcept`、无分配、无线程创建的 handoff 把 Runtime State 移交给 Reaper，然后立即返回。
- Worker 上的孤儿 handoff 在 `Running` 时默认请求 Graceful；若已经 `Stopping`，保持现有模式。
- handoff 能力必须在任何 Worker 启动前建立；建立失败则启动事务失败且不得留下活动 Worker。

## 23.3 Reaper Service 生命周期

一个进程只使用一个逻辑 Reaper Service 和一条不属于任何 Scheduler 的 coordinator thread：

```text
Pending Runtime State
        │ Runtime 自行推进，Reaper 不阻塞等待
        ▼
    Join Ready
        │ Reaper 认领唯一 join ownership
        ▼
      Stopped
```

- Pending Runtime 可以永久不终结，但不得阻塞 Reaper 回收其他 Join Ready Runtime。
- Join Ready 表示 Worker 已不可逆离开用户执行路径；它仍不等于 `Stopped`。
- 只有实际完成全部 Worker join 后才能发布 Scheduler 的 Shutdown Completion。
- Reaper 在空闲时阻塞休眠，不因最后一个 Runtime 消失而自动退出或重启。
- Reaper 控制面不执行用户 Callable，不参与 work stealing，也不产生 Internal Submission。

---

# 24. shutdown 语义

## 24.1 Graceful Shutdown

```cpp
scheduler.shutdown();
```

语义：

```text
Running → Graceful Stopping 的线性化点
   ├── 永久关闭 External Submission
   └── 继续接受已纳入任务产生的 Internal Submission
                         │
                         ▼
                排空 Drain Work Closure
                         │
                         ▼
                Worker exit + unique join
                         │
                         ▼
                       Stopped
```

Drain Work Closure 是关停前已接受任务及其在 Graceful Stopping 期间获准的同 Scheduler Internal Submission 的传递闭包；不能用“关停瞬间队列快照”代替。非 Worker 调用的 `shutdown()` 只有在该闭包全部终结、Worker 全部 join 且 `Stopped` 发布后才返回，因此可以无限期阻塞。

---

## 24.2 Immediate Shutdown

```cpp
scheduler.shutdown_now();
```

语义：

```text
关闭 External/Internal Submission
├── 已接受且从未 first-start：发布 Cancelled 终态并唤醒等待者
├── 已 first-start 的 Suspended Coroutine：请求 stop，并允许 resume segment 到合作边界
   └── 已经 Running：只发布 cooperative stop request
                         │
                         ▼
                Worker exit + unique join
                         │
                         ▼
                       Stopped
```

`Immediate` 描述策略立即升级，不表示方法立即返回。非 Worker 的 `shutdown_now()` 仍同步等待全部 Worker join；正在执行且不检查 `stop_token` 的 Callable 无法被安全强杀，因此调用可以无限期阻塞。Runtime 禁止 detach Worker、伪造 `Stopped` 或改写 Running Task 的终态。

## 24.3 并发调用、模式升级与 Worker 调用限制

`shutdown()`与`shutdown_now()`都是返回真实Shutdown Completion的同步`void`方法。同Runtime Worker调用任一方法会形成self-wait/self-join，因此必须在任何state、admission、cancel、stop、join或wait副作用前抛`std::logic_error`；异常文本不稳定。其他Runtime的Worker仍按目标Runtime的普通非Worker同步路径处理，不静默降级为异步请求。

- 所有非 Worker `shutdown()`/`shutdown_now()` 调用共享同一个 Shutdown Completion；每个 Worker 只能被 join 一次。
- Graceful Stopping 期间的 `shutdown_now()` 原子升级为 Immediate；Immediate Stopping 期间的 `shutdown()` 只加入现有完成过程，不降级。
- 当前 Scheduler Worker 调用同步 `shutdown()` 或 `shutdown_now()` 会形成 self-wait/self-join，因此必须在关闭 admission、取消任务或发布 stop request 之前同步拒绝。
- 另一个 Scheduler 的 Worker 相对目标 Scheduler 是外部调用线程，仍可按非 Worker 契约同步关停目标 Scheduler；跨 Runtime 循环等待属于调用方需要避免的更高层协议风险。

## 24.4 析构语义

```text
最后一个 Handle 析构
   ├── 非 Worker：Running 默认 Graceful，Stopping 保留模式，同步等到 Stopped
   └── 本 Scheduler Worker：Running 默认 Graceful，Stopping 保留模式，handoff 后立即返回
```

析构是 `noexcept`，不会隐式选择 Immediate。Worker handoff 只改变等待与回收执行上下文，不改变任务取消策略；永久不终结任务可能使 Runtime State 被 Reaper 长期持有。

## 24.5 进程级 Reaper Finalization

Reaper Finalization 是进程级 one-shot 生命周期边界，与单个 Scheduler shutdown 不同：

```text
RegistrationOpen
      │ begin_finalization() 线性化
      ├── 永久关闭 Scheduler Runtime 注册
      ├── 对已核算 Runtime 请求 Graceful
      └── 立即返回 FinalizationControl
      ▼
Finalizing
      │ all Runtime Shutdown Completions + Reaper work empty
      ▼
CoordinatorExited
      │ one eligible waiter joins coordinator exactly once
      ▼
Finalized / Finalization Completion
```

与 begin 竞态的 Scheduler 启动必须有唯一顺序：先成功注册的 Runtime 被纳入终结，后发生的启动在创建任何 Worker 前失败。`Finalizing` 与 `Finalized` 都永久拒绝新注册；进程内不存在 Reaper restart。已注册但仍处于 Starting 的 Runtime 必须在开放任何用户任务前观察 sticky Finalization request，并回滚启动或直接进入关闭 admission 的 Stopping。

首次 begin 时若从未建立 Reaper 且核算集合为空，注册仍永久关闭，但直接发布完成，不为了终结空集合创建 coordinator thread。

## 24.6 Finalization 公共 Interface

```cpp
namespace astra {

enum class FinalizationWaitResult {
    Completed,
    TimedOut,
};

class FinalizationControl {
public:
    FinalizationControl(const FinalizationControl&) noexcept;
    FinalizationControl& operator=(const FinalizationControl&) noexcept;
    FinalizationControl(FinalizationControl&&) noexcept;
    FinalizationControl& operator=(FinalizationControl&&) noexcept;
    ~FinalizationControl() noexcept;

    void wait() const;

    template<class Rep, class Period>
    [[nodiscard]] FinalizationWaitResult
    wait_for(std::chrono::duration<Rep, Period> timeout) const;

    void request_immediate() const noexcept;

private:
    FinalizationControl(/* internal capability */) noexcept;
    friend FinalizationControl begin_finalization() noexcept;
};

[[nodiscard]] FinalizationControl begin_finalization() noexcept;

} // namespace astra
```

`FinalizationControl` 不可默认构造、可复制、可跨非 Worker 线程共享，所有副本观察同一 Completion。它不是 Reaper 的 RAII owner：销毁一个或全部控制对象都不会阻塞、取消、恢复注册或停止后台推进。

重复或并发 `begin_finalization()` 幂等返回同一终结世代；Finalized 后再次调用也只返回已完成控制对象。

## 24.7 begin、wait、wait_for 与显式升级

### `begin_finalization()`

- 永久关闭注册并可靠发布初始 Graceful 请求。
- 保持已经存在的 Immediate Mode，不降级。
- 只等待请求状态完成线性化与通知，不等待任何 Runtime drain 或 join。
- 可由普通线程或任意 Scheduler Worker 调用。

### `wait()`

- 只有所有已核算 Runtime 达到 Shutdown Completion、Reaper 工作清空、coordinator 退出并被 join、Finalized 发布后才返回。
- 可以无限期阻塞，不自动升级、detach 或伪造完成。
- 任意 Scheduler Worker 调用都会在等待和副作用前抛出 `std::logic_error`，因为该 Worker 本身属于全局完成集合。

### `wait_for(timeout)`

- 合法非 Worker 调用在真实完成时返回 `Completed`，期限先到则返回 `TimedOut`。
- `TimedOut` 只结束本次观察；注册保持关闭，Reaper/coordinator 与所有 Runtime 继续同一次 Finalization。
- 使用 `steady_clock`。timeout 小于或等于零时执行即时观察：已完成返回 `Completed`，否则返回 `TimedOut`。
- Completion 与 deadline 在统一同步域内线性化；若 TimedOut 已线性化，即使 Completion 在方法实际返回前发布，本次仍返回 TimedOut。
- duration 不是硬实时返回上限，OS 调度可能产生轻微超时回程延迟。
- 任意 Scheduler Worker 调用，包括 `wait_for(0)`，都抛出 `std::logic_error`；需要 Worker-safe 状态查询时应另设纯观察 Interface，而不是复用等待操作。

### `request_immediate()`

- 显式把核算集合内全部尚未完成 Runtime 单向升级为 Immediate，包括只由 Reaper 持有的 orphan Runtime State。
- 已 Immediate 保持不变，已完成 Runtime 不改写历史终态。
- 请求发布后立即返回，不等待 Finalization Completion；Running Task 仍只收到 cooperative stop request，因此升级不保证有界完成。
- 可由普通线程或任意 Scheduler Worker 调用；`wait_for()` 超时绝不自动触发它。

典型主控流程：

```cpp
using namespace std::chrono_literals;

auto finalization = astra::begin_finalization();

while (finalization.wait_for(5s) ==
       astra::FinalizationWaitResult::TimedOut) {
    report_finalization_progress();

    if (must_escalate()) {
        finalization.request_immediate();
    }

    if (must_terminate_process()) {
        terminate_process();
    }
}
```

## 24.8 coordinator join 与显式进程收尾

coordinator 不能 join 自己。它在工作清空后发布内部 `CoordinatorExited` 并退出；合法非 Worker 等待者中只有一个能认领 join ownership，完成唯一 join 后再发布 Finalization Completion，其他等待者观察同一完成事件。若调用方只 begin 而从不等待，coordinator 即使退出也会保持未 join，直到未来合法等待者完成收尾。

Finalization 不注册 `atexit`，不挂接静态析构、最后一个 Scheduler 析构或空闲超时。动态库卸载前必须观察 `Completed`，并确保不再有对象会调用 AstraScheduler 代码；`TimedOut` 不能作为卸载许可。不可逆全局测试使用独立子进程，不提供公共 reset/restart。

## 24.9 Reaper 不可恢复故障

用户 Callable 异常进入 Task result/exception propagation，不属于 Reaper 故障。若 coordinator 出现逃逸异常、join ownership 破坏、handoff 所有权断裂或其他无法证明可恢复的控制面错误，Runtime 必须执行 `noexcept` 的尽力诊断并 `std::terminate()`；不得把损坏状态伪装成 `TimedOut`、`Stopped` 或 `Finalized`，也不得 detach 或 restart。

生命周期决策的完整理由与拒绝方案见 [D-002 至 D-040](./.scratch/astra-scheduler-runtime/decision-log.md) 以及 [ADR-0001 至 ADR-0018](./docs/adr/)。

---

# 25. Backpressure

即使使用 Work Stealing，也不能忽略任务洪泛。

Backpressure 使用 External Pending Capacity，而不是把某个底层队列当作全部任务的物理硬上限。它限制已接受但尚未首次进入 Running 的 External Submission，覆盖 Waiting 与 Ready；首次 start 或 start 前取消时释放 slot。Internal Submission 为保持 Drain Work Closure 与 Worker liveness 不占该配额，因此该配置不是 Runtime 总内存硬上限。

```cpp
enum class ExternalBackpressure {
    Reject,
    Block
};

enum class MetricsLevel {
    Off,
    Basic,
    Detailed
};

class TraceCollector;

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

`recommended_worker_count()`使用`hardware_concurrency()`提示并在其返回0时fallback为1。所有size字段显式0、未知enum在Reaper registration和Worker创建前以`invalid_argument`拒绝；options在Scheduler构造时形成不可变snapshot。Priority weights、deadline burst、active spin、deque/timer容量等不扩张为公共knob。

默认 `Reject`。`Block` 只允许普通非 Worker 线程等待；任意 AstraScheduler Worker 向另一 Runtime 提交时若目标 slot 不可立即取得，必须立即拒绝，不能阻塞或跨 Runtime Helping。同 Runtime Internal Submission 继续豁免 external slot。稳定 Runtime 不提供 `CallerRuns`，所有 Task 都必须先成功 admission 再经正常 Scheduler 路径执行。

Block waiter 同时观察 slot 和 lifecycle gate：slot 释放或 gate 关闭都可靠唤醒，关闭后以 lifecycle rejection 退出；不保证 FIFO 或调度延迟。

---

# 26. Metrics 设计

Metrics通过`SchedulerOptions::metrics_level`选择`Off`、默认`Basic`或`Detailed`。Off只关闭观测，不改变TaskId、同步与调度；Basic使用固定低基数、分片饱和counter和独立gauges；Detailed再加入per-Priority/per-Worker分解与固定64 bucket的log2纳秒直方图。Runtime不保留原始样本或高基数Task label。

Basic至少统计：

```text
submission_attempts / accepted_task_identities
rejected_lifecycle / rejected_capacity
first_starts / resume_segments
succeeded / failed
cancelled_before_start / cancelled_cooperative
waiting_tasks / ready_tasks / running_tasks / suspended_tasks
external_pending_slots_used
```

Work Stealing：

```text
steal_attempts
steal_successes
steal_failures
```

Worker：

```text
global_claims / local_claims
worker_parks / worker_wakes
parked_workers
```

Latency：

```text
ready_queue_wait
execution_segment
task_wall_time
blocking_admission_wait
timer_wake_lateness
deadline_start_lateness
```

Reaper / Finalization：

```text
registered_runtimes
pending_runtimes
join_ready_runtimes
runtime_handoff_count
runtime_join_latency
finalization_state
finalization_elapsed_time
finalization_timeout_count
```

同时记录Coroutine、Timer、Graph与Deadline的固定事件counter。Graph Node按Task Identity核算，GraphRun另计run admission；Coroutine只计一个Task Identity，后续resume只增加segment。

暴露：

```cpp
RuntimeMetricsSnapshot stats = scheduler.metrics_snapshot();
```

Snapshot拥有自身存储且不reset counter。它是有界扰动的fuzzy snapshot：每字段安全有效，但并发时不承诺共同线性化点或瞬时守恒；只有Runtime quiescent后才验证`accepted/outcome/gauges`等关系。所有累计量、bucket、sum与max饱和而不wrap，并用`saturated`显式报告。

`astra::process_metrics_snapshot()`独立观察Reaper/Finalization：registration、handoff、pending/join-ready/joined、begin/timeout/escalation和完成时长。它始终低成本开启，调用前不会惰性初始化Reaper，Finalized后只返回保留的最终事实，也不聚合每Runtime task hot counters。

Runtime Basic/Detailed Metrics还区分external blocking wait、same/cross-Runtime Helping Wait与Coroutine await；Detailed分别记录thread/helping/async await duration，但不维护在线wait-for graph。

---

# 27. Trace 设计

Trace使用显式共享`TraceCollector`。用户把同一个Collector放入一个或多个`SchedulerOptions`，即可在同一steady-clock origin下采集多Runtime和相关Reaper事件；默认不附加。Collector可重复start/stop capture，但同一时刻只有一代Recording。`TraceOptions`默认每Worker 16,384 events、shared external/control 65,536、每Reaper producer 4,096；Task/Wait/Coroutine/Graph/Timer/Runtime和steal-success默认启用，逐次steal-attempt等Verbose category显式opt-in。Worker只向预分配buffer追加trivially-copyable event，不分配、不阻塞、不写文件；满时drop-newest并计数。

Event schema带版本、逻辑Runtime/Worker/Task/GraphRun/Node/Coroutine segment identity、Priority/source/reason，绝不记录raw pointer或默认用户payload。事件口径与TaskState、Metrics、admission/start/terminal线性化边界一致。

记录事件：

```text
TaskCreated
TaskAdmitted / TaskRejected
TaskReady / TaskClaimed
TaskFirstStarted / TaskSegmentEnded
TaskTerminal / TaskCancelRequested
TaskStolen
WorkerSleep
WorkerWake
CoroutineSuspend
CoroutineResume
WaitBegin / WaitEnd
AwaitArmed / AwaitTriggered / AwaitResumed
TimerRegistered / TimerFired / TimerCancelled
GraphAccepted / NodeDependencyReleased / GraphTerminal
DeadlineMet / DeadlineMissed
RuntimeHandoff
RuntimeJoinReady
RuntimeJoined
FinalizationBegin
FinalizationEscalated
CoordinatorExited
FinalizationCompleted
```

`TraceCapture::stop()`只等待已进入的bounded emit临界区，随后产生共享backing的可复制不可变`TraceSnapshot`；重复stop返回同一结果。活动Capture若未stop便析构，则noexcept地disable/quiesce并丢弃该generation，使Collector回到Stopped，不产生隐式Snapshot。Chrome Trace JSON在Runtime外通过ostream离线导出，并按`(timestamp, ProducerId, local_sequence)`确定性merge。Snapshot/JSON显式记录capacity、category和per-kind loss；只在drop为0且schema有效时标记`trace_complete=true`，绝不合成事件掩盖丢失。

最终可以在 Trace Viewer 中看到：

```text
Worker 0   ███ Task A █ Task D
Worker 1      █████ Task B
Worker 2   ██ Task C       ███ Task E
Worker 3       steal → █ Task F
```

这比简单打印日志更能体现项目工程价值。

---

# 28. 日志系统

日志和 Trace 应分开。

日志负责：

```text
ERROR
WARN
INFO
```

Trace 负责：

```text
高频运行时事件
```

不要对每一个task使用同步日志。Trace不调用logger，Benchmark除专门overhead实验外禁用Trace和高频日志。

---

# 29. 推荐目录结构

```text
AstraScheduler/
│
├── CMakeLists.txt
├── README.md
├── LICENSE
├── .clang-format
├── .clang-tidy
│
├── cmake/
│   └── compiler_options.cmake
│
├── include/
│   └── astra/
│       ├── scheduler.hpp
│       ├── scheduler_options.hpp
│       ├── finalization.hpp
│       │
│       ├── task/
│       │   ├── task.hpp
│       │   ├── task_handle.hpp
│       │   ├── task_state.hpp
│       │   └── cancellation.hpp
│       │
│       ├── graph/
│       │   ├── task_graph.hpp
│       │   ├── task_node.hpp
│       │   └── graph_executor.hpp
│       │
│       ├── coroutine/
│       │   ├── task.hpp
│       │   ├── promise.hpp
│       │   └── awaiter.hpp
│       │
│       ├── runtime/
│       │   ├── worker.hpp
│       │   ├── worker_context.hpp
│       │   ├── runtime_state.hpp
│       │   ├── reaper_service.hpp
│       │   └── shutdown_state.hpp
│       │
│       ├── queue/
│       │   ├── work_stealing_deque.hpp
│       │   ├── injection_queue.hpp
│       │   └── circular_buffer.hpp
│       │
│       ├── metrics/
│       │   ├── metrics.hpp
│       │   └── snapshot.hpp
│       │
│       └── trace/
│           ├── trace_event.hpp
│           └── trace_writer.hpp
│
├── src/
│   ├── scheduler.cpp
│   ├── runtime/
│   ├── queue/
│   ├── graph/
│   ├── metrics/
│   └── trace/
│
├── tests/
│   ├── unit/
│   │   ├── test_work_stealing_deque.cpp
│   │   ├── test_scheduler.cpp
│   │   ├── test_task_graph.cpp
│   │   ├── test_cancellation.cpp
│   │   ├── test_coroutine.cpp
│   │   ├── test_shutdown.cpp
│   │   └── test_finalization.cpp
│   │
│   └── stress/
│       ├── stress_submit.cpp
│       ├── stress_steal.cpp
│       ├── stress_shutdown.cpp
│       └── stress_reaper.cpp
│
├── benchmarks/
│   ├── micro_task.cpp
│   ├── cpu_bound.cpp
│   ├── imbalanced.cpp
│   ├── fork_join.cpp
│   ├── recursive_quicksort.cpp
│   └── benchmark_main.cpp
│
├── examples/
│   ├── basic_submit.cpp
│   ├── task_graph.cpp
│   ├── cancellation.cpp
│   ├── coroutine.cpp
│   └── tracing.cpp
│
├── tools/
│   └── trace_viewer/
│
└── docs/
    ├── architecture.md
    ├── work_stealing.md
    ├── memory_model.md
    ├── task_graph.md
    └── benchmark.md
```

---

# 30. 核心类关系

```text
   Scheduler Handle ───── shared capability ─────┐
                                                  ▼
                                       ┌───────────────────┐
   Reaper Service ─── registered ─────►│   Runtime State   │
                                       └─────────┬─────────┘
                                                 │ owns runtime resources
                              ┌──────────────────┼──────────────────┐
                              ▼                  ▼                  ▼
                         WorkerGroup       InjectionQueue      RuntimeMetrics
                              │
                              │ owns N
                              ▼
                           Worker
                              │
                              ├─────────── owns ──────────┐
                              ▼                           ▼
                     WorkStealingDeque              WorkerContext
                              │
                              ▼
                             Task
                              │
                       ┌──────┼─────────┐
                       ▼      ▼         ▼
                   TaskState Result  Cancellation
```

所有权要点：

- Scheduler Handle 可以先于 Runtime State 消失。
- Worker 执行路径必须保证 Runtime State 在当前访问期间存活。
- Reaper Service 预注册 handoff 能力，并只在 Runtime 达到 Join Ready 后认领 join。
- `Stopped` 只能在全部 Worker 实际 join 后发布；队列为空或 Join Ready 都不能替代 Shutdown Completion。

TaskGraph：

```text
TaskGraph
   │ owns
   ▼
TaskNode
   │
   ├── Task
   ├── dependency_counter
   └── successors[]
```

---

# 31. SchedulerOptions

推荐集中配置：

```cpp
struct SchedulerOptions {
    std::size_t worker_count =
        std::thread::hardware_concurrency();

    std::size_t external_pending_capacity = 65536;

    std::size_t max_helping_depth = 64;

    std::size_t local_burst_limit = 64;

    std::size_t steal_probe_limit = 8;

    ExternalBackpressure external_backpressure =
        ExternalBackpressure::Reject;

    bool enable_metrics = true;
    bool enable_trace = false;
};
```

不要让构造函数出现十几个位置参数。

---

# 32. 内存管理策略

这是并发项目非常重要的一部分。

第一阶段：

```text
std::unique_ptr<Task>
std::shared_ptr<TaskState>
```

优先保证正确性。

第二阶段分析：

```text
Task allocation 是否成为瓶颈？
```

如果成为瓶颈，再考虑：

- object pool
- slab allocator
- thread-local allocator
- `std::pmr`

不要一开始就自己写复杂内存池。

---

# 33. False Sharing

Worker 高频状态变量可能发生 False Sharing。

例如：

```cpp
struct WorkerStats {
    std::atomic<uint64_t> completed;
};
```

多个 Worker 的 `completed` 如果位于同一个 cache line，就可能产生 cache line ping-pong。

可以考虑：

```cpp
struct alignas(64) WorkerStats {
    std::atomic<uint64_t> completed;
};
```

但必须使用 Benchmark 验证，不要纯理论优化。

---

# 34. Memory Ordering 学习重点

Chase-Lev Deque 是项目中最适合展示 C++ Memory Model 深度的部分。

需要重点理解：

```cpp
std::memory_order_relaxed
std::memory_order_acquire
std::memory_order_release
std::memory_order_acq_rel
std::memory_order_seq_cst
```

尤其要能够解释：

1. 为什么普通变量存在 data race。
2. 为什么 atomic 不等于“所有操作天然顺序正确”。
3. acquire/release 建立的 happens-before 关系是什么。
4. 为什么最后一个元素时 Owner 和 Thief 需要原子竞争。
5. 为什么错误 memory order 可能在 x86 上“看起来没问题”，但在 ARM 上出错。

这部分建议单独写：

```text
docs/memory_model.md
```

---

# 35. Benchmark 体系

性能必须通过数据说明。

比较对象：

```text
AstraScheduler
SimpleGlobalQueueThreadPool
std::async
oneTBB（可选）
```

---

## 35.1 Micro Task

测试：

```text
100 ns
1 μs
10 μs
100 μs
1 ms
```

关注：

- scheduling overhead
- tasks/s
- queue contention

---

## 35.2 CPU-bound

任务例子：

- prime calculation
- hashing
- matrix block computation
- image transform

关注：

- throughput
- CPU utilization
- scalability

---

## 35.3 Imbalanced Workload

例如：

```text
1 ms
1 ms
2 ms
1 ms
100 ms
1 ms
2 ms
```

用于体现 Work Stealing 对负载不均衡的改善。

---

## 35.4 Fork-Join

例如：

```text
parallel quicksort
parallel merge sort
recursive tree traversal
```

这是 Work Stealing 最典型的优势场景。

---

## 35.5 DAG Benchmark

构造：

```text
wide graph
deep graph
diamond graph
random DAG
```

评估：

- dependency release overhead
- parallelism utilization
- critical path latency

---

# 36. Benchmark 指标

必须至少输出：

```text
Throughput      tasks/s
Latency         P50/P95/P99
CPU Utilization
Context Switches
Steal Attempts
Steal Success Rate
Queue Wait Time
Task Runtime
```

结果最好生成：

```text
CSV
+
Python plot script
+
README graph
```

---

# 37. 正确性测试

线程池项目不能只测“能不能跑”。

## 37.1 基础测试

- 0 个任务
- 1 个任务
- 大量任务
- void 返回值
- 普通返回值
- move-only 参数
- task exception

## 37.2 Work Stealing

- LocalQueue 单 Owner push/pop
- 多 Thief 同时 steal
- 最后一个元素竞争
- 大量 push/pop/steal
- resize

## 37.3 生命周期

- Graceful 排空传递性的 Drain Work Closure
- Immediate 取消未运行任务并唤醒 Future 等待者
- Running Task 忽略 stop request 时关停保持未完成
- submit 与 Running → Stopping admission 线性化竞态
- Graceful → Immediate 单向升级竞态
- 多个 shutdown 调用共享 Completion 且 Worker 只 join 一次
- `Stopped` 后 shutdown/shutdown_now 幂等 no-op
- 同 Scheduler Worker 调用同步 shutdown 在副作用前拒绝
- 非 Worker 最后 Handle 析构同步 Graceful
- Worker 最后 Handle 析构无分配 handoff，Runtime State 无 UAF

## 37.4 Reaper 与 Finalization

- handoff 注册失败时不启动任何 Worker
- 永久 Pending Runtime 不阻塞其他 Join Ready Runtime 回收
- Reaper 空闲不 busy-spin、不重建 coordinator
- Scheduler start 与 begin_finalization 注册关闭竞态
- 并发/重复 begin 返回同一 Finalization Completion
- 空核算集合不创建 coordinator 且直接 Completed
- 多个控制对象并发 wait/wait_for/request_immediate
- 任意 Scheduler Worker 调用 wait/wait_for 抛出 logic_error
- `wait_for(0)`、负 duration 与 steady-clock deadline 竞态
- TimedOut 后 Runtime/Reaper 继续，后续等待可 Completed
- 一个等待者唯一 join coordinator，其他等待者观察同一完成
- 全局 Immediate escalation 覆盖 orphan Runtime State
- Finalized 后不重启、不重新开放注册
- 不可恢复 Reaper 故障在独立子进程确定性 fail-fast
- 动态库卸载 gate 只接受真实 Completed

## 37.5 DAG

- 单节点
- 链式依赖
- Diamond DAG
- 大型随机 DAG
- 环检测
- 异常节点
- 取消节点

---

# 38. Sanitizer

建议 CI 中运行：

```text
AddressSanitizer
UndefinedBehaviorSanitizer
ThreadSanitizer
```

尤其：

```text
ThreadSanitizer
```

对于这个项目非常重要。

但要注意无锁算法可能需要针对工具行为进行分析，不能简单把所有报告都视为误报，也不能简单忽略。

---

# 39. CI 建议

GitHub Actions：

```text
Linux x86_64 GCC 13+
Linux x86_64 Clang 17+
Native Linux AArch64 GCC/Clang（定期 weak-memory 验证）
```

最终 Supported Configuration 仅包含 64-bit Linux：Tier-1 为 Linux x86_64 GCC 13+/Clang 17+；Tier-2 以 native Linux AArch64 GCC/Clang 定期验证 weak-memory 路径。Windows/MSVC、macOS、其他非 Linux OS 与 32-bit 目标均不受支持，不进入 release gate、package 支持声明或正确性/性能承诺；偶然编译成功也不提升支持状态。`Scheduler::capabilities()`返回startup时冻结的实际Local Deque backend：v0.1 Global-only=`None`、locked/fallback=`Locked`、真正启用的Chase-Lev=`ChaseLevLockFree`；`lock_free_local_deque()`只在最后一种情况下为true，不能按版本或算法名称虚假宣传lock-free。Core是C++20 compiled library，安装导出`AstraScheduler::AstraScheduler` CMake target；static默认、shared可选但没有跨版本/工具链ABI承诺。public header通过版本宏与constexpr `header_version()`报告编译期版本，compiled binary通过无副作用`library_version()`/`library_version_string()`报告已链接版本；这些查询不初始化Runtime或Reaper。受支持部署每进程只有一个Astra implementation instance：单可执行程序可静态链接，多个DSO/plugin必须共同链接同一个exact-version shared library，不能各自嵌入static copy后仍声称process-wide Reaper/ID/metrics。

本机开发、configure、build、test、format、lint、package consumer、sanitizer、stress、benchmark 与 release verification 必须在 WSL Linux 用户空间执行，规范工作目录为`/mnt/d/code/cppStudy/AstraScheduler`。Windows PowerShell/cmd 只允许用于启动 WSL 或进行非开发性的宿主编排；WSL/Linux build 目录与任何 Windows-native cache/产物必须隔离。CI 与最终发布证据可直接来自 native Linux runner，WSL 不是最终运行时依赖。

至少执行：

```text
build
unit test
clang-format check
clang-tidy
asan/ubsan
tsan
```

Benchmark 不建议每次 PR 全量执行，可以：

```text
manual workflow
或
nightly benchmark
```

---

# 40. 开发阶段规划

## Phase 0：工程骨架

目标：把项目基础工程建立起来。

完成：

- [ ] CMake
- [ ] include/src/tests/benchmarks
- [ ] clang-format
- [ ] clang-tidy
- [ ] GoogleTest
- [ ] GitHub Actions

---

## Phase 1：Basic Scheduler

完成：

- [ ] Worker
- [ ] Scheduler
- [ ] Global Injection Queue
- [ ] `submit/try_submit` admission 与 External capacity/backpressure
- [ ] `TaskHandle<T>` / Terminal Outcome / wait/get/exception
- [ ] TaskState / cooperative cancellation / stop_token
- [ ] Scheduler Handle / shared Runtime State
- [ ] Graceful/Immediate Shutdown / Shutdown Completion
- [ ] 进程级 Reaper 注册、handoff 与唯一 join
- [ ] `FinalizationControl` 完整 begin/wait/wait_for/escalation 生命周期

这一阶段仅作为正确性基线。

同时保留一个：

```text
SimpleGlobalQueueThreadPool
```

以后作为 Benchmark 对照组。

---

## Phase 2：Work Stealing

这是第一个真正核心版本。

完成：

- [ ] Worker LocalDeque
- [ ] thread_local WorkerContext
- [ ] external submit → global queue
- [ ] internal submit → local queue
- [ ] stealing
- [ ] steal metrics
- [ ] stress tests

先实现带锁 LocalDeque 验证调度逻辑，再替换成 Chase-Lev，可以降低调试难度。

---

## Phase 3：Chase-Lev Deque

完成：

- [ ] atomic top/bottom
- [ ] owner pop
- [ ] thief steal
- [ ] last-item race
- [ ] buffer resize
- [ ] memory reclamation strategy
- [ ] TSan / stress test
- [ ] memory model 文档

这个版本是项目的技术核心之一。

---

## Phase 4：Task Graph

完成：

- [ ] TaskGraph
- [ ] TaskNode
- [ ] dependency counter
- [ ] successor release
- [ ] cycle detection
- [ ] graph execution handle

这是项目从线程池向 Runtime 转变的关键版本。

---

## Phase 5：Coroutine + Timer

完成：

- [ ] Coroutine Task
- [ ] promise_type
- [ ] awaiter
- [ ] Scheduler resume
- [ ] TaskHandle / GraphRun async await
- [ ] yield / cancellation point
- [ ] Worker-driven sleep timer heap
- [ ] Graph coroutine node
- [ ] Suspend / Resume Trace

不要在这一阶段直接加入真实网络 I/O。

先实现：

```text
scheduler yield
scheduler switch
sleep awaiter
```

证明 Coroutine Runtime 模型正确。

---

## Phase 6：Priority + Deadline

完成：

- [ ] Four Priority bands for Global/Local
- [ ] 8:4:2:1 weighted service
- [ ] Priority 的 Internal 默认继承与显式 `TaskOptions` override；Deadline 仅显式携带
- [ ] first-start Task Deadline
- [ ] per-Priority indexed EDF heap
- [ ] miss Metrics/Trace

---

## Phase 7：Observability

完成：

- [ ] Metrics Snapshot
- [ ] Trace Event
- [ ] Chrome Trace JSON
- [ ] worker timeline
- [ ] steal visualization

---

## Phase 8：Benchmark & Optimization

完成：

- [ ] Global Queue vs Work Stealing
- [ ] micro task
- [ ] imbalanced task
- [ ] fork-join
- [ ] DAG
- [ ] oneTBB comparison
- [ ] flame graph（可选）
- [ ] perf analysis

所有性能优化都应该从这个阶段的数据出发。

Benchmark由两层组成：默认不参与consumer build的pinned Google Benchmark micro targets，以及仓库内`astra_bench_scenarios`多阶段/子进程runner。setup、warmup、timed region、drain verification和teardown严格分开；漏任务、checksum不符、意外rejection/drop或子进程异常会使样本invalid，而不是产生“更快”数字。

固定corpus覆盖Global FIFO、locked Work-Stealing、Chase-Lev三种in-tree语义基线，以及micro/CPU/imbalanced/fork-join/DAG/Coroutine/timer/Priority/Deadline/shutdown/reaper和组合负载。oneTBB只在可比较子集作为可选背景adapter；显式`std::async(std::launch::async)`只用于受限粗粒度独立任务context，二者都不承担correctness oracle、primary regression gate或发布依赖。

Standard profile默认2秒warmup、10个至少1秒的独立repetition，不删除outlier；versioned JSON保存所有raw values、median/MAD/p10/p90/bootstrap 95% CI、Metrics buckets、commit/build/compiler/CPU/OS/options/seed/checksum等。共享PR CI只做build/smoke，正式regression gate只在专用稳定runner按versioned per-case policy同时满足实践阈值和置信区间条件时触发，baseline更新必须review。

---

# 41. 推荐开发顺序

```text
Global Runtime + TaskHandle/Lifecycle
       │
       ▼
Locked per-worker Work Stealing
       │
       ▼
Chase-Lev portable deque
       │
       ▼
Task Graph / DAG
       │
       ▼
Coroutine + Timer
       │
       ▼
Priority + Deadline
       │
       ▼
Metrics + Bounded Trace
       │
       ▼
Benchmark Framework
       │
       ▼
Cross-platform/API Hardening
       │
       ▼
v1 Stable Source API
```

不要反过来：

```text
先写 Coroutine + 无锁 + DAG + Priority + NUMA
```

这种路线非常容易陷入无法调试的并发状态组合。

---

# 42. 不建议第一版实现的东西

以下功能听起来高级，但第一版不建议：

```text
NUMA-aware scheduler
动态线程池
无锁全局队列
I/O runtime
io_uring
Fiber
Timer Wheel
GPU scheduler
Distributed task runtime
```

原因不是它们不好，而是会模糊项目真正核心：

> **高质量 Work-Stealing Task Runtime**

---

# 43. 项目真正的四个技术亮点

如果最终只能把四点写在简历上，我建议是：

## 43.1 Chase-Lev Work-Stealing Deque

体现：

```text
C++ atomic
CAS
memory ordering
lock-free algorithm
cache locality
```

## 43.2 DAG Task Scheduler

体现：

```text
task abstraction
dependency management
parallel execution
scheduler architecture
```

## 43.3 C++20 Coroutine Integration

体现：

```text
coroutine frame
promise_type
awaiter
suspend/resume
runtime scheduling
```

## 43.4 Trace + Benchmark

体现：

```text
工程验证能力
performance profiling
observability
system optimization
```

---

# 44. README 首页建议展示内容

README 第一屏不要先放大量代码。

建议：

```text
AstraScheduler

Modern C++20 Work-Stealing Task Runtime

Features:
✓ Chase-Lev Work-Stealing Deque
✓ DAG Task Graph
✓ Future & Exception Propagation
✓ Cooperative Cancellation
✓ C++20 Coroutine Scheduling
✓ Runtime Metrics
✓ Chrome Trace Export
```

然后放一张架构图：

```text
Global Queue
     │
     ▼
Worker 0 ←steal→ Worker 1 ←steal→ Worker 2
   │                 │                 │
LocalDeque         LocalDeque        LocalDeque
```

再放 Benchmark 图。

这会比 README 开头直接贴几百行源码更加专业。

---

# 45. 面试时必须能回答的问题

项目做完后，至少应能独立回答：

### Work Stealing

1. 为什么需要 Local Queue？
2. 为什么 Owner LIFO，Thief FIFO？
3. 为什么不一直使用 Global Queue？
4. stealing 失败后怎么处理？
5. Victim 怎么选？

### Chase-Lev

6. `top` 和 `bottom` 分别由谁修改？
7. 为什么最后一个元素需要 CAS？
8. 为什么 resize 很麻烦？
9. 为什么旧 Buffer 不能马上 free？
10. memory ordering 是怎么保证正确性的？

### Scheduler

11. 外部提交和内部提交有什么区别？
12. 为什么需要 thread-local WorkerContext？
13. Worker 没任务时为什么不能一直 spin？
14. 如何避免 lost wakeup？
15. shutdown 时还有任务怎么办？

### DAG

16. dependency counter 为什么适合用 atomic？
17. 一个节点什么时候进入 Ready？
18. 怎么做 cycle detection？
19. DAG 中一个任务失败怎么办？
20. DAG 取消策略是什么？

### Coroutine

21. Coroutine 和线程有什么区别？
22. `co_await` 后 Worker 在干什么？
23. Coroutine Frame 存在哪里？
24. 谁负责 resume？
25. Coroutine 怎么重新进入 Scheduler？

### Performance

26. Work Stealing 一定比全局队列快吗？
27. 哪些工作负载更适合 Work Stealing？
28. 微任务为什么容易被调度开销吞噬？
29. False Sharing 是什么？
30. 你的 Benchmark 如何保证公平？

如果这些问题能结合自己的代码和 Benchmark 数据回答，这个项目的价值就远高于单纯“功能实现”。

---

# 46. 项目完成标准

不要用“代码写完”判断完成。

建议最终 Definition of Done：

- [ ] API 能稳定使用。
- [ ] Scheduler 生命周期明确。
- [ ] Scheduler Handle 与 Runtime State 生命周期解耦经过压力测试。
- [ ] Graceful/Immediate、模式升级与重复关停语义有可追踪测试。
- [ ] Reaper 的 Pending/Join Ready、唯一 join 与空闲生命周期已验证。
- [ ] Finalization begin/wait/wait_for/escalation 与进程卸载边界已验证。
- [ ] Work Stealing 有压力测试。
- [ ] Chase-Lev 有独立测试。
- [ ] TSan 无未解释的数据竞争。
- [ ] DAG 能运行复杂依赖图。
- [ ] Cancellation 行为定义明确。
- [ ] Coroutine 可 Suspend / Resume。
- [ ] Metrics 可实时读取。
- [ ] Chrome Trace 可视化。
- [ ] Benchmark 有基线对照。
- [ ] README 有架构图。
- [ ] README 有性能图。
- [ ] `docs/memory_model.md` 能解释内存序。
- [ ] 所有性能结论有数据支撑。

---

# 47. 最终项目范围建议

如果以“亮眼 + 能完成 + 能讲深”为目标，最终版本推荐控制在：

```text
AstraScheduler
│
├── Work-Stealing Scheduler       ★★★★★
├── Chase-Lev Deque               ★★★★★
├── TaskHandle / Result / Exception ★★★☆☆
├── Cancellation                  ★★★★☆
├── DAG Task Graph                ★★★★★
├── C++20 Coroutine               ★★★★★
├── Worker-driven Timer           ★★★★☆
├── Priority                      ★★★☆☆
├── Deadline                      ★★★★☆
├── Metrics                       ★★★★☆
├── Chrome Trace                  ★★★★★
└── Benchmark                     ★★★★★
```

暂时不做：

```text
Dynamic Worker Scaling
NUMA
io_uring
Distributed Scheduler
GPU Scheduler
```

这样项目边界比较合理，同时技术深度足够。

---

# 48. 一句话架构总结

AstraScheduler 的最终核心可以概括为：

```text
                 Task API
                    │
          ┌─────────┼──────────┐
          ▼         ▼          ▼
       Callable    DAG      Coroutine
          │         │          │
          └─────────┼──────────┘
                    ▼
                 Scheduler
                    │
         ┌──────────┴──────────┐
         ▼                     ▼
 Global Injection Queue    Worker Group
                               │
                  ┌────────────┼────────────┐
                  ▼            ▼            ▼
               Worker 0     Worker 1     Worker N
                  │            │            │
               Deque 0      Deque 1      Deque N
                  │            │            │
                  └────── Work Stealing ────┘
                               │
                               ▼
                        Runtime Execution
                               │
                  ┌────────────┼────────────┐
                  ▼            ▼            ▼
               Result       Metrics       Trace
```

其中真正决定项目深度的不是“线程数量”，而是：

> **任务如何表示、任务如何进入系统、Worker 如何寻找工作、多个 Worker 如何竞争任务、依赖如何释放、Coroutine 如何恢复、取消如何传播，以及如何证明整个调度系统真的有效。**

---

# 49. 推荐的第一个正式开发目标

第一阶段先建立能承载全部后续能力的 Runtime substrate，但不提前引入 Local Queue 或 Work Stealing。

建议先完成：

```text
Phase 0（untagged）

CMake package / tests / CI / consumer smoke

AstraScheduler v0.1.0

Scheduler
+
Fixed Worker Group
+
Global Injection Queue
+
TaskHandle / Outcome / Wait / Exception
+
Cooperative Cancellation / Backpressure
+
Shutdown / Runtime State / Reaper / Finalization
```

然后在不改 Task、Admission 和生命周期语义的前提下加入：

```text
AstraScheduler v0.2.0

Per-worker locked Local Queue
+
Work Stealing / fairness / park-wakeup
```

确认整个调度逻辑正确后，再进入无锁数据结构版本：

```text
AstraScheduler v0.3.0

Locked LocalDeque
        ↓
Chase-Lev Work-Stealing Deque
```

这样能够非常清楚地区分：

```text
调度算法错误
```

和：

```text
无锁数据结构错误
```

这是整个项目开发过程中非常重要的工程决策。

---

# 50. 建议的 Git 里程碑

```text
Phase 0 untagged  CMake / tests / CI / package scaffold
v0.1.0  Global Runtime + TaskHandle/Outcome/Cancel + lifecycle/Reaper
v0.2.0  Locked per-worker Work-Stealing + wake/fairness
v0.3.0  Chase-Lev portable deque + reclamation/rebase
v0.4.0  DAG Task Graph + GraphRun/report
v0.5.0  C++20 Coroutine + Astra awaiters + Timer
v0.6.0  Priority + first-start Deadline EDF
v0.7.0  Metrics + bounded Trace + Chrome export
v0.8.0  Benchmark framework + baselines/artifacts
v0.9.0  Cross-platform/package/API hardening release candidate
v1.0.0  Stable source/semantic API
```

每个版本都尽量做到：

```text
feature
+
test
+
document
+
benchmark（需要时）
```

而不是一次提交一个巨大的最终版本。

项目采用SemVer：0.x minor允许经决策与migration note批准的source breaking change，patch不做计划性breaking；v1起source/observable semantics按SemVer稳定。C++ binary ABI不跨Astra版本、compiler、stdlib、CRT或build配置保证，shared consumer必须exact-version/toolchain配套并rebuild/relink。Metrics、Trace和Benchmark artifact使用独立schema version。

---

## 结语

这个项目最重要的目标不是写出“功能最多”的线程池，而是设计出一个**能够解释、能够测试、能够测量、能够演进**的并发任务运行时。

开发过程中始终保持以下顺序：

```text
Correctness
    ↓
Architecture
    ↓
Observability
    ↓
Benchmark
    ↓
Optimization
```

不要反过来从复杂无锁优化开始。

对于最终作品，最值得投入精力的仍然是四个部分：

```text
Chase-Lev Work-Stealing Deque
Task Graph Scheduler
C++20 Coroutine Integration
Trace + Benchmark
```

只要这四部分完成度足够高，AstraScheduler 就已经可以从“线程池练习项目”提升为一个真正具有系统设计深度的现代 C++ 并发项目。
