---
status: accepted
date: 2026-08-26
decisions: [D-099, D-100]
---

# Chase-Lev retains old buffers until Worker teardown

Chase-Lev buffer 只双倍增长，运行期不 shrink、回收或复用旧地址；所有 generations 在 Worker/deque quiescent 且 Runtime 完成 join 后统一释放。这直接保留 weak-memory proof 的 old-arrays-never-reused 假设，代价是每个 deque 按历史峰值保留内存，但 doubling 使总 buffer capacity 小于 active capacity 的两倍。

每个 Ready Task 只有一个逻辑 Scheduling Reference，resize 复制 raw TCB pointer 不复制 ownership，losing thief 在 top CAS 成功前不得解引用。已接受 Task 的 Local resize 分配失败回退到 TCB 内嵌 intrusive link 的 allocation-free Global Queue，绝不丢失、inline 执行或伪造成业务失败。

决策细节见 [D-099 与 D-100](../../.scratch/astra-scheduler-runtime/decision-log.md)。
