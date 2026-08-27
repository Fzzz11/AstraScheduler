---
status: accepted
date: 2026-08-26
decisions: [D-069, D-070, D-071]
---

# Public TaskState hides scheduler transients

AstraScheduler 的稳定公共 `TaskState` 固定为 `Waiting/Ready/Running/Suspended/Succeeded/Failed/Cancelled`。它覆盖 Callable、DAG 与 Coroutine 需要共享的生命周期语言，但不暴露 `Created`、enqueue publication、queue claim、start/cancel arbitration 或 Outcome publishing 等会随调度算法变化的内部瞬态；`Succeeded` 刻意取代含混的 `Completed`，使三个公共终态与 Value/Exception/Cancelled Terminal Outcome 一一对应。

`TaskHandle::state()` 是可并发调用的非阻塞线性化快照，不执行等待、Helping 或控制动作；非终态快照返回后可以立即过时。Terminal Outcome 内容先构造，再与对应终态通过同一个 completion publication 对外可见并通知等待者，因此观察到终态的线程也已经能无额外等待地观察完整 Outcome。

决策细节见 [D-069 至 D-071](../../.scratch/astra-scheduler-runtime/decision-log.md)。
