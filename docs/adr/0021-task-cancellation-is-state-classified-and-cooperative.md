---
status: accepted
date: 2026-08-26
decisions: [D-052, D-053, D-054, D-055, D-056, D-057, D-058, D-059, D-060]
---

# Task cancellation is state-classified and cooperative

单个 Task Cancellation Request 与 Task start 在唯一线性化顺序中竞争：尚未进入 `Running` 的已接受 Task 在取消胜出时直接发布 `Cancelled` Terminal Outcome且不执行 Callable；已经 `Running` 时只发布 cooperative stop request，不强杀线程或伪造终态；已经终结时请求为幂等 no-op。该模型复用 Immediate Shutdown 的安全边界，并让同一种取消意图在调度竞态下只有一个明确结果。

公共操作固定为 `void TaskHandle<T>::request_cancel() const noexcept`：任意应用线程都能可靠发布幂等请求后立即返回，不等待 Terminal Outcome；不同 Handle 副本上的并发请求数据竞争安全。Running Task 协作退出后的终态、Suspended Coroutine 与 DAG 传播由后续独立决策固定。

stop request 本身不决定 Running Task 的终态：Callable 正常返回仍发布 Value，普通异常发布 Exception，只有显式 cooperative cancellation signal 才发布 Cancelled。Runtime 不在 Callable 返回后通过采样 `stop_requested()` 覆盖已经产生的成功结果。

Cancelled Outcome 由 `get()` 以公开 `astra::task_cancelled` 重复报告；同一类型若未被 Callable 捕获并逃出 Task execution boundary，则作为显式 Cancellation Signal 转换为 Cancelled，而不是普通 Exception。这样取消能沿同步 TaskHandle 等待自然传播，Callable 也可通过捕获信号选择恢复并正常返回。

`submit(F, Args...)` 对 stop-aware Callable 使用确定的编译期选择：优先普通 `F(Args...)`；仅当普通形式不可调用而 `F(std::stop_token, Args...)` 可调用时，才把 Task 自己的 token 注入首参数。这样保留普通 submit 形状，同时避免 generic Callable 意外收到隐藏参数。

`astra::throw_if_stop_requested(std::stop_token)` 提供显式安全点：一次观察到 stop request 就抛出 `task_cancelled`，否则返回。它不使用 TLS，也不把一次未观察到请求的检查升级为后续完成保证。

决策细节见 [D-052 至 D-060](../../.scratch/astra-scheduler-runtime/decision-log.md)。
