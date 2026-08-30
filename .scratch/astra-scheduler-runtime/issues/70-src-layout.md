# AST-070 — 按子系统重组 private src

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-128)
Milestone: v1.2.0
Blocked by: AST-069
Status: done
Claimed by: agent

## Rules and decisions

- R-128 [primary]；source: D-177

## What to build

以纯rename提交将src组织为runtime/task/graph/lifecycle/observability/scheduling/testing，更新CMake与internal includes，保持单一产品target和非安装边界。

## Acceptance criteria

- [x] 无跨模块../ include。
- [x] src头不安装，public tests无src include path。
- [x] Debug/sanitizer/API/package/encapsulation gates通过。

## Traceability

- Decision: D-177
- Verification: `src/{runtime,task,graph,lifecycle,observability,scheduling,testing}`；CMake、benchmark、package、encapsulation gates。
