#include "await_handshake.hpp"
#include "coroutine_resume.hpp"
#include "task_control_block.hpp"

#include <astra/coroutine.hpp>
#include <astra/scheduler.hpp>

#include <chrono>
#include <utility>

namespace astra::detail {

struct AwaitProtocolAccess {
    static void add_completion(GraphRun& run, std::function<void()> cb) {
        run.add_completion_callback_internal(std::move(cb));
    }
};

namespace {

struct AwaitToken {
    std::shared_ptr<AwaitHandshake> handshake;
    std::optional<std::stop_callback<std::function<void()>>> stop_cb;
    TaskId source_id{};
    TaskId target_id{};
    std::chrono::steady_clock::time_point armed_at{};
};

std::shared_ptr<TaskControlBlock> as_tcb(const std::shared_ptr<void>& token) {
    return std::static_pointer_cast<TaskControlBlock>(token);
}

std::function<void()> make_resume_action(
    std::shared_ptr<TaskControlBlock> waiter,
    std::coroutine_handle<> coro,
    TaskRescheduler rescheduler) {
    return [waiter = std::move(waiter), coro, rescheduler = std::move(rescheduler)]() mutable {
        if (!waiter || !rescheduler) {
            return;
        }
        waiter->set_ready_published_at(std::chrono::steady_clock::now());
        rescheduler(std::make_unique<CoroutineResumeInvoker>(coro, waiter));
    };
}

}  // namespace

CoroutineResumeInvoker::CoroutineResumeInvoker(
    std::coroutine_handle<> h,
    std::shared_ptr<TaskControlBlock> s)
    : coro(h), tcb(std::move(s)) {}

void CoroutineResumeInvoker::execute() {
    if (!tcb) {
        return;
    }
    tcb->transition_to_running();
    const auto now = std::chrono::steady_clock::now();
    const auto pub = tcb->ready_published_at();
    if (now >= pub && pub != std::chrono::steady_clock::time_point{}) {
        record_metrics_ready_queue_wait(
            tcb->id(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - pub).count());
    }
    record_metrics_resumed(tcb->id());
    record_metrics_resume_segment(tcb->id());
    const std::uint64_t handoff_seq_before = tcb->resume_handoff_seq();
    TaskExecutionContextGuard guard(tcb->id(), tcb->priority());
    const auto t_start = std::chrono::steady_clock::now();
    try {
        if (coro && !coro.done()) {
            coro.resume();
        }
        const auto t_end = std::chrono::steady_clock::now();
        record_metrics_execution_segment(
            tcb->id(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
    } catch (const task_cancelled&) {
        const auto t_end = std::chrono::steady_clock::now();
        record_metrics_execution_segment(
            tcb->id(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
        tcb->set_cancelled();
    } catch (...) {
        const auto t_end = std::chrono::steady_clock::now();
        record_metrics_execution_segment(
            tcb->id(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
        tcb->set_exception(std::current_exception());
    }

    if (tcb->resume_handoff_seq() != handoff_seq_before) {
        coro = nullptr;
    } else if (coro && coro.done()) {
        coro.destroy();
        coro = nullptr;
    }
}

Priority CoroutineResumeInvoker::priority() const noexcept {
    return tcb ? tcb->priority() : Priority::Normal;
}

std::shared_ptr<void> tcb_arm_task_await(
    std::shared_ptr<void> waiter_token,
    std::shared_ptr<void> target_token,
    std::coroutine_handle<> coro) {
    auto waiter = as_tcb(waiter_token);
    auto target = as_tcb(target_token);
    if (!waiter || !target) {
        throw std::logic_error("invalid coroutine shared_state");
    }

    auto token = std::make_shared<AwaitToken>();
    token->handshake = std::make_shared<AwaitHandshake>();
    token->source_id = waiter->id();
    token->target_id = target->id();
    token->armed_at = std::chrono::steady_clock::now();

    auto post_action = make_resume_action(waiter, coro, waiter->get_rescheduler());
    waiter->mark_resume_handoff();
    auto hs = token->handshake;
    const auto src = token->source_id;
    const auto tgt = token->target_id;
    target->add_completion_callback([hs, post_action, src, tgt]() mutable {
        record_await_triggered(src, tgt, false);
        hs->trigger(post_action);
    });
    token->stop_cb.emplace(waiter->stop_token(), [hs, post_action, src, tgt]() mutable {
        record_await_triggered(src, tgt, true);
        hs->trigger_cancel(post_action);
    });
    waiter->transition_to_suspended();
    hs->arm(std::move(post_action));
    record_await_registration(src, tgt);
    return token;
}

std::shared_ptr<void> tcb_arm_graph_await(
    std::shared_ptr<void> waiter_token,
    GraphRun& run,
    std::coroutine_handle<> coro) {
    auto waiter = as_tcb(waiter_token);
    if (!waiter) {
        throw std::logic_error("invalid coroutine shared_state");
    }

    auto token = std::make_shared<AwaitToken>();
    token->handshake = std::make_shared<AwaitHandshake>();
    token->source_id = waiter->id();
    token->armed_at = std::chrono::steady_clock::now();

    auto post_action = make_resume_action(waiter, coro, waiter->get_rescheduler());
    waiter->mark_resume_handoff();
    auto hs = token->handshake;
    AwaitProtocolAccess::add_completion(run, [hs, post_action]() mutable {
        hs->trigger(post_action);
    });
    token->stop_cb.emplace(waiter->stop_token(), [hs, post_action]() mutable {
        hs->trigger_cancel(post_action);
    });
    waiter->transition_to_suspended();
    hs->arm(std::move(post_action));
    return token;
}

void tcb_arm_yield(std::shared_ptr<void> waiter_token, std::coroutine_handle<> coro) {
    auto waiter = as_tcb(waiter_token);
    if (!waiter) {
        throw std::logic_error("invalid coroutine shared_state");
    }
    waiter->transition_to_suspended();
    record_metrics_explicit_yield();
    auto rescheduler = waiter->get_rescheduler();
    if (rescheduler) {
        waiter->mark_resume_handoff();
        waiter->set_ready_published_at(std::chrono::steady_clock::now());
        rescheduler(std::make_unique<CoroutineResumeInvoker>(coro, std::move(waiter)));
    }
}

std::shared_ptr<void> tcb_arm_sleep(
    std::shared_ptr<void> waiter_token,
    std::chrono::steady_clock::time_point wake_time,
    std::coroutine_handle<> coro) {
    auto waiter = as_tcb(waiter_token);
    if (!waiter) {
        throw std::logic_error("invalid coroutine shared_state");
    }

    auto rescheduler = waiter->get_rescheduler();
    auto registrar = waiter->get_timer_registrar();
    auto canceller = waiter->get_timer_canceller();
    if (!rescheduler || !registrar || !canceller) {
        throw std::logic_error("cannot sleep outside an AstraScheduler coroutine runtime");
    }

    waiter->transition_to_suspended();

    auto token = std::make_shared<AwaitToken>();
    token->handshake = std::make_shared<AwaitHandshake>();
    token->source_id = waiter->id();
    token->armed_at = std::chrono::steady_clock::now();

    struct RegistrationContext {
        std::atomic<std::uint64_t> timer_id{0};
        std::atomic<bool> cancelled{false};
    };
    auto ctx = std::make_shared<RegistrationContext>();
    auto post_action = make_resume_action(waiter, coro, rescheduler);

    token->stop_cb.emplace(
        waiter->stop_token(),
        [handshake = token->handshake, canceller, ctx, post_action]() mutable {
            ctx->cancelled.store(true, std::memory_order_release);
            const std::uint64_t tid = ctx->timer_id.load(std::memory_order_acquire);
            if (tid != 0 && canceller) {
                canceller(tid);
            }
            handshake->trigger_cancel(post_action);
        });

    if (waiter->stop_token().stop_requested()) {
        token->stop_cb.reset();
        throw task_cancelled{};
    }

    waiter->mark_resume_handoff();
    const std::uint64_t tid = registrar(wake_time, token->handshake, post_action);
    ctx->timer_id.store(tid, std::memory_order_release);
    if (ctx->cancelled.load(std::memory_order_acquire) || waiter->stop_token().stop_requested()) {
        canceller(tid);
    }
    token->handshake->arm(post_action);
    return token;
}

void tcb_finish_await(std::shared_ptr<void>& token, TaskId target) {
    auto* await = static_cast<AwaitToken*>(token.get());
    if (!await) {
        return;
    }
    await->stop_cb.reset();
    if (await->source_id.valid()) {
        const auto dur_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - await->armed_at).count());
        record_await_resumed(await->source_id, target, dur_ns);
    }
}

bool tcb_await_cancelled(const std::shared_ptr<void>& token) noexcept {
    auto* await = static_cast<AwaitToken*>(token.get());
    return await && await->handshake && await->handshake->is_cancelled();
}

}  // namespace astra::detail
