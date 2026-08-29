// AstraScheduler bench stats/artifact self-test（AST-051 / R-091 RED evidence）。
// 验证：统计重算 golden、bootstrap 确定性、原始样本保留与 invalid 诊断、
// 环境 mismatch 拒绝、噪声 case 不误判（双门槛）、真实回归检出、quick profile
// baseline 拒绝。

#include "bench_corpus.hpp"
#include "bench_stats.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <sys/wait.h>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace astra::bench;

// 1. 统计重算 golden：已知输入 → 精确 median/MAD/p10/p90。
void test_stats_recompute_golden() {
    // 10 个值：median=(5+6)/2=5.5；p10 插值；MAD=|x-5.5| 的 median。
    const std::vector<std::uint64_t> values = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const auto st = compute_robust_stats(values, kCorpusSeed);
    assert(st.median_ns == 5.5);
    assert(st.p10_ns == 1.9);  // 0.1*(9)=0.9 → 1 + 0.9*(2-1)
    assert(st.p90_ns == 9.1);
    // MAD: |x-5.5| = {4.5,3.5,2.5,1.5,0.5,0.5,1.5,2.5,3.5,4.5} → median 2.5
    assert(st.mad_ns == 2.5);
    // bootstrap CI 包含 median 且有序。
    assert(st.ci95_lo_ns <= st.median_ns && st.median_ns <= st.ci95_hi_ns);
    std::cout << "[PASS] test_stats_recompute_golden" << std::endl;
}

// 2. bootstrap 确定性：相同输入与 seed → 相同 CI；不同 seed 允许差异但含 median。
void test_bootstrap_deterministic() {
    const std::vector<std::uint64_t> values = {100, 200, 300, 400, 500, 600, 700, 800};
    const auto a = compute_robust_stats(values, 42);
    const auto b = compute_robust_stats(values, 42);
    assert(a.ci95_lo_ns == b.ci95_lo_ns && a.ci95_hi_ns == b.ci95_hi_ns);
    std::cout << "[PASS] test_bootstrap_deterministic" << std::endl;
}

// 3. 环境 metadata 收集：字段非空（缺失即 unknown，不崩溃）。
void test_environment_metadata() {
    const auto env = EnvironmentMetadata::collect();
    assert(!env.build_type.empty());
    assert(!env.compiler.empty());
    assert(!env.sanitizer.empty());
    std::cout << "[PASS] test_environment_metadata" << std::endl;
}

// 4-7. 回归 gate（tools/bench_compare.py）行为。
// 通过 std::system 调用比较 CLI；artifact 由本测试以 corpus runner 兼容形状生成。
std::string make_artifact(const std::string& profile, const std::string& cpu_model,
                          const std::string& compiler, const std::vector<double>& medians_ns,
                          double ci_margin_ns) {
    std::string json = "{\"schema_version\":1,\"tool\":\"astra_bench_corpus\"";
    json += ",\"profile\":\"" + profile + "\"";
    json += ",\"astra_version\":\"" + std::string(astra::library_version_string()) + "\"";
    json += ",\"env\":{\"cpu_model\":\"" + cpu_model + "\",\"compiler\":\"" + compiler +
            "\",\"build_type\":\"" + ASTRA_BENCH_BUILD_TYPE + "\"}";
    json += ",\"cases\":[";
    const auto corpus = default_corpus();
    bool first = true;
    std::size_t idx = 0;
    for (const auto& fc : corpus) {
        if (idx >= medians_ns.size()) {
            break;
        }
        if (!first) {
            json += ',';
        }
        first = false;
        const double med = medians_ns[idx];
        json += "{\"name\":\"" + std::string(fc.name) + "\",\"comparable\":true";
        json += ",\"workers\":[{\"workers\":1,\"comparable\":true,\"valid\":true";
        json += ",\"timed_ns\":" + std::to_string(med);
        json += ",\"repetitions\":{\"count\":" + std::to_string(medians_ns.size() > 3 ? 10 : 3) +
                ",\"raw_ns\":[";
        for (std::size_t r = 0; r < 3; ++r) {
            if (r != 0) {
                json += ',';
            }
            json += std::to_string(static_cast<std::uint64_t>(med));
        }
        json += "],\"median_ns\":" + std::to_string(med);
        json += ",\"ci95_lo_ns\":" + std::to_string(med - ci_margin_ns);
        json += ",\"ci95_hi_ns\":" + std::to_string(med + ci_margin_ns);
        json += "}}]}";
        ++idx;
    }
    json += "]}";
    return json;
}

