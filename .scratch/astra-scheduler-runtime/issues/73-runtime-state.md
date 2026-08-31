# AST-073 — 抽出唯一组合所有者 RuntimeState

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-130)
Milestone: v1.2.0
Blocked by: AST-072
Status: done
Claimed by: agent

## Rules and decisions

- R-130 [primary]；source: D-178
- R-097, R-103, R-108 [supporting]

## What to build

建立 `src/runtime/runtime_state.{hpp,cpp}`，唯一拥有Runtime身份、resolved options、status、深模块组合与Worker同步/线程；Trace producer attachment由其组合的RuntimeDiagnostics独占；Impl收敛为shared ownership与port adapter，Worker loop改为compiled RuntimeState seam。

## Acceptance criteria

- [x] Impl不复制RuntimeState拥有的字段。
- [x] Worker loop不再模板读取Impl字段。
- [x] startup/shutdown/handoff/reaper/worker测试通过。

## Traceability

- Decision: D-178
- Verification: WSL Debug 53/53；ASan/UBSan 12/12 targeted；TSan 7/7 targeted；`check_encapsulation` passed。
