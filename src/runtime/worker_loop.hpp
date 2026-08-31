#ifndef ASTRA_SRC_RUNTIME_WORKER_LOOP_HPP
#define ASTRA_SRC_RUNTIME_WORKER_LOOP_HPP

#include <cstddef>

namespace astra::detail {

struct RuntimeState;

// 编译期稳定的内部入口；worker 循环只依赖 RuntimeState，不再 duck-type
// Scheduler::Impl，也不把完整算法实例化进 scheduler.cpp（R-130）。
void run_worker_loop(
    RuntimeState& runtime,
    void* owner_impl,
    std::size_t worker_index);

}  // namespace astra::detail

#endif  // ASTRA_SRC_RUNTIME_WORKER_LOOP_HPP
