#ifndef ASTRA_SRC_REAPER_REGISTRY_HPP
#define ASTRA_SRC_REAPER_REGISTRY_HPP

#include <astra/export.hpp>
#include <astra/id.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// ============================================================================
// Reaper 设计思想与 Coordinator 拓扑（AST-007 / D-020 / D-021 / D-022 / R-107）
// ----------------------------------------------------------------------------
// 1. 进程级单协调线程拓扑（D-021 / R-107）
//    一个进程内有且仅有一个逻辑 Reaper Service，由恰好一个不属于任何
//    Scheduler 的专用 coordinator thread 驱动全部 Runtime 的回收。
//    不得为单个 Scheduler 或单次 handoff 创建额外 Reaper thread，
//    coordinator 不得执行用户任务、参与 work stealing 或形成 Internal Submission。
//
// 2. Pending 与 Join Ready 状态分离（D-020 / R-025 / R-026）
//    - Pending 阶段：Worker handoff 发生后，Reaper 立即持有 Runtime State
//      强引用，但绝不同步等待活动任务或 Drain Work Closure；
//    - Join Ready 阶段：仅当全部 Worker 不可逆地退出工作循环后，单调进入
//      Join Ready；coordinator 认领唯一 join 权并执行非阻塞 join 与发布 Stopped。
//    - Head-of-Line 隔离：长期处于 Pending 状态的 Runtime 绝不阻塞其他
//      Join Ready Runtime 的及时回收（R-025）。
//
// 3. 空闲保持与无反复停启（D-022 / R-028）
//    Reaper Service 首次建立后，在全部 Runtime 回收完毕时进入阻塞空闲等待，
//    保持同一 coordinator 线程，不因队列为空而自动停止或重建。
// ============================================================================

namespace astra::detail {

// 进程级 Reaper 注册与 Finalization 状态机（D-021, D-022, D-023, D-024, D-155, D-156）。
enum class RegistrationState : std::uint8_t {
    Open = 0,
    Finalizing = 1,
    Finalized = 2,
};

// 预留的 Handoff 能力槽位（R-023, R-024, R-025, R-026）。
struct ASTRA_NO_EXPORT HandoffCapabilitySlot {
    RuntimeId runtime_id{};
    std::atomic<bool> handoff_executed{false};
    std::atomic<bool> join_ready{false};
    std::atomic<bool> join_claimed{false};
    std::shared_ptr<void> retained_state{};
    std::function<void()> cleanup_fn{};
    std::function<void()> request_graceful_fn{};
    std::function<void()> request_immediate_fn{};
};

class ASTRA_NO_EXPORT ReaperRegistry {
public:
    static ReaperRegistry& instance() noexcept;

    // 尝试在 Worker 启动前注册 Runtime 并预留 Reaper handoff 能力（R-023, R-097）。
    // 同时确保单例 coordinator 线程已启动（D-021 / R-107）。
    bool register_runtime(
        RuntimeId id,
        std::function<void()> req_graceful = nullptr,
        std::function<void()> req_immediate = nullptr);

    // 撤销注册并释放预留能力（用于 startup rollback 或正常非 Worker 析构）。
    void unregister_runtime(RuntimeId id) noexcept;

    // 检查注册门禁是否开放。
    [[nodiscard]] bool is_registration_open() const noexcept;

    // 永久关闭注册并转入 Finalizing（D-023, D-156）。
    void close_registration() noexcept;

    // 向全部已注册 Runtime 广播 Immediate 升级请求（R-038 / R-104）。
    void request_all_immediate() noexcept;

    // 查找已预留的 handoff 能力插槽（R-023, R-024）。
    [[nodiscard]] HandoffCapabilitySlot* find_slot(RuntimeId id) noexcept;

    // 执行 Worker 端的孤儿所有权移交（R-021, R-022, R-024）。
    // 将 Runtime State 强所有权与 cleanup 回调保存入已预留的 slot，进入 Pending 状态（R-025）。
    void execute_worker_handoff(
        RuntimeId id,
        std::shared_ptr<void> state,
        std::function<void()> cleanup_fn) noexcept;

    // 通知 Reaper 某个 Runtime 的全部 Worker 已退出循环，单调进入 Join Ready 状态（R-026）。
    void notify_join_ready(RuntimeId id) noexcept;

    // 获取当前 Reaper coordinator 线程数（R-107：恰好为 1，或未启动时为 0）。
    [[nodiscard]] std::size_t coordinator_thread_count() const noexcept;

    // --- 测试与故障注入专用 Seam ---
    void reset_for_testing() noexcept;
    void inject_handoff_reservation_failure(bool fail) noexcept;
    void inject_worker_creation_failure_at(std::size_t index) noexcept;
    [[nodiscard]] bool should_fail_reservation() const noexcept;
    [[nodiscard]] std::size_t worker_creation_failure_index() const noexcept;
    [[nodiscard]] std::size_t registered_count() const noexcept;

private:
    ReaperRegistry() = default;
    ~ReaperRegistry();
    ReaperRegistry(const ReaperRegistry&) = delete;
    ReaperRegistry& operator=(const ReaperRegistry&) = delete;

    void ensure_coordinator_started_locked();
    void coordinator_loop() noexcept;
    bool has_join_ready_slot_locked() const noexcept;

    mutable std::mutex mutex_;
    std::condition_variable coordinator_cv_;
    RegistrationState state_{RegistrationState::Open};
    std::vector<std::uint64_t> registered_ids_;
    std::vector<std::unique_ptr<HandoffCapabilitySlot>> slots_;

    std::unique_ptr<std::thread> coordinator_thread_;
    bool coordinator_stop_{false};

    bool inject_reservation_fail_{false};
    std::size_t inject_worker_fail_at_{0}; // 0 表示不注入
};

}  // namespace astra::detail

#endif  // ASTRA_SRC_REAPER_REGISTRY_HPP
