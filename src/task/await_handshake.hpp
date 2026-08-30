#ifndef ASTRA_AWAIT_HANDSHAKE_HPP
#define ASTRA_AWAIT_HANDSHAKE_HPP

#include <astra/task_handle.hpp>

#include <atomic>
#include <cstdint>

namespace astra::detail {

class AwaitHandshake {
public:
    enum class State : std::uint8_t {
        Init = 0,
        Triggered = 1,
        Armed = 2,
        Resolved = 3,
    };

    static constexpr std::uint8_t kStateMask = 0x0F;
    static constexpr std::uint8_t kCancelled = 0x80;

    AwaitHandshake() noexcept = default;

    template <typename PostAction>
    void trigger(PostAction&& post_action) {
        std::uint8_t expected = static_cast<std::uint8_t>(State::Init);
        if (raw_state_.compare_exchange_strong(expected, static_cast<std::uint8_t>(State::Triggered),
                                                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        if ((expected & kStateMask) == static_cast<std::uint8_t>(State::Armed)) {
            std::uint8_t new_state = static_cast<std::uint8_t>(State::Resolved) | (expected & kCancelled);
            if (raw_state_.compare_exchange_strong(expected, new_state,
                                                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                post_action();
            }
        }
    }

    template <typename PostAction>
    void trigger_cancel(PostAction&& post_action) {
        std::uint8_t current = raw_state_.load(std::memory_order_acquire);
        while (true) {
            if ((current & kStateMask) == static_cast<std::uint8_t>(State::Resolved)) {
                return;
            }
            if ((current & kStateMask) == static_cast<std::uint8_t>(State::Triggered)) {
                return;
            }
            if ((current & kStateMask) == static_cast<std::uint8_t>(State::Init)) {
                std::uint8_t next = static_cast<std::uint8_t>(State::Triggered) | kCancelled;
                if (raw_state_.compare_exchange_weak(current, next,
                                                      std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            if ((current & kStateMask) == static_cast<std::uint8_t>(State::Armed)) {
                std::uint8_t next = static_cast<std::uint8_t>(State::Resolved) | kCancelled;
                if (raw_state_.compare_exchange_weak(current, next,
                                                      std::memory_order_acq_rel, std::memory_order_acquire)) {
                    post_action();
                    return;
                }
                continue;
            }
            break;
        }
    }

    template <typename PostAction>
    void arm(PostAction&& post_action) {
        std::uint8_t expected = static_cast<std::uint8_t>(State::Init);
        if (raw_state_.compare_exchange_strong(expected, static_cast<std::uint8_t>(State::Armed),
                                                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        if ((expected & kStateMask) == static_cast<std::uint8_t>(State::Triggered)) {
            std::uint8_t new_state = static_cast<std::uint8_t>(State::Resolved) | (expected & kCancelled);
            if (raw_state_.compare_exchange_strong(expected, new_state,
                                                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                post_action();
            }
        }
    }

    [[nodiscard]] bool is_resolved() const noexcept {
        return (raw_state_.load(std::memory_order_acquire) & kStateMask) == static_cast<std::uint8_t>(State::Resolved);
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return (raw_state_.load(std::memory_order_acquire) & kCancelled) != 0;
    }

    [[nodiscard]] State state() const noexcept {
        return static_cast<State>(raw_state_.load(std::memory_order_acquire) & kStateMask);
    }

private:
    std::atomic<std::uint8_t> raw_state_{static_cast<std::uint8_t>(State::Init)};
};

}  // namespace astra::detail

#endif  // ASTRA_AWAIT_HANDSHAKE_HPP
