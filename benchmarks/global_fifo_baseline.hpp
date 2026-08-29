#ifndef ASTRA_BENCH_GLOBAL_FIFO_BASELINE_HPP
#define ASTRA_BENCH_GLOBAL_FIFO_BASELINE_HPP

// In-tree mutex-protected Global FIFO fixed-worker baseline（AST-050 / R-003 / D-142）。
// v0.1.0 Global Queue Scheduler 的语义基线保留：与 Chase-Lev variant 使用
// 相同 Task body、worker count、admission 与 shutdown 边界，仅作 Benchmark
// 对照组，不属于库的 public API 或安装接口。

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace astra::bench {

class GlobalFifoBaseline {
public:
    explicit GlobalFifoBaseline(std::size_t worker_count) {
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~GlobalFifoBaseline() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    GlobalFifoBaseline(const GlobalFifoBaseline&) = delete;
    GlobalFifoBaseline& operator=(const GlobalFifoBaseline&) = delete;

    // 提交一个返回 std::uint64_t 的任务；与 Astra submit 相同的 admission
    // 语义（提交后排队，FIFO 顺序执行）。允许 Worker 内递归提交
    // （fork-join 与 Astra 语义一致）。
    std::shared_future<std::uint64_t> submit(std::function<std::uint64_t()> body) {
        auto task = std::make_shared<std::packaged_task<std::uint64_t()>>(std::move(body));
        std::shared_future<std::uint64_t> future = task->get_future().share();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.emplace_back([task] { (*task)(); });
        }
        cv_.notify_one();
        return future;
    }

    // Worker 内等待 = helping（R-052 / D-048 同语义）：执行队列任务直到
    // 目标完成；外部线程直接阻塞等待。缺少 helping 的纯 FIFO 会在递归
    // fork-join 上死锁，无法充当可比 baseline。
    std::uint64_t wait_result(const std::shared_future<std::uint64_t>& future) {
        if (t_owner_ != this) {
            return future.get();
        }
        while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            std::function<void()> job;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty()) {
                    job = std::move(queue_.front());
                    queue_.pop_front();
                }
            }
            if (job) {
                job();
            } else {
                std::this_thread::yield();
            }
        }
        return future.get();
    }

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

private:
    inline static thread_local GlobalFifoBaseline* t_owner_{nullptr};

    void worker_loop() {
        t_owner_ = this;
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
                if (stopped_ && queue_.empty()) {
                    return;
                }
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stopped_{false};
    std::vector<std::thread> workers_;
};

}  // namespace astra::bench

#endif  // ASTRA_BENCH_GLOBAL_FIFO_BASELINE_HPP
