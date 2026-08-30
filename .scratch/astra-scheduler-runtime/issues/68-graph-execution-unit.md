# AST-068 — 将 GraphExecution 迁入独立实现单元

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-126)
Milestone: v1.2.0
Blocked by: AST-067
Status: done
Claimed by: agent

## Rules and decisions

- R-126 [primary]；source: D-177

## What to build

把Graph协议迁入graph_execution.cpp，以实例方法拥有publication、successor release、cancel与terminal。

## Acceptance criteria

- [x] scheduler.cpp不含Graph依赖传播实现。
- [x] GraphExecution不再是仅含static run的命名空间替代品。
- [x] Graph/Coroutine/ASan测试通过。

## Traceability

- Decision: D-177
- Verification: `src/graph/graph_execution.cpp`；Graph/Coroutine tests；WSL Debug/ASan gates。
