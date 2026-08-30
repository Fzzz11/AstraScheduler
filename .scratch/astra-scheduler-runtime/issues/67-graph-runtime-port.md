# AST-067 — 建立 GraphRuntimePort 与 admission lease

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-125)
Milestone: v1.2.0
Blocked by: AST-066
Status: done
Claimed by: agent

## Rules and decisions

- R-125 [primary]；source: D-177

## What to build

建立非安装GraphRuntimePort，让Graph只使用Runtime能力；引入单一RAII reservation owner，删除Graph对Scheduler private representation的依赖。

## Acceptance criteria

- [x] GraphExecution不引用Scheduler/Impl字段。
- [x] 构造、identity、edge allocation与publication前失败统一回滚。
- [x] Graph/admission行为测试通过。

## Traceability

- Decision: D-177
- Verification: `src/graph/graph_runtime_port.hpp`；`astra_graph_runtime_port_test`；WSL Debug ctest。
