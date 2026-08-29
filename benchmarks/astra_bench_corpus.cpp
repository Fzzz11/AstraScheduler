// AstraScheduler 固定 benchmark corpus runner（AST-050/AST-051 / R-003/R-090/R-091 / D-142/D-143）。
// Standard profile：每 case/variant ≥2s warmup + 10 个独立 repetition（目标 ≥1s
// timed region），不删除 outlier，保留全部 raw values 与 invalid 诊断；
// quick/exploratory profile 在 artifact 显式命名，不得用于长期 baseline gate。

#include "bench_corpus.hpp"
#include "bench_stats.hpp"

#include <astra/version.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace astra::bench;

struct RunParams {
    std::string profile{"standard"};
    std::uint64_t warmup_ms{2000};
    std::size_t repetitions{10};
    std::uint64_t min_timed_ms{1000};
    bool params_overridden{false};
};

struct RepetitionRecord {
    bool valid{false};
    std::string invalid_reason;
    std::uint64_t timed_ns{0};
    std::uint64_t checksum{0};
};

std::string json_escape(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out += c;
        }
    }
    out += '"';
    return out;
}

std::uint64_t run_workload_once(const VariantAdapter& adapter, const CorpusCase& fc,
                                std::size_t workers) {
    if (fc.kind == WorkloadKind::ForkJoin) {
        const auto leaf = [iters = fc.kernel_iters](std::uint64_t idx) {
            return cpu_kernel(idx + 1, iters);
        };
        return adapter.run_fork_join(workers, fc.fork_depth, leaf);
    }
    return adapter.run_fan_tasks(workers, fc.task_count, make_fan_body(fc));
}

}  // namespace

