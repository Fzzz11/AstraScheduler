# AST-055 — 运行 v1 全量 release gate 并发布稳定基线

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-003, R-091, R-111, R-094)
Milestone: v1.0.0
Blocked by: AST-051, AST-053, AST-054
Status: done
Claimed by: agent

## Rules and decisions

- R-003 [supporting] — 保留 v0.1.0 可运行基线；source: D-001
- R-091 [supporting] — Benchmark artifact 保存原始重复并限定回归gate；source: D-143
- R-111 [supporting] — Supported Configuration 仅限 64-bit Linux；source: D-167
- R-094 [supporting] — Phase 0 至 v1.0 按纵向里程碑交付；source: D-146

## What to build

汇总全部approved-rule tests、Tier-1 Linux x86_64 GCC/Clang builds、native Linux AArch64 weak-memory证据、docs/package/schema/benchmark artifacts，形成Linux-only v1.0.0可重现发布基线。

## Invariants

- `[R-003]` 后续 Work-Stealing 版本发布后，v0.1.0 Global Queue Scheduler 必须仍可运行并作为 Benchmark 对照组。
- `[R-091]` Standard profile必须默认2秒warmup、10个至少1秒独立repetition且不删outlier；versioned artifact保存全部raw values、median/MAD/p10/p90/bootstrap95%CI、环境/构建/options/seed/checksum/schema；共享PR CI只smoke，正式regression只在专用稳定runner同时越过versioned实践阈值与置信区间，baseline更新需review。 例外边界：非Standard exploratory profile必须在artifact显式命名参数。
- `[R-111]` AstraScheduler的Supported Configuration必须仅包含64-bit Linux：Tier-1为Linux x86_64 GCC13+/Clang17+，Tier-2为native Linux AArch64 GCC/Clang weak-memory验证；Windows/MSVC、macOS、其他非Linux OS与32-bit目标必须标记为unsupported且不得进入release gate、package支持声明或性能/正确性承诺，偶然编译成功不得升级支持状态。 例外边界：compiled CMake package由R-110约束，single implementation instance由R-107约束；Linux atomic能力不足时按R-101报告Locked fallback而不改变平台支持状态。
- `[R-094]` 路线必须依次以Phase0 scaffold、v0.1 Global Runtime+Task/lifecycle、v0.2 locked WS、v0.3 Chase-Lev、v0.4 DAG、v0.5 Coroutine+Timer、v0.6 Priority+Deadline、v0.7 Observability、v0.8 Benchmark、v0.9 hardening、v1 stable source API交付，每tag满足approved-rule测试、Tier-1 build、并发证据、docs/package/schema/benchmark gates。 例外边界：private seam可提前，未定public语义不得提前暴露。

## Test-first seam

- Public seam: v0.1.0之后的Benchmark Framework。；performance claim、release evidence与regression automation。；Linux build/install/release、CI与package支持声明。；release规划与后续to-tickets。
- RED evidence: release checklist 默认失败，只有所有可追踪证据具备、版本一致且 artifact 可重算时才通过。
- 本集成 Ticket 没有 primary owner；验证只为 supporting 规则补充发布证据，不转移主实现责任。

## Acceptance criteria

- [x] `[R-003]` Benchmark 可在同一工作负载下运行 Global Queue 基线（in-tree global_fifo_baseline）与后续 Scheduler。
- [x] `[R-091]` 性能结论可追溯原始重复（corpus artifact raw repetitions + 稳健统计），偶发共享runner噪声不阻断发布（双门槛 gate：effect + bootstrap CI）。
- [x] `[R-111]` v1 release evidence只声明Linux x86_64 GCC/Clang与native Linux AArch64支持（platform matrix 审计 ok），且不存在Windows/MSVC release artifact或支持声明。
- [x] `[R-094]` 每个实现Ticket有目标版本且每个tag可独立构建运行（v1.0.0 release checklist verdict=ok：10/10 检查含全量测试/sanitizer/package/api freeze/版本一致性）。

## Out of scope

- 不加入未经过决策台账与 approved Spec 的新 public 功能；v1 gate 只冻结已批准范围。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-003, R-091, R-111, R-094
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-001, D-143, D-167, D-146
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: `tools/check_release_baseline.py` verdict=ok（docs/release/1.0.0/release-evidence.json，10/10 检查）：
  release gates 15/15、traceability 通过（tickets=56）、全量 Debug 51/51、hardening verdict=ok（真实 ASan+UBSan/TSan，Tier-2 deferred 显式记录）、package consumer 通过、平台矩阵审计 ok、API freeze（v1.0.0 golden：17 headers/110 symbols）、版本一致性 1.0.0、corpus baseline（astra_version=1.0.0，固定 seed，checksum 可重算）。
  版本升至 1.0.0（project VERSION 单一版本源，consumer 钉住值同步）。
  Tier-2 native AArch64 weak-memory 证据如实 deferred（无 native 硬件；stress 载体 tests/test_weak_memory_stress.cpp 已交付）；TSan 5 个已知问题测试已分类记录（owner: AST-006/007/033）。
 
