# AST-054 — 冻结 v1 public source/semantic compatibility surface

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-004, R-093, R-094)
Milestone: v1.0.0
Blocked by: AST-031, AST-037, AST-041, AST-048, AST-053
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-004 [supporting] — 规格覆盖跨版本 Runtime；source: D-004
- R-093 [supporting] — SemVer保证source/semantic并分离header/library version；source: D-145, D-164
- R-094 [supporting] — Phase 0 至 v1.0 按纵向里程碑交付；source: D-146

## What to build

审计并冻结 documented public headers、namespace、types、signatures、error/exception 与 observable semantics；明确不承诺跨 toolchain ABI。

## Invariants

- `[R-004]` AstraScheduler 的设计规格必须覆盖整个跨版本 Runtime；仅适用于某版本的规则必须显式标出版本范围。
- `[R-093]` 0.x minor可经decision+migration产生breaking change而patch不得计划性breaking，v1起documented source/observable semantics按SemVer；不保证跨版本/toolchain ABI。必须公开可比较的 `Version{uint32 major,minor,patch}`、ASTRA_VERSION_MAJOR/MINOR/PATCH、constexpr header_version()及无分配/无锁/不初始化Runtime的library_version()/library_version_string()，后者string_view指向进程期静态规范SemVer文本；schema版本独立，CMake exact-version检查为主要mismatch边界。 例外边界：runtime查询不使错误header/binary组合成为受支持。
- `[R-094]` 路线必须依次以Phase0 scaffold、v0.1 Global Runtime+Task/lifecycle、v0.2 locked WS、v0.3 Chase-Lev、v0.4 DAG、v0.5 Coroutine+Timer、v0.6 Priority+Deadline、v0.7 Observability、v0.8 Benchmark、v0.9 hardening、v1 stable source API交付，每tag满足approved-rule测试、Tier-1 build、并发证据、docs/package/schema/benchmark gates。 例外边界：private seam可提前，未定public语义不得提前暴露。

## Test-first seam

- Public seam: 本规格及后续实质修订。；release、shared package、artifact与consumer诊断。；release规划与后续to-tickets。
- RED evidence: 先生成 public API compile matrix 与 golden manifest，任何未记录的 surface 漂移都使 gate 失败。
- 本集成 Ticket 没有 primary owner；验证只为 supporting 规则补充发布证据，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-004]` 规格规则带有 Applies to，且不会把整体目标误写为单版本范围。
- [ ] `[R-093]` 同一安装header/library版本一致，查询不启动Reaper或分配。
- [ ] `[R-094]` 每个实现Ticket有目标版本且每个tag可独立构建运行。

## Out of scope

- 不加入未经过决策台账与 approved Spec 的新 public 功能；v1 gate 只冻结已批准范围。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-004, R-093, R-094
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-004, D-145, D-164, D-146
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending
