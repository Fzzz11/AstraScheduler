// AstraScheduler scenario runner（AST-049 / R-089 / D-141）。
// 承载多阶段/生命周期 Runtime workloads：JSON artifact 输出、子进程隔离
// （Finalization/Reaper 等不可重启状态每样本一个子进程）、严格 verification。
// 每 case 的 timed region 内容写入 metadata：默认吞吐 case 为 Metrics Off、
// Trace disabled；Scheduler 构造/销毁归 setup/teardown，绝不进入计时区。

#include "bench_harness.hpp"

#include <astra/finalization.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

using astra::bench::CaseContext;
using astra::bench::SampleResult;
using astra::bench::ScenarioCase;

// 单 sample 独占：Scheduler 生命周期归 setup/teardown（D-141 阶段分离）。
std::unique_ptr<astra::Scheduler> g_sample_scheduler;
std::atomic<std::uint64_t> g_last_checksum{0};

// -----------------------------------------------------------------------------
// Case：submit/drain 吞吐 —— timed region 只含 submit+drain
// -----------------------------------------------------------------------------
ScenarioCase make_submit_drain_case(std::uint64_t task_count) {
    ScenarioCase fc;
    fc.name = "scenario.submit_drain";
    fc.expected_work = task_count;
    fc.metadata["metrics_level"] = "Off";
    fc.metadata["trace"] = "disabled";
    fc.metadata["timed_region"] = "submit+drain";
    fc.metadata["primary_metric"] = "ns_per_task";

    fc.setup = [task_count](CaseContext&) {
        astra::SchedulerOptions opts{};
        opts.worker_count = 2;
        opts.metrics_level = astra::MetricsLevel::Off;
        g_sample_scheduler = std::make_unique<astra::Scheduler>(opts);
        auto warm = g_sample_scheduler->submit([] { return 0; });  // 预热路径（不计入 timed）
        warm.get();
    };

    fc.timed_region = [task_count](CaseContext& ctx) {
        std::vector<astra::TaskHandle<std::uint64_t>> handles;
        handles.reserve(task_count);
        for (std::uint64_t i = 1; i <= task_count; ++i) {
            handles.push_back(g_sample_scheduler->submit([i] { return i; }));
        }
        std::uint64_t sum = 0;
        for (auto& h : handles) {
            sum += h.get();
        }
        g_last_checksum.store(sum, std::memory_order_relaxed);
        ctx.account_work(task_count);
    };

    fc.verify = [task_count](CaseContext&) {
        const std::uint64_t expected_sum = task_count * (task_count + 1) / 2;
        if (g_last_checksum.load(std::memory_order_relaxed) != expected_sum) {
            return astra::bench::Verification{false, "checksum_mismatch"};
        }
        return astra::bench::Verification{true, ""};
    };

    fc.teardown = [](CaseContext&) {
        g_sample_scheduler.reset();
        g_last_checksum.store(0, std::memory_order_relaxed);
    };
    return fc;
}

// -----------------------------------------------------------------------------
// Case：Finalization 生命周期 —— 不可重启全局状态，每样本运行在新子进程
// -----------------------------------------------------------------------------
int run_finalization_child() {
    try {
        astra::SchedulerOptions opts{};
        opts.worker_count = 2;
        astra::Scheduler sched(opts);
        auto h = sched.submit([] { return 1; });
        if (h.get() != 1) {
            return 2;
        }
        auto control = astra::begin_finalization();
        if (control.wait_for(5s) != astra::FinalizationWaitResult::Completed) {
            return 3;
        }
        return 0;
    } catch (...) {
        return 1;
    }
}

ScenarioCase make_finalization_case() {
    ScenarioCase fc;
    fc.name = "scenario.finalization_lifecycle";
    fc.isolated_process = true;  // D-035/D-141：Reaper 不可重启 ⇒ 子进程隔离
    fc.expected_work = 1;
    fc.metadata["timed_region"] = "submit+finalization_wait";
    fc.metadata["primary_metric"] = "ns_per_finalization";
    fc.metadata["isolation"] = "child_process";
    fc.timed_region = [](CaseContext& ctx) {
        ctx.account_work(1);
    };
    return fc;
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------

void append_json_escaped(std::string& out, const std::string& text) {
    out += '"';
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out += c;
        }
    }
    out += '"';
}

int run_child_mode(const std::string& case_name) {
    if (case_name == "scenario.finalization_lifecycle") {
        return run_finalization_child();
    }
    std::cerr << "unknown child case: " << case_name << std::endl;
    return 64;
}

int run_runner_mode(int argc, char** argv) {
    const std::size_t kSamplesPerCase = 3;
    std::string output_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--output=", 9) == 0) {
            output_path = argv[i] + 9;
        }
    }

    std::vector<ScenarioCase> cases;
    cases.push_back(make_submit_drain_case(2000));
    cases.push_back(make_finalization_case());

    std::string json = "{\"schema_version\":1,\"tool\":\"astra_bench_scenarios\",\"cases\":[";
    bool first_case = true;
    for (const auto& fc : cases) {
        if (!first_case) {
            json += ',';
        }
        first_case = false;
        json += "{\"name\":";
        append_json_escaped(json, fc.name);
        json += ",\"isolated_process\":";
        json += fc.isolated_process ? "true" : "false";
        json += ",\"samples\":[";
        for (std::size_t s = 0; s < kSamplesPerCase; ++s) {
            SampleResult r;
            if (fc.isolated_process) {
                // 子进程隔离：异常/非零退出 ⇒ invalid sample（D-141）。
                std::string cmd = std::string("\"") + argv[0] + "\" --child=" + fc.name;
                const int rc = std::system(cmd.c_str());
                r.valid = (rc == 0);
                if (!r.valid) {
                    r.invalid_reason = "child_process_failure";
                }
            } else {
                r = astra::bench::run_case_sample(fc);
            }
            if (s != 0) {
                json += ',';
            }
            json += "{\"valid\":";
            json += r.valid ? "true" : "false";
            json += ",\"timed_ns\":";
            json += std::to_string(r.valid ? r.timed_ns : 0);
            json += ",\"checksum\":";
            json += std::to_string(r.checksum);
            json += ",\"completed_work\":";
            json += std::to_string(r.completed_work);
            json += ",\"invalid_reason\":";
            append_json_escaped(json, r.invalid_reason);
            json += '}';
        }
        json += "],\"metadata\":{";
        bool first_meta = true;
        for (const auto& [k, v] : fc.metadata) {
            if (!first_meta) {
                json += ',';
            }
            first_meta = false;
            append_json_escaped(json, k);
            json += ':';
            append_json_escaped(json, v);
        }
        json += "}}";
    }
    json += "]}";

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

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--child=", 8) == 0) {
            return run_child_mode(argv[i] + 8);
        }
    }
    return run_runner_mode(argc, argv);
}
