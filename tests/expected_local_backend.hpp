#ifndef ASTRA_TESTS_EXPECTED_LOCAL_BACKEND_HPP
#define ASTRA_TESTS_EXPECTED_LOCAL_BACKEND_HPP

#include <astra/capabilities.hpp>

#include <atomic>
#include <cstdint>

// 与生产 ChaseLevDeque<ReadyLinkedInvoker*>::is_lock_free() 对齐的测试期望值。
// 探针包含 index、cell 指针、cell 元素，以及 maintenance / active-thief guard。
constexpr bool kExpectedLocalDequeLockFree =
    std::atomic<std::uint64_t>::is_always_lock_free &&
    std::atomic<void*>::is_always_lock_free &&
    std::atomic<bool>::is_always_lock_free &&
    std::atomic<std::uint32_t>::is_always_lock_free;
constexpr astra::LocalDequeBackend kExpectedLocalDequeBackend =
    kExpectedLocalDequeLockFree
        ? astra::LocalDequeBackend::ChaseLevLockFree
        : astra::LocalDequeBackend::Locked;

#endif  // ASTRA_TESTS_EXPECTED_LOCAL_BACKEND_HPP
