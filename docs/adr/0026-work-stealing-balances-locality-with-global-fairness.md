---
status: accepted
date: 2026-08-26
decisions: [D-090, D-091, D-092, D-093, D-147]
---

# Work stealing balances locality with global fairness

v0.1.0 保留 all-global FIFO 基线；后续 Work-Stealing 版本把 same-Runtime Worker publication 默认路由到 owner Local Deque，把 External/off-Worker publication 路由到 Global Injection Queue。专用规则优先：首次Deadline work进入Global EDF，yield和timer resume强制ordinary Global；普通DAG/await completion才按publisher context回落。Local owner bottom LIFO 与 thief top oldest 语义在带锁和 Chase-Lev 两阶段保持一致，外部线程永远不写 owner-only deque。

为防持续 Internal Submission 饿死 Global work，每个 Worker 默认最多连续执行 64 个 local Task 后强制 Global probe；Global 为空才开始新 burst。只有 local/global 都未取得任务才进入 steal round，每轮默认最多探测 8 个 Worker-private pseudo-random、互不重复 victim，成功即停，失败转入 idle/backoff。调优数值可配置且必须为正。

决策细节见 [D-090 至 D-093 与 D-147](../../.scratch/astra-scheduler-runtime/decision-log.md)。
