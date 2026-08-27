# AstraScheduler Issue Tracker

## Backend

AstraScheduler 使用仓库内的本地 Markdown 文件管理工作项。GitHub remote 只表示代码托管位置，不把 GitHub Issues 变成默认 Ticket backend。

## Locations

- Ticket 目录：`.scratch/astra-scheduler-runtime/issues/`
- Ticket Plan：`.scratch/astra-scheduler-runtime/ticket-plan.md`
- Approved Spec：`.scratch/astra-scheduler-runtime/spec.md`
- Decision Ledger：`.scratch/astra-scheduler-runtime/decision-log.md`

## Workflow rules

- 新 Ticket 必须由已批准的 Spec 生成，并引用其 Primary/Supporting Rules 与来源决策。
- Ticket 文件名遵循现有的 `<NN>-<slug>.md` 形式；稳定 Ticket ID 使用 `AST-<NNN>`。
- `Blocked by`、`Status` 和 `Claimed by` 字段是本地执行状态的权威记录。
- 只有无 blocker、规则可追踪且状态为 `ready-for-agent` 的 Ticket 才能进入实现流程。
- 实现完成后，在 Ticket 内保存 acceptance、测试和 verification evidence；不得只在聊天中声明完成。
- 不因仓库存在 GitHub remote 而自动创建、迁移或同步 GitHub Issues。改变 backend 前必须重新运行项目初始化并获得 owner 确认。
