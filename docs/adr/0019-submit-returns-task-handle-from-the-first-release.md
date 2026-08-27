---
status: accepted
date: 2026-08-26
decisions: [D-041, D-042, D-043, D-044, D-061, D-063, D-064, D-067, D-068, D-072, D-073, D-074, D-075, D-076, D-077, D-153, D-165]
---

# submit returns a shared TaskHandle from the first release

从 v0.1.0 起，`submit()` 的稳定公共返回类型使用 AstraScheduler 自定义的 `TaskHandle<T>`，而不先暴露 `std::future<T>` 再在后续版本替换。内部早期实现仍可使用 `std::promise`、`std::packaged_task` 或其他标准设施，但这些机制不能成为公共返回契约；这样 Cancellation、TaskState、DAG 与 Coroutine 可以继续扩展同一个任务能力对象，而不用破坏首版 API 或长期维护两套结果抽象。

`TaskHandle<T>` 可复制、可移动，所有副本表示同一个 Task Identity 和完成状态；复制 Handle 不会复制或重新提交任务。该选择把自定义任务共享状态及其正确性验证提前到 v0.1.0。结果消费、并发调用、最后 Handle 销毁、等待、取消与 submission rejection 语义由后续独立决策固定。

最后一个 Task Handle 的销毁只放弃前台观察与控制能力，不隐式取消已经接受的任务；Runtime 继续持有执行责任直到任务进入终态。这避免 Handle 引用计数意外成为取消策略，并允许 fire-and-forget 与内部 Runtime 工作在没有外部 Handle 时继续推进。

任务终结时发布一个不可变的 Terminal Outcome，所有有效 Handle 副本都能重复观察同一个 Value、Exception 或 Cancelled 事实，观察不会消费或改写共享完成状态。move-only value 的具体访问形式继续作为独立 Interface 决策处理。

成功 Value 通过左值限定的 `TaskHandle<T>::get() const &` 返回共享 `const T&`，`void` 特化也只允许左值调用；rvalue/临时 Handle 的 `get()` 在编译期拒绝，避免完整表达式结束后引用悬垂。基础 Interface 不提供会从共享 Outcome 移走值的 `take()` 或重复所有权抽象的 Result View，调用方通过保留/复制 Handle 持有结果生命周期。

`void wait() const` 只同步到真实 Terminal Outcome，不传播 Value、Exception 或 Cancelled；结果处理继续由 `get()` 承担。该分离让监督与组合代码可以等待完成而不触发业务异常。

`wait_for(duration)` 以 `TaskWaitResult::Completed/TimedOut` 提供同样不传播 Outcome 的有界观察；timeout 不取消 Task。它使用 `steady_clock`，非正 duration 执行即时观察，并对 Terminal Outcome publication 与 deadline 建立唯一线性化顺序。

TaskHandle 具有与 Task 生命周期分离的显式空状态：默认构造为空，move 转移关联并使源为空，`valid()` 无异常查询关联。空 Handle 的结果、等待、状态和身份操作抛 `std::logic_error`；已经固定为 `noexcept` 的 `request_cancel()` 对空 Handle 安全 no-op。这保留常规 C++ Handle 值语义，同时不把 `Invalid` 混入 TaskState。

共享状态操作支持同一稳定 Handle 或不同副本上的并发调用；只有同一 Handle 对象的 move/赋值/swap/析构与访问需要调用方同步。所有同步等待者共享单次 completion publication，注册竞态不会丢失完成，但等待者顺序、公平性和调度延迟不成为公共保证。

Callable 的裸引用结果在编译期拒绝；需要显式引用语义时返回 `reference_wrapper`，需要 ownership 时返回 owning value。结果支持 void、copyable 与 move-only object，并去除顶层 cv；首个稳定 API 暂不承诺完全 immovable object result。

`submit/try_submit`从v0.1起对Callable与参数执行decay-owned capture，并以stored rvalue恰好调用一次；支持move-only target/argument和`operator()&&`，真实引用必须显式`std::ref`。内部queue payload不得因copy-only `std::function`缩窄这项public能力。

稳定 API 不再并行提供 `try_get()`、`exception()`、Outcome/Result View：`state()`、等待、左值 `get()` 与复制 Handle 分别覆盖类别、同步、传播和 ownership。

公共`RuntimeId`、`TaskId(RuntimeId, sequence)`与`GraphRunId`是trivially-copyable强值类型。有效Handle/GraphRun和Scheduler暴露同一logical identity供日志、Trace和wait edge关联；ID不来自地址、不授予lookup/control能力，0保留invalid且checked sequence永不wrap复用。

决策细节见 [D-041 至 D-044、D-061、D-063、D-064、D-067、D-068、D-072 至 D-077、D-153 与 D-165](../../.scratch/astra-scheduler-runtime/decision-log.md)。
