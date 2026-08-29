# AST-052 — 验证 Linux-only Tier matrix 与单 Astra implementation instance 部署

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-111, R-107)
Milestone: v0.9.0
Blocked by: AST-003, AST-007, AST-027, AST-044
Status: in-progress
Claimed by: agent

## Rules and decisions

- R-111 [primary] — Supported Configuration 仅限 64-bit Linux；source: D-167
- R-107 [supporting] — Supported Configuration 只有一个实现实例与Reaper coordinator；source: D-021, D-159

## What to build

固定Linux-only Tier matrix：Tier-1 Linux x86_64 GCC/Clang、Tier-2 native Linux AArch64；验证non-Linux不进入release/package支持声明，并验证supported deployment只有一个实现实例与coordinator/ID/metrics domain。

## Invariants

- `[R-111]` AstraScheduler的Supported Configuration必须仅包含64-bit Linux：Tier-1为Linux x86_64 GCC13+/Clang17+，Tier-2为native Linux AArch64 GCC/Clang weak-memory验证；Windows/MSVC、macOS、其他非Linux OS与32-bit目标必须标记为unsupported且不得进入release gate、package支持声明或性能/正确性承诺，偶然编译成功不得升级支持状态。 例外边界：compiled CMake package由R-110约束，single implementation instance由R-107约束；Linux atomic能力不足时按R-101报告Locked fallback而不改变平台支持状态。
- `[R-107]` R-111定义的Linux-only Supported Configuration中，一个进程必须只加载一个Astra Implementation Instance，并由其中一个逻辑Reaper Service和恰好一条不属于任何Scheduler的专用coordinator服务全部Runtime；不得按Scheduler/handoff增加Reaper线程，coordinator不得执行用户任务或参与steal；多个DSO各自静态嵌入实现不享有这些process-wide保证。 例外边界：从未建立服务且空集合Finalization不创建coordinator。

## Test-first seam

- Public seam: Linux build/install/release、CI、package metadata与process-wide Reaper/ID/metrics保证。；process-wide Reaper、Finalization gate、ID allocator与Process Metrics拓扑。
- RED evidence: 先写Linux-only build-matrix manifest、无Windows/MSVC release job审计、shared consumer、多个plugin共享exact-version library和不受支持双static copy的负向部署测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-111]` release matrix和package支持声明只包含Linux x86_64 GCC/Clang与native Linux AArch64，不存在Windows/MSVC release job；非Linux结果不得标记Supported。
- [ ] `[R-107]` 支持配置中Scheduler数量不增加coordinator数，unsupported duplicate instance被部署文档/测试明确拒绝。

## Out of scope

- 不承诺跨 compiler、stdlib、CRT 或版本的 binary ABI，也不把仅能编译的平台标为 Supported。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-111, R-107
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-167, D-159, D-021
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending
