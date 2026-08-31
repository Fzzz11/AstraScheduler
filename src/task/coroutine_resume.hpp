#ifndef ASTRA_COROUTINE_RESUME_HPP
#define ASTRA_COROUTINE_RESUME_HPP

#include "task_control_block.hpp"
#include "ready_linked_invoker.hpp"

#include <astra/coroutine.hpp>

#include <coroutine>
#include <memory>

namespace astra::detail {

class CoroutineResumeInvoker final : public ReadyLinkedInvoker {
public:
    std::coroutine_handle<> coro;
    std::shared_ptr<TaskControlBlock> tcb;

    CoroutineResumeInvoker(std::coroutine_handle<> h, std::shared_ptr<TaskControlBlock> s);
    ~CoroutineResumeInvoker() override = default;

    void execute() override;
    void cancel_pre_start() noexcept override {}
    [[nodiscard]] bool is_resume_segment() const noexcept override { return true; }
    [[nodiscard]] Priority priority() const noexcept override;
};

template <typename T>
class CoroutineResumeInvokerModel final : public TaskInvokerBase {
public:
    std::unique_ptr<CoroutineResumeInvoker> inner;

    template <typename Cell>
    CoroutineResumeInvokerModel(std::coroutine_handle<TaskPromise<T>> h, std::shared_ptr<Cell> s)
        : inner(std::make_unique<CoroutineResumeInvoker>(
              std::coroutine_handle<>::from_address(h.address()),
              s ? s->protocol_ : nullptr)) {}

    void execute() override {
        if (inner) {
            inner->execute();
        }
    }
    void cancel_pre_start() noexcept override {
        if (inner) {
            inner->cancel_pre_start();
        }
    }
    [[nodiscard]] bool is_resume_segment() const noexcept override { return true; }
    [[nodiscard]] Priority priority() const noexcept override {
        return inner ? inner->priority() : Priority::Normal;
    }
};

}  // namespace astra::detail

#endif  // ASTRA_COROUTINE_RESUME_HPP
