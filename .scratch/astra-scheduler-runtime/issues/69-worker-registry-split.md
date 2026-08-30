# AST-069 — 抽出 Worker loop 与 Runtime registry

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-127)
Milestone: v1.2.0
Blocked by: AST-068
Status: done
Claimed by: agent

## Rules and decisions

- R-127 [primary]；source: D-177

## What to build

按完整协议迁移Worker loop与non-owning Runtime registry/diagnostics，使Scheduler facade只保留校验和委托。

## Acceptance criteria

- [x] scheduler.cpp不定义worker_main或全局Runtime map。
- [x] Worker/weak-memory/shutdown/TSan语义不变。

## Traceability

- Decision: D-177
- Verification: `src/runtime/worker_loop.hpp`、`src/runtime/runtime_registry.{hpp,cpp}`；worker/weak-memory/shutdown tests；WSL TSan gate。
