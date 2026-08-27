---
status: accepted
date: 2026-08-26
decisions: [D-118, D-119, D-154]
---

# Suspended Coroutine cancellation is cooperative

Astra内建awaiter用register→suspend commit→arm/trigger handshake连接语言suspend与Runtime Ready publication，覆盖completion-before/during/after registration且最多发布一个resume ticket。Completion/stop callback只做noexcept竞争、enqueue与notify，绝不直接resume frame。

Suspended cancellation仍是cooperative：内建awaiter让stop参与trigger竞争，stop winner在source Runtime排队resume并由await_resume抛`task_cancelled`；任意foreign awaitable没有标准注销协议，Runtime只设置stop request并等待自然resume，可能永久Pending。为避免外部callback UAF，不强毁frame或伪造Cancelled。

Immediate只禁止从未开始Task的first start；已经开始过的Coroutine即使当前Suspended/Ready，仍可执行唯一resume segment到取消点、自然completion或RAII unwind。用户可捕获取消继续，foreign awaitable也可能永不恢复，因此Immediate仍无有界保证。

决策细节见 [D-118、D-119 与 D-154](../../.scratch/astra-scheduler-runtime/decision-log.md)。
