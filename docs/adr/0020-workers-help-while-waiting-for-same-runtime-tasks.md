---
status: accepted
date: 2026-08-26
decisions: [D-048, D-049, D-050, D-051, D-062, D-065, D-066, D-078, D-079, D-080]
---

# Workers help while waiting for same-Runtime tasks

同 Scheduler Worker 对另一个未完成任务调用 `get()` 时采用 Helping Wait：当前 Callable 保留在栈上，Worker 继续通过该 Runtime 的正常调度路径执行 Eligible Task，直到目标 Terminal Outcome 发布。相比直接阻塞 OS Worker 或拒绝这种调用，该方案支持单 Worker fork-join 并避免 Worker Group 因嵌套结果等待而耗尽，但引入 Task Context 切换、嵌套执行和循环依赖风险。

v0.1.0 通过 Global Injection Queue 帮助执行，后续版本使用其正常 local/global/steal 顺序；不创建补偿线程。Direct Self-Wait 在任何等待或帮助副作用前抛出 `std::logic_error`；嵌套深度和 Shutdown 交互由后续独立决策固定。

Runtime 不维护通用动态 wait-for graph，也不承诺检测 Indirect Wait Cycle；这种环可以让相关任务永久 Helping Wait。可验证无环的显式依赖使用 TaskGraph/DAG，并由 DAG 在执行前执行 cycle detection。该边界避免把图维护和可达性检查加入每次 TaskHandle `get()` 的基础路径。

Worker 等待另一 Runtime 的 Task 时采用 Cross-Runtime Helping Wait：它继续帮助自己的源 Runtime，并只观察目标 Runtime 的 Terminal Outcome，绝不执行、窃取或内联目标 Runtime 的任务。这样保留 WorkerContext、Local Queue、Metrics 与 Shutdown 的 Runtime 归属，同时避免把源 Worker 专用于普通阻塞。

TaskHandle 的 `wait()` 与 `get()` 使用同一 caller-relative 进度协议：非 Worker 执行 Unbounded Wait，同/跨 Runtime Worker 执行对应 Helping Wait，Direct Self-Wait 抛 `std::logic_error`。差别只在 Terminal Outcome 到达后是否读取结果。

正 duration 的 `wait_for()` 延续同一 caller-relative 进度协议，但在 deadline 到达时允许退出；任何 duration 的 Direct Self-Wait 仍优先抛出。由于 helped Callable 非抢占，deadline 是下一次完成/超时观察边界，不是硬实时返回上限；Worker 在 deadline 前开始的 Callable 可以使实际回程无界越过 timeout。

`SchedulerOptions::max_helping_depth` 默认 64 且必须为正；达到上限后，在启动下一层帮助前抛出可捕获的 `helping_depth_exceeded`，而不阻塞 Worker、伪造 timeout 或取消目标。该阈值限制普通 C++ 栈递归，但不承诺字节级 stack safety。

Helping Wait 每次选择任务都服从源 Runtime 的正常 Shutdown eligibility：Graceful 只帮助 Drain Work Closure，Immediate 后不启动任何新任务；跨 Runtime 等待也只观察目标 Outcome，不接受目标 Runtime 的执行权限。

决策细节见 [D-048 至 D-051、D-062、D-065、D-066 与 D-078 至 D-080](../../.scratch/astra-scheduler-runtime/decision-log.md)。
