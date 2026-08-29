#ifndef ASTRA_BENCH_HARNESS_HPP
#define ASTRA_BENCH_HARNESS_HPP

// AstraScheduler scenario benchmark harness（AST-049 / R-089 / D-141）。
// 每 case 固定阶段协议：setup → warmup → timed region（steady clock、barrier）
// → drain/verification → teardown；setup/teardown 绝不进入 timed region。
// checksum/rejection/drop/子进程异常使 sample invalid，而非产生性能值。

#include <astra/export.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace astra::bench {

// 单次 sample 的结果：valid=false 时不产生任何可比较性能值（D-141）。
struct SampleResult {
    bool valid{false};
    std::string invalid_reason;      // invalid 时的机器可读原因
    std::uint64_t timed_ns{0};       // timed region 实测纳秒（steady clock）
    std::uint64_t checksum{0};       // timed+verified 工作的 checksum
    std::uint64_t completed_work{0}; // timed region 内记账的工作量
};

struct Verification {
    bool ok{false};
    std::string reason;              // 失败原因（checksum/count/rejection/drop）
};

// case 执行上下文：timed region 内的工作记账与阶段标记。
class CaseContext {
public:
    void account_work(std::uint64_t items) noexcept {
        if (in_timed_region_) {
            timed_work_ += items;
        }
    }
    void account_rejection() noexcept { rejections_ += 1; }
    void account_drop() noexcept { drops_ += 1; }

    [[nodiscard]] bool in_timed_region() const noexcept { return in_timed_region_; }
    [[nodiscard]] std::uint64_t timed_work() const noexcept { return timed_work_; }
    [[nodiscard]] std::uint64_t rejections() const noexcept { return rejections_; }
    [[nodiscard]] std::uint64_t drops() const noexcept { return drops_; }

    void enter_timed_region() noexcept { in_timed_region_ = true; }
    void exit_timed_region() noexcept { in_timed_region_ = false; }

private:
    bool in_timed_region_{false};
    std::uint64_t timed_work_{0};
    std::uint64_t rejections_{0};
    std::uint64_t drops_{0};
};

// 场景 case 协议（D-141：setup/warmup/timed/verify/teardown 分离）。
struct ScenarioCase {
    std::string name;
    // 阶段函数；任意阶段抛出异常 ⇒ 该 sample invalid（不产生性能值）。
    std::function<void(CaseContext&)> setup;
    std::function<void(CaseContext&)> warmup;                    // 可为空
    std::function<void(CaseContext&)> timed_region;              // 被测量
    std::function<Verification(CaseContext&)> verify;            // checksum/count 校验
    std::function<void(CaseContext&)> teardown;
    std::uint64_t expected_work{0};                              // timed region 应完成的工作量
    std::uint64_t expected_checksum{0};
    bool isolated_process{false};   // Finalization/Reaper 等不可重启状态 ⇒ 每样本子进程
    std::uint64_t warmup_iterations{1};
    std::map<std::string, std::string> metadata;  // MetricsLevel/trace 状态/timed region 内容声明
};

// 单 sample 执行：严格按阶段顺序，任何异常/校验失败/空 repetition/工作缺失
// 都使 sample invalid。timed_ns 仅在 valid 时可比较；teardown 在所有路径执行。
[[nodiscard]] inline SampleResult run_case_sample(const ScenarioCase& fc) {
    SampleResult result;
    CaseContext ctx;
    try {
        if (fc.setup) {
            fc.setup(ctx);
        }
        for (std::uint64_t i = 0; i < fc.warmup_iterations && fc.warmup; ++i) {
            fc.warmup(ctx);
        }

        // timed region：steady clock 单一测量区（barrier 由 runner 层协调）。
        const auto t0 = std::chrono::steady_clock::now();
        ctx.enter_timed_region();
        if (fc.timed_region) {
            fc.timed_region(ctx);
        }
        ctx.exit_timed_region();
        const auto t1 = std::chrono::steady_clock::now();
        result.timed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

        // drain/verification：拒绝/丢失/工作量/checksum 全部校验。
        if (ctx.rejections() != 0) {
            result.invalid_reason = "unexpected_rejection";
        } else if (ctx.drops() != 0) {
            result.invalid_reason = "unexpected_drop";
        } else if (fc.expected_work != 0 && ctx.timed_work() == 0) {
            result.invalid_reason = "empty_repetition";
        } else if (fc.expected_work != 0 && ctx.timed_work() != fc.expected_work) {
            // timed-region 污染：进入测量区的工作量与声明不符。
            result.invalid_reason = "timed_work_mismatch";
        } else if (fc.verify) {
            Verification v = fc.verify(ctx);
            if (!v.ok) {
                result.invalid_reason = v.reason.empty() ? "verification_failed" : v.reason;
            }
        }
        if (result.invalid_reason.empty()) {
            result.checksum = fc.expected_checksum;
            result.completed_work = ctx.timed_work();
            result.valid = true;
        }
    } catch (const std::exception& e) {
        result.valid = false;
        result.invalid_reason = std::string("exception:") + e.what();
    } catch (...) {
        result.valid = false;
        result.invalid_reason = "exception:unknown";
    }

    // teardown 在 valid 与 invalid 路径都必须执行；失败覆盖为 invalid。
    try {
        if (fc.teardown) {
            fc.teardown(ctx);
        }
    } catch (const std::exception& e) {
        result.valid = false;
        result.invalid_reason = std::string("exception:teardown:") + e.what();
    } catch (...) {
        result.valid = false;
        result.invalid_reason = "exception:teardown";
    }
    return result;
}

}  // namespace astra::bench

#endif  // ASTRA_BENCH_HARNESS_HPP
