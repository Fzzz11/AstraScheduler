// AstraScheduler 固定 benchmark corpus runner（AST-050 / R-003 / R-090 / D-142）。
// 运行 Global FIFO baseline / AstraScheduler（/可选 std::async 背景）× corpus
// × worker matrix，输出 JSON artifact（seed/shape/版本/adapter 限制），并支持
// 保存/加载 Global baseline。

#include "bench_corpus.hpp"

#include <astra/version.hpp>

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

}  // namespace

int main(int argc, char** argv) {
    std::string output_path;
    std::string save_baseline_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--output=", 9) == 0) {
            output_path = argv[i] + 9;
        } else if (std::strncmp(argv[i], "--save-baseline=", 16) == 0) {
            save_baseline_path = argv[i] + 16;
        }
    }

    const auto corpus = default_corpus();
    std::vector<VariantAdapter> adapters;
    adapters.push_back(make_astra_adapter());
    adapters.push_back(make_global_fifo_adapter());
    adapters.push_back(make_std_async_adapter());

    const std::size_t hw = std::thread::hardware_concurrency() == 0 ? 1 : std::thread::hardware_concurrency();
    const auto matrix = worker_matrix(hw);

    BaselineRecord baseline;
    baseline.astra_version = std::string(astra::library_version_string());
    baseline.seed = kCorpusSeed;

    std::string json = "{\"schema_version\":1,\"tool\":\"astra_bench_corpus\"";
    json += ",\"astra_version\":";
    append_json_escaped(json, baseline.astra_version);
    json += ",\"seed\":";
    json += std::to_string(kCorpusSeed);
    json += ",\"hardware_concurrency\":";
    json += std::to_string(hw);
    json += ",\"worker_matrix\":[";
    for (std::size_t i = 0; i < matrix.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        json += std::to_string(matrix[i]);
    }
    json += "],\"oneTBB\":\"unavailable (optional adapter absent; not comparable subset skipped)\"";
    json += ",\"cases\":[";
    bool first_case = true;
    for (const auto& fc : corpus) {
        if (!first_case) {
            json += ',';
        }
        first_case = false;
        json += "{\"name\":";
        append_json_escaped(json, fc.name);
        json += ",\"shape\":";
        append_json_escaped(json, fc.shape.count("shape") ? fc.shape.at("shape") : "");

        std::uint64_t reference_checksum = 0;
        bool reference_set = false;
        json += ",\"variants\":[";
        bool first_variant = true;
        for (const auto& adapter : adapters) {
            if (!first_variant) {
                json += ',';
            }
            first_variant = false;
            json += "{\"name\":";
            append_json_escaped(json, adapter.name);
            if (!case_comparable(adapter, fc)) {
                // R-090 例外边界：外部 adapter 缺少等价语义 ⇒ not comparable。
                json += ",\"comparable\":false,\"limitations\":";
                append_json_escaped(json, adapter.limitations);
                json += '}';
                continue;
            }
            json += ",\"comparable\":true,\"limitations\":";
            append_json_escaped(json, adapter.limitations);
            json += ",\"workers\":[";
            bool first_w = true;
            for (const std::size_t w : matrix) {
                if (!first_w) {
                    json += ',';
                }
                first_w = false;
                const auto t0 = std::chrono::steady_clock::now();
                std::uint64_t checksum = 0;
                std::string error;
                try {
                    if (fc.kind == WorkloadKind::ForkJoin) {
                        const auto leaf = [iters = fc.kernel_iters](std::uint64_t idx) {
                            return cpu_kernel(idx + 1, iters);
                        };
                        checksum = adapter.run_fork_join(w, fc.fork_depth, leaf);
                    } else {
                        checksum = adapter.run_fan_tasks(w, fc.task_count, make_fan_body(fc));
                    }
                } catch (const std::exception& e) {
                    error = e.what();
                } catch (...) {
                    error = "unknown";
                }
                const auto t1 = std::chrono::steady_clock::now();
                const auto timed_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                if (!reference_set) {
                    reference_checksum = checksum;
                    reference_set = true;
                }
                json += "{\"workers\":";
                json += std::to_string(w);
                json += ",\"valid\":";
                json += (error.empty() && checksum == reference_checksum) ? "true" : "false";
                json += ",\"timed_ns\":";
                json += std::to_string(timed_ns);
                json += ",\"checksum\":";
                json += std::to_string(checksum);
                if (!error.empty()) {
                    json += ",\"error\":";
                    append_json_escaped(json, error);
                }
                json += '}';
            }
            json += "]}";
        }
        json += "]";

        // 基线 checksum（Global FIFO baseline 的可比 case）。
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
