---
status: accepted
date: 2026-08-25
decisions: [D-038]
---

# Finalization is explicit process teardown

Reaper Finalization 是不可逆且可能无界的进程控制操作，只能由应用显式调用；AstraScheduler 不从 `atexit`、静态析构、空闲超时或最后一个 Scheduler Handle 析构自动执行 begin、wait 或 Immediate escalation。这样应用可以在任务依赖资源仍有效、日志和退出策略仍可用时决定继续等待、升级或终止进程，而不会遭遇 static destruction order、隐式永久阻塞或进程级准入被意外关闭。

动态库卸载必须先由非 Worker 观察真实 `Completed`，并保证不再有会调用库代码的公共对象；`TimedOut` 绝不是安全卸载信号。不可逆行为的测试使用独立子进程，不提供破坏真实 one-shot 语义的公共 reset/restart。

决策细节见 [D-038](../../.scratch/astra-scheduler-runtime/decision-log.md)。
