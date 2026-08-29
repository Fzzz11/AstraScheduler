#ifndef ASTRA_BENCH_STATS_HPP
#define ASTRA_BENCH_STATS_HPP

// Benchmark artifact 统计与环境元数据（AST-051 / R-091 / D-143）。
// 保留全部原始 repetition（不删除 outlier），报告 median/MAD/p10/p90 与
// bootstrap 95% CI；环境/构建/schema 元数据缺失记为 unknown，不猜测。

#include <astra/version.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Linux 开发组件：popen/pclose 用于 git 元数据采集（benchmarks 不进入 public API）。
#if defined(__linux__)
#include <sys/wait.h>
#endif
#include <string>
#include <vector>

namespace astra::bench {

// 确定性 splitmix64：bootstrap 重采样使用固定派生 seed（结果可复现）。
[[nodiscard]] inline std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct RobustStats {
    double median_ns{0};
    double mad_ns{0};
    double p10_ns{0};
    double p90_ns{0};
    double ci95_lo_ns{0};
    double ci95_hi_ns{0};
};

[[nodiscard]] inline double percentile_of(std::vector<double> sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double pos = p * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(pos);
    const auto hi = static_cast<std::size_t>(pos + 0.999999);
    if (hi >= sorted.size()) {
        return sorted.back();
    }
    const double frac = pos - static_cast<double>(lo);
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

// median/MAD/p10/p90 + bootstrap 95% CI（对 median 重采样；确定性 seed）。
[[nodiscard]] inline RobustStats compute_robust_stats(const std::vector<std::uint64_t>& values_ns,
                                                      std::uint64_t seed,
                                                      std::size_t bootstrap_resamples = 2000) {
    RobustStats stats;
    std::vector<double> v;
    v.reserve(values_ns.size());
    for (const auto x : values_ns) {
        v.push_back(static_cast<double>(x));
    }
    if (v.empty()) {
        return stats;
    }
    std::sort(v.begin(), v.end());
    stats.median_ns = percentile_of(v, 0.5);
    stats.p10_ns = percentile_of(v, 0.10);
    stats.p90_ns = percentile_of(v, 0.90);

    std::vector<double> abs_dev;
    abs_dev.reserve(v.size());
    for (const double x : v) {
        abs_dev.push_back(x > stats.median_ns ? x - stats.median_ns : stats.median_ns - x);
    }
    std::sort(abs_dev.begin(), abs_dev.end());
    stats.mad_ns = percentile_of(abs_dev, 0.5);

    // bootstrap：对 median 重采样（固定 seed ⇒ 相同输入得到相同 CI）。
    std::vector<double> boot;
    boot.reserve(bootstrap_resamples);
    std::uint64_t rng = seed ^ 0xC0FFEE123456789Full;
    std::vector<double> sample(v.size());
    for (std::size_t b = 0; b < bootstrap_resamples; ++b) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            sample[i] = v[static_cast<std::size_t>(splitmix64(rng) % v.size())];
        }
        std::vector<double> sorted_sample = sample;
        std::sort(sorted_sample.begin(), sorted_sample.end());
        boot.push_back(percentile_of(sorted_sample, 0.5));
    }
    std::sort(boot.begin(), boot.end());
    stats.ci95_lo_ns = percentile_of(boot, 0.025);
    stats.ci95_hi_ns = percentile_of(boot, 0.975);
    return stats;
}

// ---------------------------------------------------------------------------
// 环境/构建元数据（D-143）：缺失不可探测项记 "unknown"，不猜测。
// ---------------------------------------------------------------------------
struct EnvironmentMetadata {
    std::string os_sysname{"unknown"};
    std::string kernel_release{"unknown"};
    std::string cpu_model{"unknown"};
    std::string cpu_logical_count{"unknown"};
    std::string git_commit{"unknown"};
    std::string git_dirty{"unknown"};
    std::string compiler{"unknown"};
    std::string build_type{ASTRA_BENCH_BUILD_TYPE};
    std::string sanitizer{ASTRA_BENCH_SANITIZER};
    std::string compiler_flags{ASTRA_BENCH_CXX_FLAGS};

    [[nodiscard]] static EnvironmentMetadata collect() {
        EnvironmentMetadata env;
#if defined(__linux__)
        if (FILE* f = std::fopen("/proc/sys/kernel/osrelease", "rb")) {
            char buf[256] = {0};
            const auto n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            if (n > 0) {
                env.kernel_release = buf;
                while (!env.kernel_release.empty() &&
                       (env.kernel_release.back() == '\n' || env.kernel_release.back() == ' ')) {
                    env.kernel_release.pop_back();
                }
            }
        }
        if (FILE* f = std::fopen("/proc/cpuinfo", "rb")) {
            char buf[4096];
            std::string cpuinfo;
            std::size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                cpuinfo.append(buf, n);
            }
            std::fclose(f);
            const auto pos = cpuinfo.find("model name");
            if (pos != std::string::npos) {
                const auto colon = cpuinfo.find(':', pos);
                const auto eol = cpuinfo.find('\n', colon);
                if (colon != std::string::npos && eol != std::string::npos) {
                    env.cpu_model = cpuinfo.substr(colon + 2, eol - colon - 2);
                }
            }
        }
#endif
        if (FILE* f = ::popen("git rev-parse HEAD 2>/dev/null", "r")) {
            char buf[64] = {0};
            if (std::fgets(buf, sizeof(buf), f)) {
                env.git_commit = buf;
                while (!env.git_commit.empty() && env.git_commit.back() == '\n') {
                    env.git_commit.pop_back();
                }
            }
            ::pclose(f);
        }
        if (FILE* f = ::popen("git status --porcelain 2>/dev/null", "r")) {
            char buf[16] = {0};
            env.git_dirty = std::fgets(buf, sizeof(buf), f) ? "true" : "false";
            ::pclose(f);
        }
#if defined(__clang__)
        env.compiler = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
        env.compiler = std::string("gcc ") + __VERSION__;
#endif
        return env;
    }
};

}  // namespace astra::bench

#endif  // ASTRA_BENCH_STATS_HPP