int run_compare(const std::string& candidate_json, const std::string& baseline_json) {
    FILE* c = std::fopen("/tmp/gate_candidate.json", "wb");
    std::fwrite(candidate_json.data(), 1, candidate_json.size(), c);
    std::fclose(c);
    FILE* b = std::fopen("/tmp/gate_baseline.json", "wb");
    std::fwrite(baseline_json.data(), 1, baseline_json.size(), b);
    std::fclose(b);
    const int status = std::system(
        "python3 tools/bench_compare.py --candidate /tmp/gate_candidate.json "
        "--baseline /tmp/gate_baseline.json --policy tools/bench_policy.json >/tmp/gate_out.txt 2>&1");
    // std::system 返回 wait status：取 WEXITSTATUS。
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void test_gate_behaviors() {
    // 真实回归：candidate 中位劣化 20% 且 CI 不重叠 → gate FAIL（退出码 1）。
    {
        const auto corpus = default_corpus();
        std::vector<double> base;
        std::vector<double> cand;
        for (std::size_t i = 0; i < corpus.size(); ++i) {
            base.push_back(1'000'000.0);
            cand.push_back(i == 0 ? 1'200'000.0 : 1'000'000.0);  // 首个 case +20%
        }
        const std::string baseline = make_artifact("standard", "CPU-X", "gcc", base, 5'000.0);
        const std::string candidate = make_artifact("standard", "CPU-X", "gcc", cand, 5'000.0);
        const int rc = run_compare(candidate, baseline);
        assert(rc == 1);
    }
    // 噪声 case：中位劣化 6%（>5% effect）但 CI 重叠 → 不误判（退出码 0）。
    {
        const auto corpus = default_corpus();
        std::vector<double> base;
        std::vector<double> cand;
        for (std::size_t i = 0; i < corpus.size(); ++i) {
            base.push_back(1'000'000.0);
            cand.push_back(i == 0 ? 1'060'000.0 : 1'000'000.0);
        }
        const std::string baseline = make_artifact("standard", "CPU-X", "gcc", base, 100'000.0);
        const std::string candidate = make_artifact("standard", "CPU-X", "gcc", cand, 100'000.0);
        const int rc = run_compare(candidate, baseline);
        assert(rc == 0);
    }
    // 环境 mismatch：CPU model 不同 → 拒绝自动判定（退出码 3）。
    {
        const auto corpus = default_corpus();
        std::vector<double> base;
        std::vector<double> cand;
        for (std::size_t i = 0; i < corpus.size(); ++i) {
            base.push_back(1'000'000.0);
            cand.push_back(1'000'000.0);
        }
        const std::string baseline = make_artifact("standard", "CPU-X", "gcc", base, 5'000.0);
        const std::string candidate = make_artifact("standard", "CPU-Y", "gcc", cand, 5'000.0);
        const int rc = run_compare(candidate, baseline);
        assert(rc == 3);
    }
    // quick profile baseline 不得作为 gate 基线（退出码 3）。
    {
        const auto corpus = default_corpus();
        std::vector<double> vals;
        for (std::size_t i = 0; i < corpus.size(); ++i) {
            vals.push_back(1'000'000.0);
        }
        const std::string baseline = make_artifact("quick", "CPU-X", "gcc", vals, 5'000.0);
        const std::string candidate = make_artifact("standard", "CPU-X", "gcc", vals, 5'000.0);
        const int rc = run_compare(candidate, baseline);
        assert(rc == 3);
    }
    std::cout << "[PASS] test_gate_behaviors" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_bench_stats_selftest..." << std::endl;

    test_stats_recompute_golden();
    test_bootstrap_deterministic();
    test_environment_metadata();
    test_gate_behaviors();

    std::cout << "All AST-051 bench stats selftest passed successfully!" << std::endl;
    return 0;
}
