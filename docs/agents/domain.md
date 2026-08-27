# AstraScheduler Domain Context

## Layout

AstraScheduler 使用 single-context domain layout：整个仓库共享一份领域术语表和一组 ADR，不按里程碑或子系统拆分独立 bounded-context 文档。

- Domain glossary：`CONTEXT.md`
- ADR directory：`docs/adr/`
- Decision Ledger：`.scratch/astra-scheduler-runtime/decision-log.md`
- Approved Spec：`.scratch/astra-scheduler-runtime/spec.md`
- Overall design reference：`AstraScheduler_项目整体设计.md`

## Consumer rules

- 开始需求澄清、设计评审、Spec/Ticket 维护或实现前，先读取与任务相关的领域术语和 ADR。
- 使用 `CONTEXT.md` 中已经确定的术语；不要为同一概念静默引入近义名称。
- `CONTEXT.md` 或相关 ADR 缺失时，可以继续做不依赖该信息的工作，但不得虚构领域规则。
- 若请求、Ticket、代码或旧设计参考与 accepted ADR、Decision Ledger 或 approved Spec 冲突，必须明确指出冲突并先走决策与规格修订流程。
- `AstraScheduler_项目整体设计.md` 是参考材料；发生冲突时，以 accepted Decision Ledger 和 approved Spec 为规范性来源。
- 只读取与当前任务有关的 ADR，避免把全部历史文档无差别载入上下文。

## Platform invariant

开发与最终 Supported Configuration 均仅限 64-bit Linux。本机开发和验证通过 WSL Linux 执行；详细约束见 `AGENTS.md`、`docs/development.md` 与 `docs/adr/0047-linux-only-support-and-wsl-development.md`。
