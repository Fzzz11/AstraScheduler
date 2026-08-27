---
status: accepted
date: 2026-08-26
decisions: [D-094, D-095, D-096]
---

# Work publication and parking use an epoch handshake

Idle Worker 在有界 pause/yield backoff 后进入可通知 park，具体循环次数保持内部可调。所有 Ready、mode 和 exit publication 统一遵循 publish-before-epoch-before-notify；Worker 登记 park intent 后二次检查来源、Shutdown 与 epoch，只有仍无工作且 generation 未变才阻塞。固定宽度 generation 饱和时禁用 park或进入无 ABA slow path，不能靠周期 polling 掩盖丢唤醒。

单个 Ready work 至少通知一个 parked Worker，batch 按可并行度逐步唤醒；owner-local work 也通知以允许 steal。Shutdown/mode/exit 变化 notify-all。Fanout 只影响利用率，epoch/predicate 才是正确性边界。

决策细节见 [D-094 至 D-096](../../.scratch/astra-scheduler-runtime/decision-log.md)。
