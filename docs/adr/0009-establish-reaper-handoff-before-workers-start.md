---
status: accepted
date: 2026-08-25
decisions: [D-019]
---

# Establish Reaper handoff before workers start

Runtime 必须在任何 Worker 启动前建立并预留 Reaper handoff 能力，准备失败则不进入 `Running` 且不留下活动 Worker。这样 Worker 析构中的所有权移交可以保持 `noexcept`、不分配内存且不创建线程，把可能失败的资源获取放在仍有正常错误通道的启动阶段，而不是在无法恢复的析构边界。

决策细节见 [D-019](../../.scratch/astra-scheduler-runtime/decision-log.md)。
