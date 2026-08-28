# AST-028 — 实现 consuming TaskGraph freeze 与 NodeId 验证

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-069)
Milestone: v0.4.0
Blocked by: AST-004, AST-009
Status: done
Claimed by: agent

## Rules and decisions

- R-069 [primary] — TaskGraph consuming freeze 与 NodeId 验证固定；source: D-104, D-105, D-161

## What to build

构建 mutable builder→validated move-only Frozen Graph 的 consuming freeze；按插入顺序分配强类型 NodeId，拒绝坏边与 cycle。

## Invariants

- `[R-069]` `TaskGraph`必须是caller-serialized move-only builder，emplace返回graph-local强类型NodeId；`freeze() &&`消费并验证foreign/self/duplicate/cycle后产生immutable single-shot `FrozenTaskGraph`，失败抛 `astra::graph_validation_error : logic_error`且 `reason()`稳定返回GraphValidationError::{ForeignNode,SelfEdge,DuplicateEdge,Cycle}，Cycle携带首尾同Node的确定NodeId witness；NodeId从nonzero checked insertion sequence分配且不公开GraphNodeId别名。 例外边界：空图合法；freeze失败builder仅保证可析构/重新赋值。

## Test-first seam

- Public seam: DAG定义阶段与Graph validation error。
- RED evidence: 先写空图、重复/越界 edge、cycle、move-only node、freeze 后不可变和二次消费失败测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-069]` 非DAG输入在admission前确定失败，freeze不重编号且move-only Node可用。

## Out of scope

- 不实现可复用 mutable graph template、数据流值传播或 Coroutine node 行为。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-069
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-104, D-105, D-161
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:
  - 架构与实现：`include/astra/graph.hpp` 与 `src/graph.cpp` 实现了 move-only `TaskGraph` builder 与 immutable single-shot `FrozenTaskGraph`；实现了基于 Kahn + 确定性 DFS 的图结构校验器；`include/astra/error.hpp` 实现了 `GraphValidationError::{ForeignNode,SelfEdge,DuplicateEdge,Cycle}` 及携带确定性 `cycle_witness` 的 `graph_validation_error`。
  - 单元测试：`tests/test_task_graph_freeze.cpp` 覆盖 R-069（空图合法性、Move-only Callable 节点支持、按插入顺序严格递增分配 NodeId、ForeignNode 越界校验、SelfEdge 自环校验、DuplicateEdge 重复边校验、Cycle 环路检测与首尾闭合确定性 witness 提取、复杂 DAG 与非连通环路隔离）。
  - In-tree CTest：`wsl bash -lc "ctest --test-dir build/wsl-gcc-debug --output-on-failure"` 26/26 tests 全部 PASS。
  - ASan / UBSan / LSan 内存安全与泄漏门禁：`build/wsl-gcc-asan` 26/26 tests 全部 PASS（0 leaks / 0 errors / 0 deadlocks）。
  - Package consumer 与安装门禁：`python3 -X utf8 tools/check_cmake_package.py` 43/43 tests 全部 OK。
  - 发布门禁：`python3 -X utf8 tools/check_release_gates.py` 15/15 tests 全部 OK。

