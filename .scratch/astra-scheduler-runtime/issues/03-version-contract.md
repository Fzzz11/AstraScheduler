# AST-003 — 提供 header/library 版本查询与 mismatch 诊断

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-093)
Milestone: Phase 0
Blocked by: AST-002
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-093 [primary] — SemVer保证source/semantic并分离header/library version；source: D-145, D-164

## What to build

实现 SemVer header 宏、`header_version()`、无副作用 `library_version()`/`library_version_string()`，定义 header/library mismatch 的可诊断行为。

## Invariants

- `[R-093]` 0.x minor可经decision+migration产生breaking change而patch不得计划性breaking，v1起documented source/observable semantics按SemVer；不保证跨版本/toolchain ABI。必须公开可比较的 `Version{uint32 major,minor,patch}`、ASTRA_VERSION_MAJOR/MINOR/PATCH、constexpr header_version()及无分配/无锁/不初始化Runtime的library_version()/library_version_string()，后者string_view指向进程期静态规范SemVer文本；schema版本独立，CMake exact-version检查为主要mismatch边界。 例外边界：runtime查询不使错误header/binary组合成为受支持。

## Test-first seam

- Public seam: release、shared package、artifact与consumer诊断。
- RED evidence: 先写版本相等、查询不启动 Runtime/Reaper、模拟 mismatch 可被发现的 consumer 测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-093]` 同一安装header/library版本一致，查询不启动Reaper或分配。

## Out of scope

- 不实现 Runtime 调度行为；仅允许为编译、安装和测试建立最小私有 seam。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-093
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-145, D-164
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending

