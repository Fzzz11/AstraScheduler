# AST-072 — 抽出拥有 Ready work 协议的 ReadyQueues

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-129)
Milestone: v1.2.0
Blocked by: AST-070
Status: done
Claimed by: agent

## Rules and decisions

- R-129 [primary]；source: D-178
- R-064, R-081, R-082, R-083 [supporting]

## What to build

建立 `src/runtime/ready_queues.{hpp,cpp}`，完整拥有Global EDF/FIFO bands、per-Worker local queues、weighted claim、bounded steal、immediate cancel cleanup与inspection，不持有`Scheduler::Impl*`。

## Acceptance criteria

- [x] RuntimeState/Impl不再定义global/local queue容器。
- [x] ReadyQueues不持有Impl*或lifecycle状态。
- [x] priority、EDF、local routing、steal与immediate cancel测试通过。

## Traceability

- Decision: D-178
- Verification: WSL Debug 53/53；ASan/UBSan 12/12 targeted；TSan 7/7 targeted；`check_encapsulation` passed。