int main(int argc, char** argv) {
    RunParams params;
    std::string output_path;
    std::string save_baseline_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--output=", 9) == 0) {
            output_path = argv[i] + 9;
        } else if (std::strncmp(argv[i], "--save-baseline=", 16) == 0) {
            save_baseline_path = argv[i] + 16;
        } else if (std::strncmp(argv[i], "--profile=", 10) == 0) {
            params.profile = argv[i] + 10;
        } else if (std::strncmp(argv[i], "--warmup-ms=", 12) == 0) {
            params.warmup_ms = std::strtoull(argv[i] + 12, nullptr, 10);
            params.params_overridden = true;
        } else if (std::strncmp(argv[i], "--repetitions=", 14) == 0) {
            params.repetitions = std::strtoull(argv[i] + 14, nullptr, 10);
            params.params_overridden = true;
        } else if (std::strncmp(argv[i], "--min-timed-ms=", 15) == 0) {
            params.min_timed_ms = std::strtoull(argv[i] + 15, nullptr, 10);
            params.params_overridden = true;
        }
    }
    if (params.profile == "quick" && !params.params_overridden) {
        // quick/exploratory 默认：单次 warmup + 3 reps，无最小 timed region；
        // artifact 显式记录 profile 与实际参数（R-091 例外边界）。
        params.warmup_ms = 0;
        params.repetitions = 3;
        params.min_timed_ms = 0;
    }

    const auto corpus = default_corpus();
    std::vector<VariantAdapter> adapters;
    adapters.push_back(make_astra_adapter());
    adapters.push_back(make_global_fifo_adapter());
    adapters.push_back(make_std_async_adapter());

    const std::size_t hw =
        std::thread::hardware_concurrency() == 0 ? 1 : std::thread::hardware_concurrency();
    const auto matrix = worker_matrix(hw);
    const auto env = EnvironmentMetadata::collect();

    BaselineRecord baseline;
    baseline.astra_version = std::string(astra::library_version_string());
    baseline.seed = kCorpusSeed;

    std::string json = "{\"schema_version\":1,\"tool\":\"astra_bench_corpus\"";
    json += ",\"profile\":" + json_escape(params.profile);
    json += ",\"params\":{\"warmup_ms\":" + std::to_string(params.warmup_ms);
    json += ",\"repetitions\":" + std::to_string(params.repetitions);
    json += ",\"min_timed_ms\":" + std::to_string(params.min_timed_ms);
    json += ",\"overridden\":";
    json += params.params_overridden ? "true" : "false";
    json += '}';
    json += ",\"astra_version\":" + json_escape(baseline.astra_version);
    json += ",\"seed\":";
    json += std::to_string(kCorpusSeed);
    json += ",\"hardware_concurrency\":";
    json += std::to_string(hw);
    json += ",\"worker_matrix\":[";
    for (std::size_t i = 0; i < matrix.size(); ++i) {
        if (i != 0) json += ',';
        json += std::to_string(matrix[i]);
    }
    json += "],\"oneTBB\":\"unavailable (optional adapter absent; not comparable subset skipped)\"";
    json += ",\"env\":{\"os_sysname\":" + json_escape(env.os_sysname);
    json += ",\"kernel_release\":" + json_escape(env.kernel_release);
    json += ",\"cpu_model\":" + json_escape(env.cpu_model);
    json += ",\"cpu_logical_count\":" + json_escape(env.cpu_logical_count);
    json += ",\"git_commit\":" + json_escape(env.git_commit);
    json += ",\"git_dirty\":" + json_escape(env.git_dirty);
    json += ",\"compiler\":" + json_escape(env.compiler);
    json += ",\"build_type\":" + json_escape(env.build_type);
    json += ",\"sanitizer\":" + json_escape(env.sanitizer);
    json += ",\"compiler_flags\":" + json_escape(env.compiler_flags);
    json += '}';

    json += ",\"cases\":[";
    bool first_case = true;
    for (const auto& fc : corpus) {
        if (!first_case) json += ',';
        first_case = false;
        json += "{\"name\":" + json_escape(fc.name);
        json += ",\"shape\":" + json_escape(fc.shape.count("shape") ? fc.shape.at("shape") : "");

        std::uint64_t reference_checksum = 0;
        bool reference_set = false;
        json += ",\"variants\":[";
        bool first_variant = true;
        for (const auto& adapter : adapters) {
            if (!first_variant) json += ',';
            first_variant = false;
            json += "{\"name\":" + json_escape(adapter.name);
            if (!case_comparable(adapter, fc)) {
                // R-090 例外边界：外部 adapter 缺少等价语义 ⇒ not comparable。
                json += ",\"comparable\":false,\"limitations\":" + json_escape(adapter.limitations);
                json += '}';
                continue;
            }
            json += ",\"comparable\":true,\"limitations\":" + json_escape(adapter.limitations);
            json += ",\"workers\":[";
            bool first_w = true;
            for (const std::size_t w : matrix) {
                if (!first_w) json += ',';
                first_w = false;

                // warmup：先运行一次估算单次耗时；standard 下循环至 warmup_ms
                //（warmup 不计入任何统计）。
                const auto warmup_start = std::chrono::steady_clock::now();
                const auto t_est0 = std::chrono::steady_clock::now();
                (void)run_workload_once(adapter, fc, w);
                std::uint64_t est_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t_est0).count());
                while (params.warmup_ms > 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - warmup_start).count();
                    if (elapsed >= static_cast<std::int64_t>(params.warmup_ms)) break;
                    (void)run_workload_once(adapter, fc, w);
                }

                // 冻结 batch：使每个 repetition 的 timed region ≥ min_timed_ms
                //（quick 为 1：单次 workload）。
                std::size_t batch = 1;
                if (params.min_timed_ms > 0) {
                    const std::uint64_t target_ns = params.min_timed_ms * 1'000'000ull;
                    const std::uint64_t est = est_ns == 0 ? 1 : est_ns;
                    batch = static_cast<std::size_t>((target_ns + est - 1) / est);
                    if (batch == 0) batch = 1;
                }

                std::vector<RepetitionRecord> reps;
                reps.reserve(params.repetitions);
                for (std::size_t r = 0; r < params.repetitions; ++r) {
                    RepetitionRecord rep;
                    try {
                        const auto t0 = std::chrono::steady_clock::now();
                        std::uint64_t checksum = 0;
                        for (std::size_t b = 0; b < batch; ++b) {
                            checksum ^= run_workload_once(adapter, fc, w);
                        }
                        const auto t1 = std::chrono::steady_clock::now();
                        rep.timed_ns = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                        rep.checksum = checksum;
                        rep.valid = true;
                    } catch (const std::exception& e) {
                        rep.valid = false;
                        rep.invalid_reason = std::string("exception:") + e.what();
                    } catch (...) {
                        rep.valid = false;
                        rep.invalid_reason = "exception:unknown";
                    }
                    reps.push_back(rep);
                }

                // 统计：仅有效 repetition；invalid 保留原因不混入统计（D-143）。
                std::vector<std::uint64_t> valid_ns;
                for (const auto& rep : reps) {
                    if (rep.valid) valid_ns.push_back(rep.timed_ns);
                }
                const std::uint64_t stats_seed =
                    kCorpusSeed ^ static_cast<std::uint64_t>(std::hash<std::string>{}(
                                      std::string(fc.name) + adapter.name + std::to_string(w)));
                const auto stats = compute_robust_stats(valid_ns, stats_seed);

                if (!reference_set) {
                    reference_checksum = reps.empty() ? 0 : reps.front().checksum;
                    reference_set = true;
                }

                json += "{\"workers\":";
                json += std::to_string(w);
                json += ",\"comparable\":true";
                json += ",\"batch\":";
                json += std::to_string(batch);
                json += ",\"valid\":";
                json += (valid_ns.size() == reps.size()) ? "true" : "false";
                json += ",\"checksum\":";
                json += std::to_string(reference_checksum);
                json += ",\"repetitions\":{\"count\":";
                json += std::to_string(reps.size());
                json += ",\"raw_ns\":[";
                for (std::size_t r = 0; r < reps.size(); ++r) {
                    if (r != 0) json += ',';
                    json += std::to_string(reps[r].timed_ns);
                }
                json += "],\"checksums\":[";
                for (std::size_t r = 0; r < reps.size(); ++r) {
                    if (r != 0) json += ',';
                    json += std::to_string(reps[r].checksum);
                }
                json += "],\"invalid\":[";
                bool first_inv = true;
                for (std::size_t r = 0; r < reps.size(); ++r) {
                    if (reps[r].valid) continue;
                    if (!first_inv) json += ',';
                    first_inv = false;
                    json += "{\"index\":" + std::to_string(r);
                    json += ",\"reason\":" + json_escape(reps[r].invalid_reason) + '}';
                }
                json += "]";
                json += ",\"median_ns\":" + std::to_string(stats.median_ns);
                json += ",\"mad_ns\":" + std::to_string(stats.mad_ns);
                json += ",\"p10_ns\":" + std::to_string(stats.p10_ns);
                json += ",\"p90_ns\":" + std::to_string(stats.p90_ns);
                json += ",\"ci95_lo_ns\":" + std::to_string(stats.ci95_lo_ns);
                json += ",\"ci95_hi_ns\":" + std::to_string(stats.ci95_hi_ns);
                json += "}}";
            }
            json += "]}";
        }
        json += "]";

        if (reference_set) {
            baseline.case_checksums.emplace_back(fc.name, reference_checksum);
        }
        json += '}';
    }
    json += "]}";

    if (!save_baseline_path.empty()) {
        const std::string baseline_json = save_baseline(baseline);
        FILE* f = std::fopen(save_baseline_path.c_str(), "wb");
        if (!f) {
            std::cerr << "cannot write baseline: " << save_baseline_path << std::endl;
            return 1;
        }
        std::fwrite(baseline_json.data(), 1, baseline_json.size(), f);
        std::fclose(f);
        std::cout << "wrote global baseline: " << save_baseline_path << std::endl;
    }
    if (!output_path.empty()) {
        FILE* f = std::fopen(output_path.c_str(), "wb");
        if (!f) {
            std::cerr << "cannot open output: " << output_path << std::endl;
            return 1;
        }
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        std::cout << "wrote artifact: " << output_path << std::endl;
    } else {
        std::cout << json << std::endl;
    }
    return 0;
}
