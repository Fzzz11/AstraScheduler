#ifndef ASTRA_SRC_REAPER_REGISTRY_HPP
#define ASTRA_SRC_REAPER_REGISTRY_HPP

#include <astra/export.hpp>
#include <astra/id.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// ============================================================================
// Reaper 设计思想（为什么需要 ReaperRegistry）
// ----------------------------------------------------------------------------
// 1. 问题：最后一个 Handle 在自己的 Worker 上析构
//    Scheduler 是 copyable/movable 共享 Handle，Runtime State 被 Handle、
//    Worker 执行路径与 Reaper 回收路径三方共享。若最后一个关联 Handle
//    恰好在本 Scheduler 的某个 Worker 线程上析构，析构者不能同步等待
//    自己所在 Runtime 的全部 Worker join（self-join 死锁），也不能直接
//    销毁仍被兄弟 Worker 引用的 Runtime State（use-after-free）。因此
//    需要一个不属于任何 Scheduler 的执行上下文接管所有权并完成最终回收
//    —— 这就是进程级 Reaper Service（D-021 / ADR-0011）。
//
// 2. 约束：析构边界的 handoff 必须 noexcept、零分配、零线程创建（R-024）
//    析构无法报告失败、无法恢复，所以所有权移交只能在"最后一个 Handle"
//    析构的瞬间做原子级轻量操作。任何可失败的资源准备（注册、堆分配、
//    coordinator 建立）都必须提前到仍有正常错误通道的启动阶段完成
//    （D-019 / R-023），否则失败将无处安放。
//
// 3. 由此固定的启动顺序（D-155, D-156, R-097）
//    ① 校验 options；
//    ② 向 ReaperRegistry 注册 Runtime 并预留 HandoffCapabilitySlot；
//    ③ 创建全部 Worker，经 startup barrier 后一次发布 Running。
//    任一步失败则完整回滚：join 已启动 Worker、撤销注册，保证 0 活跃
//    Worker、0 注册泄漏，不返回可观察的半启动 Handle。
//
// 4. ReaperRegistry 的职责
//    - 进程级注册门禁（单例）：记录哪些 Runtime 已纳入 Reaper 核算；
//    - Finalization 状态机 Open -> Finalizing -> Finalized：begin_finalization()
//      线性化地永久关门（D-023 / ADR-0013）。关门前已注册的 Runtime 纳入
//      终结核算，关门后的新构造在创建 Worker 前失败（D-156）；
//    - 预留 HandoffCapabilitySlot：把运行期 noexcept 移交所需的最小能力
//      提前分配，保证 Worker 上孤儿 handoff 不申请新资源（R-023, R-024）；
//    - 服务空闲不自动退出，只在进程级 Finalization 阶段由 coordinator
//      收尾（D-022 / ADR-0012）。
//
// 5. 与 registry 配合的运行时回收语义
//    - 可长期持有 Pending Runtime State，但不得阻塞其他 Join Ready
//      Runtime 的回收（head-of-line 隔离，D-020 / R-025）；
//    - 仅当 Runtime 单调进入 Join Ready 后认领唯一 join 并发布 Stopped
//      （D-020 / R-026）；
//    - Finalization 对核算集合内全部 Runtime 请求 Graceful，允许显式
//      shutdown_now() 单向升级为 Immediate（D-024 / ADR-0014）；
//    - 控制面不可恢复故障 fail-fast：noexcept 尽力诊断后 std::terminate()，
//      不得伪装成 TimedOut/Stopped/Finalized，也不得 detach/restart
//      （D-040 / ADR-0018）。
// ============================================================================

namespace astra::detail {

// 进程级 Reaper 注册与 Finalization 状态机（D-021, D-022, D-023, D-024, D-155, D-156）。
enum class RegistrationState : std::uint8_t {
    Open = 0,
    Finalizing = 1,
    Finalized = 2,
};

// 预留的 Handoff 能力槽位（R-023, R-024）。
// 在 Worker 启动前预分配并持有，保证运行期移交 noexcept 且不申请新资源。
struct ASTRA_NO_EXPORT HandoffCapabilitySlot {
    RuntimeId runtime_id{};
    std::atomic<bool> handoff_executed{false};
    std::atomic<bool> join_ready{false};
    std::shared_ptr<void> retained_state{};
    std::unique_ptr<std::thread> reaper_thread{};
};

class ASTRA_NO_EXPORT ReaperRegistry {
public:
    static ReaperRegistry& instance() noexcept;

    // 尝试在 Worker 启动前注册 Runtime 并预留 Reaper handoff 能力（R-023, R-097）。
    // 若 Finalization 已启动或预留失败，返回 false。
    bool register_runtime(RuntimeId id);

    // 撤销注册并释放预留能力（用于 startup rollback 或正常生命周期结束）。
    void unregister_runtime(RuntimeId id) noexcept;

    // 检查注册门禁是否开放。
    [[nodiscard]] bool is_registration_open() const noexcept;

    // 永久关闭注册并转入 Finalizing（D-023, D-156）。
    void close_registration() noexcept;

    // 查找已预留的 handoff 能力插槽（R-023, R-024）。
    [[nodiscard]] HandoffCapabilitySlot* find_slot(RuntimeId id) noexcept;

    // 执行 Worker 端的孤儿所有权移交（R-021, R-022, R-024）。
    void execute_worker_handoff(
        RuntimeId id,
        std::shared_ptr<void> state,
        std::function<void()> cleanup_fn) noexcept;

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

    mutable std::mutex mutex_;
    RegistrationState state_{RegistrationState::Open};
    std::vector<std::uint64_t> registered_ids_;
    std::vector<std::unique_ptr<HandoffCapabilitySlot>> slots_;

    bool inject_reservation_fail_{false};
    std::size_t inject_worker_fail_at_{0}; // 0 表示不注入
};

}  // namespace astra::detail

#endif  // ASTRA_SRC_REAPER_REGISTRY_HPP
