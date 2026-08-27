---
status: accepted
date: 2026-08-25
decisions: [D-030, D-031, D-032, D-033, D-034, D-035, D-036, D-037, D-039]
---

# Finalization uses an explicit shared control object

`begin_finalization()` 返回不可默认构造、可复制且析构无副作用的 `FinalizationControl`，并只通过它提供 `wait()`、`wait_for()` 与 `request_immediate()`。相比一组可任意排序的全局函数，这个 Interface 在类型层面表达 begin-before-wait，并让多个非 Worker 观察者共享唯一 Finalization Completion；重复或并发 begin 幂等返回同一终结世代，Finalized 后也不会重启 Reaper。

begin 与显式升级是可从任意应用线程调用的请求式操作，不能等待 Runtime 或 join；`wait()`/`wait_for()` 从任意 Scheduler Worker 调用则在副作用前抛出 `std::logic_error`。`wait_for()` 使用 steady clock，非正 duration 是即时观察，Completion 与 deadline 在统一同步域内线性化。coordinator 退出后由恰好一个合法等待者完成唯一 join，再发布 Finalized；控制对象析构不会偷偷承担这个可能无界的工作。

公共 C++20 Interface 固定为：

```cpp
namespace astra {

enum class FinalizationWaitResult {
    Completed,
    TimedOut,
};

class FinalizationControl {
public:
    FinalizationControl(const FinalizationControl&) noexcept;
    FinalizationControl& operator=(const FinalizationControl&) noexcept;
    FinalizationControl(FinalizationControl&&) noexcept;
    FinalizationControl& operator=(FinalizationControl&&) noexcept;
    ~FinalizationControl() noexcept;

    void wait() const;

    template<class Rep, class Period>
    [[nodiscard]] FinalizationWaitResult
    wait_for(std::chrono::duration<Rep, Period> timeout) const;

    void request_immediate() const noexcept;

private:
    FinalizationControl(/* internal capability */) noexcept;
    friend FinalizationControl begin_finalization() noexcept;
};

[[nodiscard]] FinalizationControl begin_finalization() noexcept;

} // namespace astra
```

决策细节见 [D-030 至 D-037 与 D-039](../../.scratch/astra-scheduler-runtime/decision-log.md)。
