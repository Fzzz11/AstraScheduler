# AST-001 — 建立里程碑交付矩阵与规则门禁

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-004, R-005, R-094, R-112)
Milestone: Phase 0
Blocked by: None
Status: done
Claimed by: CodeBuddy agent + Codex Linux/WSL revision (2026-08-27)

## Rules and decisions

- R-004 [primary] — 规格覆盖跨版本 Runtime；source: D-004
- R-005 [primary] — 实现工作按版本拆票；source: D-005
- R-094 [primary] — Phase 0 至 v1.0 按纵向里程碑交付；source: D-146
- R-112 [primary] — 本机开发与验证命令必须在 WSL Linux 内执行；source: D-168

## What to build

在仓库文档和 CI 配置入口中固定 Phase 0 至 v1.0 的里程碑、每 tag DoD、规则追踪入口和“approved Spec 才能拆票/实现”的门禁；同时固定Linux-only支持范围、WSL本机命令入口与跨环境build-cache隔离门禁。

## Invariants

- `[R-004]` AstraScheduler 的设计规格必须覆盖整个跨版本 Runtime；仅适用于某版本的规则必须显式标出版本范围。
- `[R-005]` 后续实现必须拆分为带目标版本的多个 Tickets，不得把整个 AstraScheduler 合并为单一实现 Ticket。 例外边界：文档维护或不产生实现的管理工作不属于实现 Ticket。
- `[R-094]` 路线必须依次以Phase0 scaffold、v0.1 Global Runtime+Task/lifecycle、v0.2 locked WS、v0.3 Chase-Lev、v0.4 DAG、v0.5 Coroutine+Timer、v0.6 Priority+Deadline、v0.7 Observability、v0.8 Benchmark、v0.9 hardening、v1 stable source API交付，每tag满足approved-rule测试、Tier-1 build、并发证据、docs/package/schema/benchmark gates。 例外边界：private seam可提前，未定public语义不得提前暴露。
- `[R-112]` 所有本机configure、build、test、format、lint、package consumer、sanitizer、stress、benchmark与release verification命令必须在WSL Linux用户空间执行，canonical workspace为`/mnt/d/code/cppStudy/AstraScheduler`；Windows PowerShell/cmd仅可启动WSL或做非开发性宿主编排，不得直接运行Windows-native toolchain或项目二进制形成验证证据，Windows与WSL不得复用build cache或产物。 例外边界：文件编辑工具可操作共享工作区；CI/release可以在native Linux runner执行，WSL不是最终运行时依赖；本机WSL benchmark的正式性能claim仍需按R-089至R-091记录环境并可由native Linux复核。

## Test-first seam

- Public seam: 本规格及后续实质修订。；本规格批准后的 Ticket 规划。；release规划与后续to-tickets。；本机开发者、Coding Agent、Ticket verification与本机Benchmark artifact。
- RED evidence: 先增加会因缺少里程碑、规则引用、WSL开发入口、cache隔离或release gate而失败的文档/配置校验测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-004]` 规格规则带有 Applies to，且不会把整体目标误写为单版本范围。
- [x] `[R-005]` 每个实现 Ticket 记录目标版本，且不存在覆盖完整项目的单一实现 Ticket。
- [x] `[R-094]` 每个实现Ticket有目标版本且每个tag可独立构建运行。
- [x] `[R-112]` 仓库指令、开发文档与Ticket verification只给出WSL/Linux命令，WSL build目录与Windows native cache隔离，任何通过声明均可追溯到WSL或native Linux输出。

## Out of scope

- 不实现 Runtime 调度行为；仅允许为编译、安装和测试建立最小私有 seam。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-004, R-005, R-094, R-112
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-004, D-005, D-146, D-168
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-27；Linux-only/WSL修订已在WSL复验）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-004 | `tools/check_release_gates.py::R004SpecScopeTests` — spec `Status: approved` 门禁、每条规则非空 `Applies to`、spec 声明跨版本范围（Problem Statement/Goals 含 v0.1.0 与 v1.0、纵向里程碑、跨版本锚点） | `Ran 6 tests ... FAILED (errors=1)`：缺少 `docs/release/milestones.md` 使 gate suite 失败；R-004 审计项当时通过并形成回归保护 | `Ran 11 tests ... OK` |
| R-005 | `R005TicketVersioningTests` — 每个 issue 恰好一个允许的 `Milestone` 目标版本、primary 引用的规则必须存在于 spec、无 Ticket 以 primary 覆盖全部 active 规则 | 同上 | `Ran 11 tests ... OK` |
| R-094 | `R094MilestoneGateTests` — `docs/release/milestones.md` 矩阵含 Phase 0 至 v1.0.0 全部 11 个 tag、恰好一次、按交付顺序排列；每 tag 行 scope/release evidence 非空；统一 DoD 含 approved-rule 测试、Tier-1 build、并发证据、docs/package/schema/benchmark gates 与“可独立构建运行”；规则追踪入口链接；`.github/workflows/ci.yml` 运行同一校验 | `FileNotFoundError: required file is missing: docs/release/milestones.md`（缺少里程碑矩阵与 release gate 即 RED 失败原因） | `Ran 11 tests ... OK` |
| R-112 | `R112WslDevelopmentGateTests` — `AGENTS.md`、`docs/development.md`、里程碑WSL命令与完成Ticket的verification command审计 | `Ran 15 tests ... FAILED (failures=2)`：AST-001未声明R-112且milestones缺少Local WSL development gate | `Ran 15 tests ... OK` |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 /mnt/c/Users/fzt/.agents/skills/decision-ledger/scripts/validate_traceability.py --ledger .scratch/astra-scheduler-runtime/decision-log.md --spec .scratch/astra-scheduler-runtime/spec.md --tickets-dir .scratch/astra-scheduler-runtime/issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=会话开始快照，无 commit）：已处理必须项——tag 交付顺序断言改为真实顺序校验、新增每 tag 矩阵行 scope/evidence 非空校验、`section_body`/`text_before` 统一 markdown 解析、DoD 关键词限定在 DoD 节内、milestones.md 权威句对齐 spec `Further Notes`。修复后重新验证通过。
- 记录边界：R-004“误写为单版本范围”的语义级审计无法无假阳性自动化，由锚点测试 + spec 修订 review 把关；“每 tag 可独立构建运行”在本 Ticket 固化为 DoD 门禁，实际构建证据由各 tag release 时兑现（AST-055 汇总）。
- 2026-08-27因D-167/D-168与R-111/R-112重新打开本Ticket；保留原R-004/R-005/R-094证据并追加WSL-only开发门禁。
- 2026-08-27批准修订后，从WSL执行追踪校验与15项release gate均通过；AST-001重新完成并解锁AST-002。
- 2026-08-27（Linux/WSL符合性审计）：确认全部交付物与R-111/R-112一致——CI仅使用Linux runner（`runs-on: ubuntu-latest`）、AGENTS.md与docs/development.md固定WSL命令入口与canonical path、milestones.md含Local WSL development gate；对齐`tools/check_release_gates.py` docstring运行命令为WSL形式，并从WSL复核`Ran 15 tests ... OK`与`Traceability valid`。
- 2026-08-27（Python版本对齐）：WSL仅提供python3.8（3.8.10）；CI setup-python从3.12改为与本机WSL一致的3.8，脚本docstring标注兼容Python 3.8+；WSL复验`py_compile`与`Ran 15 tests ... OK`。
