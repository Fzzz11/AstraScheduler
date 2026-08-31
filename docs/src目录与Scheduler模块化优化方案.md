# AstraScheduler `src` 目录与 Scheduler 模块化优化方案

状态：已按 D-177/D-178、R-125～R-130、AST-067～073 完成核心重构与 `RuntimeState/ReadyQueues/diagnostics` 深层拆分
日期：2026-08-31
适用范围：AstraScheduler 1.2.x 之后的纯内部重构

## 1. 背景

当前项目已经明确区分两类头文件：

- `include/astra/*.hpp`：安装给 consumer 的 public headers；
- `src/*.hpp`：不安装的实现头和白盒测试 seam。

因此，`src` 中同时存在 `.hpp` 和 `.cpp` 本身不是问题。当前真正需要优化的是：

1. `src/scheduler.cpp` 仍有约 2500 行，承载 Runtime 状态、Worker loop、诊断路由、公共门面和 Graph 执行等多类知识；
2. `GraphExecution` 虽然已有独立内部类型，但 `GraphExecution::run()` 仍定义在 `scheduler.cpp`，并通过 `scheduler.impl_` 直接访问 Scheduler 内部字段；
3. `src` 已形成 Runtime、Task、Graph、Lifecycle、Observability 等稳定子系统，但文件仍全部平铺，目录结构没有反映代码所有权。

这三个问题存在因果关系：Graph 执行没有窄的 Runtime seam，因此无法形成独立编译单元；内部职责没有真正分离，因此简单移动目录只能改善视觉整洁，不能改善封装性。

## 2. 目标

- `scheduler.cpp` 只保留 Scheduler public facade 和 Runtime 总装配。
- Graph 执行只依赖一个窄的内部 Runtime port，不直接读取 `Scheduler::Impl` 字段。
- Graph admission、构造和发布前失败使用统一 RAII transaction 回滚。
- Worker claim/steal/park/execute/helping 协议形成独立内部模块。
- Runtime lookup 和 wait/await diagnostics 不再与 Scheduler public facade 混放。
- `src` 按子系统组织，相关 `.hpp/.cpp` 就近放置。
- 不改变 documented public API、异常类型、取消语义、调度语义、Metrics/Trace 口径或 package surface。

## 3. 非目标

- 不按扩展名拆成 `src/headers` 和 `src/sources`。
- 不为每个目录创建只转发调用的浅层 library target。
- 不在本轮修改 Worker scheduling、Graph edge policy、Coroutine handoff 或 shutdown 行为。
- 不把 `src` 下的内部头加入安装集合。
- 不以固定行数作为成功标准；行数下降只是职责收敛的结果。
- 不把 `Scheduler::Impl` 整体移入一个被所有模块直接访问的大型头文件。

## 4. 设计原则

### 4.1 按不变量拆分，不按代码长度拆分

模块应拥有完整规则和状态，而不是仅包装字段。应提供：

```cpp
runtime.admit_graph(node_count, caller);
runtime.publish_ready(std::move(invoker), origin);
runtime.register_timer(...);
```

不应跨模块直接执行：

```cpp
runtime.metrics.active_graph_runs++;
runtime.external_pending_count -= count;
runtime.work_epoch.fetch_add(1);
```

### 4.2 内部头可以存在，但必须保持非安装

模块间需要共享的声明放在 `src/<module>/*.hpp`。这些头通过 AstraScheduler target 的 `PRIVATE` include path 使用，package/install gate 必须继续证明它们不会进入 consumer 安装目录。

### 4.3 先建立 seam，再移动实现和目录

直接把 `GraphExecution::run()` 剪切到新 `.cpp` 会遇到 `Scheduler::Impl` 只在 `scheduler.cpp` 中定义的问题。正确顺序是先建立窄端口，消除对 `scheduler.impl_` 的直接依赖，再迁移实现。

## 5. GraphExecution 深化方案

### 5.1 当前耦合

当前 `GraphExecution::run()` 直接使用以下 Scheduler 内部知识：

- RuntimeId、TaskId、GraphRunId 分配；
- admission slots 的申请与释放；
- Task invoker 的 ready publication；
- timer 注册和取消；
- Graph admission、accepted/rejected、active run Metrics；
- Worker caller 类型和继承优先级；
- Coroutine node 的 rescheduler。

这些依赖使 `GraphExecution` 只是名称上的独立类型，不是独立模块。

### 5.2 引入 `GraphRuntimePort`

在非安装头 `src/graph/graph_runtime_port.hpp` 中定义 Graph 所需的最小能力：

```cpp
namespace astra::detail {

class GraphRuntimePort {
public:
    virtual RuntimeId runtime_id() const noexcept = 0;
    virtual GraphRunId allocate_graph_run_id() = 0;
    virtual TaskId allocate_task_id() = 0;

    virtual AdmissionDecision reserve_graph(
        std::size_t node_count,
        bool may_block,
        bool internal_caller) = 0;

    virtual void release_external_slots(std::size_t count) noexcept = 0;
    virtual void post(
        std::unique_ptr<TaskInvokerBase> invoker,
        bool external_origin) = 0;

    virtual std::uint64_t register_timer(
        std::chrono::steady_clock::time_point,
        std::shared_ptr<AwaitHandshake>,
        std::function<void()>) = 0;
    virtual void cancel_timer(std::uint64_t timer_id) noexcept = 0;

    virtual void record_graph_admission_attempt() noexcept = 0;
    virtual void record_graph_rejected() noexcept = 0;
    virtual void record_graph_accepted() noexcept = 0;
    virtual void record_graph_finished() noexcept = 0;

protected:
    ~GraphRuntimePort() = default;
};

}  // namespace astra::detail
```

这是 internal seam，不是 public API。是否使用虚函数可在实现 Ticket 中结合 benchmark 决定；首要要求是 `GraphExecution` 不再知道 `Scheduler::Impl` 的字段布局。

### 5.3 Scheduler 侧适配

`Scheduler::Impl` 实现 `GraphRuntimePort`，方法内部委托现有深模块：

```cpp
AdmissionDecision Scheduler::Impl::reserve_graph(...) {
    return admission.acquire_slots(...);
}

TaskId Scheduler::Impl::allocate_task_id() {
    return identities.allocate_task(runtime_id);
}

void Scheduler::Impl::post(...) {
    post_task_internal(...);
}
```

Graph 模块由此依赖“Runtime 能做什么”，而不是“Runtime 字段如何组织”。

### 5.4 使用 RAII admission lease

当前 Graph 构造失败路径需要分别恢复 external slots 和 active graph metrics。建议引入：

```cpp
class GraphAdmissionLease {
public:
    GraphAdmissionLease(GraphRuntimePort&, std::size_t slots, bool external);
    ~GraphAdmissionLease();

    GraphAdmissionLease(const GraphAdmissionLease&) = delete;
    GraphAdmissionLease& operator=(const GraphAdmissionLease&) = delete;

    void commit_to_execution() noexcept;
};
```

语义：

- reservation 成功后立即创建 lease；
- Graph state、node state、TaskId、edge storage 构造失败时，析构统一回滚；
- 所有 root 成功 publication 后调用 `commit_to_execution()`；
- commit 后 slots 由 node terminal 路径释放，active graph metric 由 Graph completion 释放。

### 5.5 将递归 lambda 变为实例方法

`GraphExecution` 应成为拥有运行协议的实例，而不是只有静态 `run()` 的命名空间替代品：

```cpp
class GraphExecution final
    : public std::enable_shared_from_this<GraphExecution> {
public:
    static GraphRun start(
        GraphRuntimePort&,
        std::optional<TaskOptions>,
        FrozenTaskGraph&&);

private:
    void publish_node(NodeId);
    void complete_node(NodeId, TaskState, std::exception_ptr = nullptr);
    void release_successors(NodeId);
    void cancel_unstarted(NodeId);

    GraphRuntimePort& runtime_;
    std::shared_ptr<GraphRunSharedState> state_;
    bool internal_caller_{false};
};
```

这样依赖传播、取消、Coroutine resume 和 terminal publication 都由同一对象维护。异步 callback 捕获 `shared_ptr<GraphExecution>`，避免捕获多组裸 `Impl*`、state 和递归 lambda。

### 5.6 迁移到独立编译单元

完成 seam 后新增 `src/graph/graph_execution.cpp`。`Scheduler::run_impl()` 最终只保留：

```cpp
return detail::GraphExecution::start(
    *impl_, options, std::move(graph));
```

同时移除 `GraphExecution` 对 `Scheduler` 的 friend 权限。

## 6. Scheduler 拆分方案

### 6.1 `runtime_state`

已提取到 `src/runtime/runtime_state.{hpp,cpp}`，作为单个 Runtime 的唯一组合状态所有者；`Scheduler::Impl` 通过内部继承复用该状态，只保留 shared ownership、Reaper handoff 与 Graph port 适配，不再复制字段。

拥有 Runtime 组合状态和生命周期转换：

- frozen SchedulerOptions 和 capabilities；
- packed SchedulerStatus；
- AdmissionController、TimerQueue、RuntimeMetrics、RuntimeIdentityAllocator；
- Worker thread 集合与 startup/shutdown/join 协议；
- ReadyQueues、RuntimeDiagnostics 和 work epoch。

该模块提供完整操作，不为其他模块暴露可写字段。

### 6.2 `worker_loop`

拥有 Worker 运行协议：

- Global/Local/steal claim 顺序；
- priority band service；
- park handshake 和 wake epoch；
- invoker execute/cancel-before-start；
- helping wait 的 Worker 执行部分；
- Worker execution context 安装与恢复。

入口已改为独立 `.cpp` 编译的稳定 seam，不再以模板读取 `Scheduler::Impl` 字段：

```cpp
void run_worker_loop(RuntimeState&, void* owner_impl, std::size_t worker_index);
```

### 6.3 `runtime_registry`

拥有进程内 RuntimeId 到 `RuntimeDiagnostics` 的 non-owning lookup。Registry 不再保存无类型 `void*`，也不得取得 Runtime 生命周期所有权；Scheduler/Reaper 仍按现有规则拥有和移交 Runtime State。

### 6.4 `ready_queues`

`src/runtime/ready_queues.{hpp,cpp}` 独占 Global EDF/FIFO bands、每 Worker local queues、weighted claim、bounded steal、Immediate cancel cleanup 与 claimed 计数。它只依赖 Metrics 和 AdmissionController，不保存 `Scheduler::Impl*` 或 lifecycle 状态。

### 6.5 `runtime_diagnostics`

`src/runtime/runtime_diagnostics.{hpp,cpp}` 独占 Metrics/Trace 路由、Trace producer attachment、WaitDiagnosticsGuard 以及 wait/await/unobserved-failure hooks。`scheduler.cpp` 只保留等待和 helping 协议入口，通过窄 diagnostics 对象发射事件。

### 6.4 `scheduler.cpp`

最终只保留：

- Scheduler 构造、复制、移动和析构；
- `valid/runtime_id/status/capabilities/metrics_snapshot`；
- `shutdown/shutdown_now`；
- submit/spawn/run 所需的非模板桥接；
- 对 RuntimeState、GraphExecution 和 Finalization 的委托。

预计可收敛到约 400～700 行，但验收应以依赖方向和字段访问边界为准。

## 7. 目标目录结构

```text
src/
├── runtime/
│   ├── scheduler.cpp
│   ├── runtime_registry.{hpp,cpp}
│   ├── runtime_state.{hpp,cpp}
│   ├── ready_queues.{hpp,cpp}
│   ├── runtime_diagnostics.{hpp,cpp}
│   ├── worker_loop.{hpp,cpp}
│   ├── admission_controller.{hpp,cpp}
│   ├── timer_queue.{hpp,cpp}
│   ├── runtime_metrics.{hpp,cpp}
│   └── runtime_identity.hpp
├── task/
│   ├── task_control_block.{hpp,cpp}
│   ├── await_protocol.cpp
│   ├── await_handshake.hpp
│   └── coroutine_resume.hpp
├── graph/
│   ├── graph.cpp
│   ├── graph_execution.{hpp,cpp}
│   ├── graph_runtime_port.hpp
│   └── graph_shared_state.hpp
├── lifecycle/
│   ├── finalization.cpp
│   └── reaper_registry.{hpp,cpp}
├── observability/
│   ├── trace_collector.{hpp,cpp}
│   └── trace_export.cpp
├── scheduling/
│   └── chase_lev_deque.hpp
├── testing/
│   └── test_seam.hpp
└── version.cpp
```

内部 target 使用 `src` 作为 `PRIVATE` include root：

```cpp
#include "runtime/admission_controller.hpp"
#include "graph/graph_execution.hpp"
```

不使用 `../runtime/...`，避免 include 路径依赖调用文件位置。

## 8. CMake 组织

继续只构建一个产品 target：

```cmake
target_sources(AstraScheduler PRIVATE
  src/version.cpp
  src/runtime/scheduler.cpp
  src/runtime/runtime_registry.cpp
  src/runtime/admission_controller.cpp
  src/runtime/timer_queue.cpp
  src/runtime/runtime_metrics.cpp
  src/task/task_control_block.cpp
  src/task/await_protocol.cpp
  src/graph/graph.cpp
  src/graph/graph_execution.cpp
  src/lifecycle/finalization.cpp
  src/lifecycle/reaper_registry.cpp
  src/observability/trace_collector.cpp
  src/observability/trace_export.cpp
)

target_include_directories(AstraScheduler PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)
```

