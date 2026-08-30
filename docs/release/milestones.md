# AstraScheduler Release Milestones and Rule Gates

Status: active
Source rules: R-094 (D-146), R-004 (D-004), R-005 (D-005), R-112 (D-168)
Source ticket: AST-001 (`.scratch/astra-scheduler-runtime/issues/01-release-rule-gates.md`)

本文档固定 AstraScheduler Phase 0 至 v1.2.0 的里程碑交付矩阵、每个 tag 的统一
Definition of Done 与 release gates，以及规则追踪入口。它是 R-094 的仓库级权威
入口；`AstraScheduler_项目整体设计.md` 是架构参考，冲突处以 accepted decisions
和对应 R-rule 为准（与 spec `Further Notes` 一致）。

## Milestone delivery matrix

每个 tag 是一个纵向可运行 increment，按下列顺序交付：

| Tag | Scope | Release evidence（每 tag DoD 摘要） |
|---|---|---|
| Phase 0 | Engineering scaffold（untagged） | WSL开发指令、编译库骨架、安装导出 target、独立 Linux consumer、版本查询、测试入口可运行。 |
| v0.1.0 | Global-only Runtime、TaskHandle/Result、取消、完整 Scheduler/Reaper/Finalization 生命周期 | Global-only Ready 路径证据（Local Deque capability 为 `None`）、admission/backpressure 与完整生命周期证据。 |
| v0.2.0 | Locked Local Deque、bounded steal round、Park Handshake | v0.1 baseline 仍可运行；routing precedence 与 fairness 证据。 |
| v0.3.0 | Chase-Lev portable ordering、resize retention、index 边界 | 仅实际 capability 报告 lock-free；oracle differential 与 stress 证据。 |
| v0.4.0 | 单次 Frozen Graph、原子 admission、edge policy、GraphRun 完整报告 | graph 语义与 failure/cancel 传播证据。 |
| v0.5.0 | Cold Task、runtime-owned resume、取消握手、Graph coroutine node、Worker timer | coroutine/timer 语义与 resume ownership 证据。 |
| v0.6.0 | 固定 Priority、8:4:2:1 band service、best-effort first-start Deadline、Global indexed EDF | priority/deadline 语义与长期服务比例证据。 |
| v0.7.0 | Runtime/Process Metrics、bounded Trace、versioned events、Chrome Trace 离线导出与 wait/await 诊断 | observability schema fixture 与确定性导出证据。 |
| v0.8.0 | Micro harness、scenario runner、语义基线、原始 artifact 与受限 regression gate | benchmark validity、checksum 与 artifact 可重算证据。 |
| v0.9.0 | Linux-only Tier matrix、sanitizer/weak-memory/package consumer、单实现实例部署约束 harden | Linux x86_64 GCC/Clang、native Linux AArch64、sanitizer 与 package consumer 证据；不存在Windows/MSVC release job。 |
| v1.0.0 | Public source/semantic compatibility 冻结 | 全部 approved-rule、文档、package、schema、benchmark gates 通过。 |
| v1.1.0 | 封装边界收紧：semantic API manifest、测试入口私有化、TaskId/admission 所有权归 Runtime、GraphExecution 边界 | semantic API gate（documented surface + consumer probes）、graph/coroutine/admission 并发证据、public tests 无 src/ include。 |
| v1.2.0 | compiled TaskControlBlock：协议类型离开 installed headers；薄 awaiter、F 信封、private nested 结果格；shared 只导出 documented allowlist | 协议类型完成型 probes、VERSION 1.2.0、v1.0/v1.1 manifest 不可改写、shared dynsym allowlist。 |

## Definition of Done（每个 tag 的统一 DoD）

每个 release tag 必须满足以下统一 DoD，缺一不可；release tag 不得以“测试未来补”
绕过 Definition of Done：

1. 其覆盖的 approved spec rules 均有 Ticket 与测试证据（approved-rule 测试）。
2. Tier-1 Linux x86_64 GCC/Clang Release build 与 unit/integration 测试通过。
3. 涉及并发的变更通过对应 stress 测试及可用 sanitizer（并发证据）。
4. public behavior、docs、examples 与 CMake package 同步（docs/package gates）。
5. Metrics/Trace schema fixture 更新（schema gate）。
6. 相关 benchmark 至少 build/smoke，且性能 claim 附带 artifact（benchmark gate）。
7. 每个 tag 可独立构建、运行：tag 签出后不依赖未交付 feature 即可构建、运行并
   通过其测试。

例外边界：private seam 可提前建立，未批准的 public 语义不得提前暴露。

### Local WSL development gate

R-112要求所有本机开发与验证命令从WSL Linux执行，canonical workspace为
`/mnt/d/code/cppStudy/AstraScheduler`。Windows宿主shell只能启动WSL或执行非开发
编排；不得直接运行Windows-native toolchain、测试程序或benchmark形成项目证据，
也不得在Windows native与WSL之间复用build cache或产物。

本机运行仓库门禁：

```powershell
wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"
```

CI与release在native Linux runner执行；WSL只是本机开发入口，不是最终运行时依赖。

## 规则追踪入口

- Spec（approved 规则的唯一权威来源）：`.scratch/astra-scheduler-runtime/spec.md`
- Decision ledger：`.scratch/astra-scheduler-runtime/decision-log.md`
- Tickets（每票记录目标版本）：`.scratch/astra-scheduler-runtime/issues/`
- Ticket plan（拆票原则与依赖 DAG）：`.scratch/astra-scheduler-runtime/ticket-plan.md`
- ADRs：`docs/adr/`

## Approved Spec 门禁

只有 `Status: approved` 的 spec 才能拆票（to-tickets）与实现：

- 实现工作必须按 R-005 拆分为带目标版本的多个 Tickets，不得把整个
  AstraScheduler 合并为单一实现 Ticket；文档维护或不产生实现的管理工作不属于
  实现 Ticket。
- 规格按 R-004 覆盖整个跨版本 Runtime，每条规则必须声明 `Applies to` 范围，仅
  适用于某版本的规则显式标出版本范围。
- CI 入口（`.github/workflows/ci.yml`）运行 `tools/check_release_gates.py`，
  对 spec 状态、每条规则的 `Applies to`、每票目标版本、里程碑矩阵与每 tag DoD
  进行校验；任一校验失败即阻止合入。

本地从WSL运行同一校验：

```powershell
wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"
```
