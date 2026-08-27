# AST-002 — 建立 compiled library 与可安装 CMake package

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-110, R-111)
Milestone: Phase 0
Blocked by: AST-001
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-110 [primary] — CMake package 隐藏实现并验证独立consumer；source: D-167, D-145
- R-111 [supporting] — Supported Configuration 仅限 64-bit Linux；source: D-167

## What to build

在WSL/Linux中创建 C++20 compiled library、隐藏实现目录、安装并导出 `AstraScheduler::AstraScheduler`，加入仓库外独立 Linux consumer 测试。

## Invariants

- `[R-110]` public headers必须仅安装于include/astra且不泄漏internal/第三方依赖，CMake target声明cxx_std_20并导出AstraSchedulerConfig.cmake/version file；tests/examples/benchmarks/tools由ASTRA_BUILD_*控制且consumer默认不下载其依赖；static/shared共用语义/tests，public symbol经export macro控制，internal symbol hidden，独立find_package/link/run smoke必须通过；不支持-fno-exceptions，core不要求RTTI。 例外边界：warnings-as-errors、sanitizer与内部编译选项不传播给consumer。
- `[R-111]` AstraScheduler的Supported Configuration必须仅包含64-bit Linux：Tier-1为Linux x86_64 GCC13+/Clang17+，Tier-2为native Linux AArch64 GCC/Clang weak-memory验证；Windows/MSVC、macOS、其他非Linux OS与32-bit目标必须标记为unsupported且不得进入release gate、package支持声明或性能/正确性承诺，偶然编译成功不得升级支持状态。 例外边界：compiled CMake package由R-110约束，single implementation instance由R-107约束；Linux atomic能力不足时按R-101报告Locked fallback而不改变平台支持状态。

## Test-first seam

- Public seam: Phase0、install/export、static/shared release package。；Linux build/install/release、CI与package metadata。
- RED evidence: 先在WSL写独立Linux consumer的configure/build测试，证明未安装、泄漏私有include或错误声明非Linux支持时失败。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-110]` 安装目录可被独立最小工程消费，consumer compile line不包含项目内部依赖或强制诊断选项。
- [ ] `[R-111]` C++20 compiled static默认target与可选shared target均可在Linux完成install/consume smoke；完整Linux Tier matrix与single implementation instance验证留给AST-052/AST-053。

## Out of scope

- 不实现 Runtime 调度行为；仅允许为编译、安装和测试建立最小私有 seam。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-110, R-111
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-167, D-145
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending
