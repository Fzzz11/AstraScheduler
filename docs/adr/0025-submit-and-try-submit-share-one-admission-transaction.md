---
status: accepted
date: 2026-08-26
decisions: [D-087, D-088, D-089]
---

# submit and try_submit share one admission transaction

最终Runtime admission rejection使用`SubmissionError::{Stopping, Stopped, CapacityExhausted}`；primary `submit()`抛携带reason的`submission_rejected`，C++20 `try_submit()`即时返回`variant<TaskHandle<T>, SubmissionError>`，不因Block policy等待。D-087早期的`NotRunning`由D-155单阶段Scheduler构造消除：有效Scheduler曾成功Running，空/moved-from Handle是`logic_error`，Finalization期间创建失败使用独立`scheduler_creation_rejected`。Allocation、Callable/参数capture与配置异常保持原类型。

两种 API 共享同一个强异常安全 transaction：只有 gate、slot、Task/Callable/stop/completion 构造、Identity、outstanding-work 与 Waiting/Ready publication 全部可靠建立后才算成功。之前任何失败都回滚 Runtime 预留且不执行 Callable；成功 Task 可以由正常 Worker 在方法返回 Handle 前开始或终结，但不会成为 CallerRuns。

决策细节见 [D-087 至 D-089](../../.scratch/astra-scheduler-runtime/decision-log.md)。