不建议为每个目录建立 object/static library，除非未来出现独立编译、复用或链接边界需求。

## 9. 实施顺序与提交粒度

### Commit 1

`refactor(graph): introduce runtime port and admission lease`

- 新增 GraphRuntimePort；
- Scheduler::Impl 实现适配；
- GraphExecution 改为依赖 port；
- 引入 admission lease；
- 暂不移动文件。

### Commit 2

`refactor(graph): move execution into dedicated implementation unit`

- 新增 `graph_execution.cpp`；
- 把 Graph protocol 移出 `scheduler.cpp`；
- 把递归 lambda 收敛为实例方法；
- 移除 GraphExecution 对 Scheduler 私有表示的访问。

### Commit 3

`refactor(runtime): extract worker loop and runtime registry`

- 抽出 Worker loop；
- 抽出 Runtime lookup/diagnostics；
- Scheduler facade 只保留委托。

### Commit 4

`refactor(layout): organize private sources by subsystem`

- 只移动文件；
- 更新 include、CMake 和 internal test paths；
- 不改变行为。

目录移动放在最后，避免代码所有权重构与大批 rename 混在同一 review 中。

## 10. 验收标准

### 结构门禁

- `GraphExecution` 实现中不出现 `Scheduler`、`Scheduler::Impl` 或 `scheduler.impl_`。
- `scheduler.cpp` 不包含 Graph node dependency propagation 实现。
- Graph 模块不直接访问 RuntimeMetrics、AdmissionController 或 TimerQueue 的字段。
- `src` 头文件不出现在 install manifest。
- public tests 仍然没有 `src` include path。

### 行为验证

- WSL GCC Debug 全量测试通过；
- Graph admission、edge policy、run control、Coroutine identity 测试通过；
- Coroutine frame lifetime 和 resume handshake 测试通过；
- ASan/UBSan Graph/Coroutine/admission suite 通过；
- TSan 或等价并发验证覆盖 Graph completion/cancel 与 Worker publication；
- semantic API freeze gate 通过；
- encapsulation negative compile gate 通过；
- static/shared package consumer gate 通过；
- `git diff --check` 通过。

### 复杂度验收

- Graph admission、构造和 publish-before-start 失败只有一个 rollback owner；
- node terminal transition 只有一个协议入口；
- Scheduler facade 不需要理解 Graph edge propagation；
- GraphExecution 不需要理解 Runtime queue、metrics shard 或 timer storage 的具体表示；
- 文件移动后不存在循环 include 或通过 `../` 建立的脆弱跨目录依赖。

## 11. 主要风险与控制

### 生命周期风险

Graph callback 从捕获 `Impl*` 改为捕获 GraphExecution 时，必须证明 Runtime 和 GraphExecution 的生命周期顺序。不得让 GraphExecution 强持有整个 Scheduler Handle 来掩盖问题，否则可能改变 shutdown completion。

### Admission 重复释放

RAII lease commit 后必须明确 slot ownership 已转移给 node terminal 路径。测试应覆盖构造前失败、部分 node state 构造失败、root publication 失败、取消传播和正常完成。

### Metrics 口径漂移

移动 accepted/rejected/active graph counters 时，应保持原线性化点，不得因为模块拆分改变 snapshot 关系。

### Worker 热路径开销

若 GraphRuntimePort 使用虚调用，应测量 node publication 热路径；必要时可以采用 non-owning function table 或模板化 adapter，但不得重新暴露 Runtime 字段。

### Review 噪声

职责重构和目录 rename 必须分开提交。目录移动提交应尽量保持内容字节不变，以便 Git 识别 rename。

## 12. 与权威设计来源的关系

本文是后续设计和拆票输入，不直接覆盖：

- `.scratch/astra-scheduler-runtime/spec.md`；
- `.scratch/astra-scheduler-runtime/decision-log.md`；
- `.scratch/astra-scheduler-runtime/issues/`；
- `docs/adr/0047-linux-only-support-and-wsl-development.md`。

正式实施前，应先确认该重构不改变 approved observable semantics；如需新增 GraphRuntimePort、GraphAdmissionLease、RuntimeState ownership 等规范性约束，应先更新 Decision Ledger、Spec 和对应 Tickets，再按依赖顺序实施。
