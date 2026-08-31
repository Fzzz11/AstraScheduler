#ifndef ASTRA_SRC_TASK_READY_LINKED_INVOKER_HPP
#define ASTRA_SRC_TASK_READY_LINKED_INVOKER_HPP

#include <astra/task_handle.hpp>

#include <memory>
#include <utility>

namespace astra::detail {

// src-only Scheduling Reference (D-100): Local cell 与 Global FIFO 只保存
// 该对象指针。链接字段不得出现在安装头的 TaskInvokerBase 上。
class ReadyLinkedInvoker : public TaskInvokerBase {
public:
    ReadyLinkedInvoker* ready_next{nullptr};
    bool ready_is_external{false};
};

// Header F envelope（GraphTaskInvokerModel 等）在 publication 边界包装为此
// 对象；它本身就是 Scheduling Reference，不是额外的并行 ChaseNode。
class ReadyLinkedAdapter final : public ReadyLinkedInvoker {
public:
    explicit ReadyLinkedAdapter(std::unique_ptr<TaskInvokerBase> inner)
        : inner_(std::move(inner)) {}

    void execute() override {
        if (inner_) {
            inner_->execute();
        }
    }

    void cancel_pre_start() noexcept override {
        if (inner_) {
            inner_->cancel_pre_start();
        }
    }

    void abandon_unstarted() noexcept override {
        if (inner_) {
            inner_->abandon_unstarted();
        }
    }

    [[nodiscard]] bool is_resume_segment() const noexcept override {
        return inner_ && inner_->is_resume_segment();
    }

    [[nodiscard]] bool is_coroutine_node() const noexcept override {
        return inner_ && inner_->is_coroutine_node();
    }

    [[nodiscard]] Priority priority() const noexcept override {
        return inner_ ? inner_->priority() : Priority::Normal;
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept override {
        return inner_ ? inner_->deadline() : std::nullopt;
    }

private:
    std::unique_ptr<TaskInvokerBase> inner_;
};

inline std::unique_ptr<ReadyLinkedInvoker> adopt_ready_linked(
    std::unique_ptr<TaskInvokerBase> task) {
    if (!task) {
        return nullptr;
    }
    if (dynamic_cast<ReadyLinkedInvoker*>(task.get()) != nullptr) {
        return std::unique_ptr<ReadyLinkedInvoker>(
            static_cast<ReadyLinkedInvoker*>(task.release()));
    }
    return std::make_unique<ReadyLinkedAdapter>(std::move(task));
}

}  // namespace astra::detail

#endif  // ASTRA_SRC_TASK_READY_LINKED_INVOKER_HPP
