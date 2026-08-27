---
status: accepted
date: 2026-08-26
decisions: [D-104, D-105, D-106, D-107, D-108, D-123, D-161]
---

# Task graphs are validated single-shot executions

`TaskGraph` 是 caller-serialized move-only builder；`freeze() &&` 消费并验证 foreign/self/duplicate/cycle，产生结构不可变、move-only、只可执行一次的 `FrozenTaskGraph`。这保留 move-only Node Callable，不为复用强加 copy/factory 协议；Cycle error 提供确定、真实的 NodeId witness。

公共节点标识统一为 graph-local 强类型 `NodeId`：0 为 invalid，合法值按 builder 插入顺序 checked-monotonic 分配且 freeze 不重编号。历史决策文字中的 `GraphNodeId` 只是旧拼写，不形成 alias 或第二个公共类型；运行节点的完整关联是 `GraphRunId + NodeId + TaskId`。

External GraphRun all-or-nothing 地为全部 Waiting/Ready Node 占用 External Pending slot，过大图立即拒绝；same-Runtime Internal Graph 豁免 slot但仍全部计入 Drain Work Closure。Node completion 先发布 Terminal Outcome，再按每 edge exactly-once countdown；唯一 1→0 owner acquire 汇合 predecessor disposition，并恰好一次 Ready/terminal successor。

Graph Node 是返回 void 的控制任务，不自动形成 typed dataflow 或 per-node TaskHandle；非 void Node 在编译期拒绝，数据 ownership 由显式共享对象或 Graph 外的普通 TaskHandle 表达。

显式`emplace_coroutine(Task<void>&&)`允许同一个void Node在Ready/Running/Suspended间恢复，不创建child Task Identity、Handle或slot；普通emplace不隐式unwrap Coroutine Task。

决策细节见 [D-104 至 D-108、D-123 与 D-161](../../.scratch/astra-scheduler-runtime/decision-log.md)。
