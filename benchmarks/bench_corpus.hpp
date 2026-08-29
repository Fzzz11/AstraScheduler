#ifndef ASTRA_BENCH_CORPUS_HPP
#define ASTRA_BENCH_CORPUS_HPP

// 固定 benchmark corpus（AST-050 / R-003 / R-090 / D-142 / D-150）。
// 每 case 使用确定性 CPU kernel（无 sleep 模拟）、固定 seed 与 checksum；
// 所有 variant 执行同一逻辑工作量并产生相同 checksum/outcome 集合。
// 外部 adapter 缺少等价语义的 case 标注 not_comparable。

#include <astra/coroutine.hpp>
#include <astra/graph.hpp>
#include <astra/scheduler.hpp>
#include <astra/version.hpp>

#include "global_fifo_baseline.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace astra::bench {

// 固定 seed（记录进 artifact；所有随机负载使用）。
constexpr std::uint64_t kCorpusSeed = 0x4153545244303530ull;  // "ASTRD050"

// 确定性 CPU kernel：xorshift 混合，可验证、难以被优化消除（D-142 禁 sleep）。
[[nodiscard]] inline std::uint64_t cpu_kernel(std::uint64_t seed, std::uint64_t iters) noexcept {
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 0xA5A5A5A5A5A5A5A5ull;
    for (std::uint64_t i = 0; i < iters; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x += i * 0xFF51AFD7ED558CCDull;
    }
    return x;
}

// Worker matrix：不超过 hardware concurrency 的唯一 1,2,4,8,... 幂次及最大值。
[[nodiscard]] inline std::vector<std::size_t> worker_matrix(std::size_t hw) {
    std::vector<std::size_t> counts;
    for (std::size_t c = 1; c <= hw; c *= 2) {
        counts.push_back(c);
    }
    if (counts.empty() || counts.back() != hw) {
        counts.push_back(hw);
    }
    return counts;
}

// ---------------------------------------------------------------------------
// Variant adapters：同一 case body 在不同调度实现上执行。
// ---------------------------------------------------------------------------
enum class WorkloadKind {
    FanTasks,        // 纯 submit/drain（micro/cpu/imbalanced 共用形态）
    ForkJoin,        // 递归分治（外部背景 std::async 不可比较）
    Dag,             // 需要 Graph 语义
    Coroutine,       // 需要 Coroutine 语义
    Timer,           // 需要 Timer 语义
    Priority,        // 需要 Priority 语义
    Deadline,        // 需要 Deadline 语义
};

struct AdapterRunError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct VariantAdapter {
    const char* name{};
    // 缺少等价语义时返回 false（case 对该 variant 标 not_comparable）。
    std::function<bool(WorkloadKind)> supports;
    // 提交 count 个 body(i) 任务并等待全部完成，返回结果 sum（checksum）。
    std::function<std::uint64_t(std::size_t workers, std::size_t count,
                                const std::function<std::uint64_t(std::size_t)>& body)>
        run_fan_tasks;
    // 递归 fork-join：depth 层、fanout 2，叶子调用 leaf(i)，返回 sum。
    std::function<std::uint64_t(std::size_t workers, std::size_t depth,
                                const std::function<std::uint64_t(std::uint64_t)>& leaf)>
        run_fork_join;
    std::string limitations;  // artifact 中明确记录 adapter 限制
};

// --- AstraScheduler（当前 Chase-Lev based Global Worker Runtime）---
inline VariantAdapter make_astra_adapter() {
    VariantAdapter a;
    a.name = "astra_scheduler";
    a.supports = [](WorkloadKind) { return true; };
    a.run_fan_tasks = [](std::size_t workers, std::size_t count,
                         const std::function<std::uint64_t(std::size_t)>& body) {
        astra::SchedulerOptions opts{};
        opts.worker_count = workers == 0 ? 1 : workers;
        opts.metrics_level = astra::MetricsLevel::Off;
        astra::Scheduler sched(opts);
        std::vector<astra::TaskHandle<std::uint64_t>> handles;
        handles.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            handles.push_back(sched.submit([i, &body] { return body(i); }));
        }
        std::uint64_t sum = 0;
        for (auto& h : handles) {
            sum += h.get();
        }
        return sum;
    };
    a.run_fork_join = [](std::size_t workers, std::size_t depth,
                         const std::function<std::uint64_t(std::uint64_t)>& leaf) {
        astra::SchedulerOptions opts{};
        opts.worker_count = workers == 0 ? 1 : workers;
        opts.metrics_level = astra::MetricsLevel::Off;
        astra::Scheduler sched(opts);
        // 递归 fork-join：每个内部节点 submit 两个子任务并 join（D-142）。
        std::function<std::uint64_t(std::size_t, std::uint64_t)> recurse =
            [&](std::size_t d, std::uint64_t idx) -> std::uint64_t {
            if (d == 0) {
                return leaf(idx);
            }
            auto left = sched.submit([&recurse, d, idx] { return recurse(d - 1, idx * 2); });
            auto right = sched.submit([&recurse, d, idx] { return recurse(d - 1, idx * 2 + 1); });
            return left.get() + right.get();
        };
        return recurse(depth, 0);
    };
    a.limitations = "full semantic surface";
    return a;
}

// --- In-tree Global FIFO baseline（v0.1.0 语义基线，R-003 对照组）---
inline VariantAdapter make_global_fifo_adapter() {
    VariantAdapter a;
    a.name = "global_fifo_baseline";
    a.supports = [](WorkloadKind kind) {
        return kind == WorkloadKind::FanTasks || kind == WorkloadKind::ForkJoin;
    };
    a.run_fan_tasks = [](std::size_t workers, std::size_t count,
                         const std::function<std::uint64_t(std::size_t)>& body) {
        GlobalFifoBaseline fifo(workers == 0 ? 1 : workers);
        std::vector<std::shared_future<std::uint64_t>> futures;
        futures.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            futures.push_back(fifo.submit([i, &body] { return body(i); }));
        }
        std::uint64_t sum = 0;
        for (auto& f : futures) {
            sum += f.get();
        }
        return sum;
    };
    a.run_fork_join = [](std::size_t workers, std::size_t depth,
                         const std::function<std::uint64_t(std::uint64_t)>& leaf) {
        GlobalFifoBaseline fifo(workers == 0 ? 1 : workers);
        std::function<std::uint64_t(std::size_t, std::uint64_t)> recurse =
            [&fifo, &recurse, depth, &leaf](std::size_t d, std::uint64_t idx) -> std::uint64_t {
            if (d == 0) {
                return leaf(idx);
            }
            auto left = fifo.submit([&recurse, d, idx] { return recurse(d - 1, idx * 2); });
            auto right = fifo.submit([&recurse, d, idx] { return recurse(d - 1, idx * 2 + 1); });
            return fifo.wait_result(left) + fifo.wait_result(right);
        };
        return recurse(depth, 0);
    };
    a.limitations =
        "mutex-protected Global FIFO fixed-worker semantic baseline (R-003); "
        "no Graph/Coroutine/Timer/Priority/Deadline semantics";
    return a;
}

// --- std::async 粗粒度独立背景（R-090：不参与递归/DAG 等 feature ranking）---
inline VariantAdapter make_std_async_adapter() {
    VariantAdapter a;
    a.name = "std_async_background";
    a.supports = [](WorkloadKind kind) { return kind == WorkloadKind::FanTasks; };
    a.run_fan_tasks = [](std::size_t, std::size_t count,
                         const std::function<std::uint64_t(std::size_t)>& body) {
        std::vector<std::future<std::uint64_t>> futures;
        futures.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            futures.push_back(std::async(std::launch::async, [i, &body] { return body(i); }));
        }
        std::uint64_t sum = 0;
        for (auto& f : futures) {
            sum += f.get();
        }
        return sum;
    };
    a.limitations =
        "std::async(std::launch::async) coarse independent background only (R-090); "
        "different thread topology; must not be used as primary regression oracle";
    return a;
}

// ---------------------------------------------------------------------------
// Corpus cases：每 case 固定 seed、shape parameters 与 expected checksum。
// ---------------------------------------------------------------------------
struct CorpusCase {
    const char* name{};
    WorkloadKind kind{};
    std::size_t task_count{};
    std::uint64_t kernel_iters{};   // CPU 工作量（0 = 纯调度 micro）
    std::size_t fork_depth{};       // ForkJoin 专用
    // Astra-only 语义 case 的执行体（checksum 由 case 自行核算）。
    std::function<std::uint64_t(astra::Scheduler&, std::size_t)> astra_only_body;
    std::map<std::string, std::string> shape;  // 记录进 artifact 的 shape 参数
};

[[nodiscard]] inline std::vector<CorpusCase> default_corpus() {
    std::vector<CorpusCase> cases;
    // 1. micro：纯调度开销（同 worker 数下各 variant 可比）。
    cases.push_back(CorpusCase{
        "micro_empty", WorkloadKind::FanTasks, 4096, 0, 0, nullptr,
        {{"shape", "trivial-return-1"}}});
    // 2. CPU calibrated：确定性 kernel。
    cases.push_back(CorpusCase{
        "cpu_calibrated", WorkloadKind::FanTasks, 256, 4000, 0, nullptr,
        {{"shape", "kernel-4000"}}});
    // 3. imbalanced：少数重任务 + 多数轻任务（同总 kernel 量）。
    cases.push_back(CorpusCase{
        "imbalanced", WorkloadKind::FanTasks, 512, 0, 0, nullptr,
        {{"shape", "8-heavy(6000)+504-light(79)"}}});
    // 4. fork-join：递归分治（外部背景 std::async 不可比较）。
    cases.push_back(CorpusCase{
        "fork_join", WorkloadKind::ForkJoin, 0, 2000, 4, nullptr,
        {{"shape", "fanout-2-depth-4"}}});
    // 5. DAG chain：Astra Graph 语义。
    cases.push_back(CorpusCase{
        "dag_chain", WorkloadKind::Dag, 256, 500, 0, nullptr,
        {{"shape", "linear-chain-256"}}});
    // 6. coroutine suspend/resume storm。
    cases.push_back(CorpusCase{
        "coroutine_storm", WorkloadKind::Coroutine, 512, 300, 0, nullptr,
        {{"shape", "yield-per-task"}}});
    // 7. timer storm。
    cases.push_back(CorpusCase{
        "timer_storm", WorkloadKind::Timer, 256, 200, 0, nullptr,
        {{"shape", "sleep-1ms"}}});
    // 8. priority fairness：四个 band 各 256 任务。
    cases.push_back(CorpusCase{
        "priority_fairness", WorkloadKind::Priority, 1024, 300, 0, nullptr,
        {{"shape", "4-bands-x-256"}}});
    // 9. deadline EDF。
    cases.push_back(CorpusCase{
        "deadline_edf", WorkloadKind::Deadline, 512, 300, 0, nullptr,
        {{"shape", "future-deadlines"}}});
    return cases;
}

// FanTasks/ForkJoin 的 body：imbalanced case 特殊分派（重/轻任务）。
[[nodiscard]] inline std::function<std::uint64_t(std::size_t)> make_fan_body(
    const CorpusCase& fc) {
    if (fc.name == std::string("imbalanced")) {
        // 每 64 个任务一个重任务（6000 iters），其余 79 iters：
        // 总 kernel 量与 cpu_calibrated 不同属预期（checksum 仅要求跨 variant 一致）。
        return [](std::size_t i) {
            return (i % 64 == 0) ? cpu_kernel(i + 1, 6000) : cpu_kernel(i + 1, 79);
        };
    }
    const std::uint64_t iters = fc.kernel_iters;
    return [iters](std::size_t i) {
        return iters == 0 ? 1 : cpu_kernel(i + 1, iters);
    };
}

// Astra-only 语义 case：使用 Scheduler 完整语义执行，返回 checksum。
[[nodiscard]] inline std::uint64_t run_astra_only_case(const CorpusCase& fc, std::size_t workers) {
    astra::SchedulerOptions opts{};
    opts.worker_count = workers == 0 ? 1 : workers;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);
    const std::uint64_t iters = fc.kernel_iters;

    if (fc.kind == WorkloadKind::Dag) {
        // 线性 chain：N 个节点依次依赖，checksum = 各节点 kernel 结果之和。
        astra::TaskGraph graph;
        std::atomic<std::uint64_t> sum{0};
        astra::NodeId prev = astra::NodeId{};
        for (std::size_t i = 0; i < fc.task_count; ++i) {
            auto node = graph.emplace([i, iters, &sum] {
                sum.fetch_add(cpu_kernel(i + 1, iters), std::memory_order_relaxed);
            });
            if (prev.valid()) {
                graph.add_edge(prev, node);
            }
            prev = node;
        }
        auto run = sched.run(std::move(graph).freeze());
        run.wait();
        return sum.load(std::memory_order_relaxed);
    }
    if (fc.kind == WorkloadKind::Coroutine) {
        std::atomic<std::uint64_t> sum{0};
        for (std::size_t i = 0; i < fc.task_count; ++i) {
            auto coro = [&](astra::Scheduler& s) -> astra::Task<std::uint64_t> {
                co_await astra::yield();
                co_return cpu_kernel(i + 1, iters);
            };
            auto handle = sched.spawn(coro(sched));
            sum.fetch_add(handle.get(), std::memory_order_relaxed);
        }
        return sum.load(std::memory_order_relaxed);
    }
    if (fc.kind == WorkloadKind::Timer) {
        std::atomic<std::uint64_t> sum{0};
        for (std::size_t i = 0; i < fc.task_count; ++i) {
            auto coro = [&](astra::Scheduler& s) -> astra::Task<std::uint64_t> {
                co_await astra::sleep_for(std::chrono::milliseconds(1));
                co_return cpu_kernel(i + 1, iters);
            };
            auto handle = sched.spawn(coro(sched));
            sum.fetch_add(handle.get(), std::memory_order_relaxed);
        }
        return sum.load(std::memory_order_relaxed);
    }
    if (fc.kind == WorkloadKind::Priority) {
        // 四个 Priority band 轮转提交；checksum = 全部任务 kernel 结果之和。
        std::vector<astra::TaskHandle<std::uint64_t>> handles;
        handles.reserve(fc.task_count);
        const int bands[4] = {0, 1, 2, 3};
        for (std::size_t i = 0; i < fc.task_count; ++i) {
            astra::TaskOptions topts;
            topts.priority = static_cast<astra::Priority>(bands[i % 4]);
            handles.push_back(sched.submit(topts, [i, iters] { return cpu_kernel(i + 1, iters); }));
        }
        std::uint64_t sum = 0;
        for (auto& h : handles) {
            sum += h.get();
        }
        return sum;
    }
    if (fc.kind == WorkloadKind::Deadline) {
        std::vector<astra::TaskHandle<std::uint64_t>> handles;
        handles.reserve(fc.task_count);
        for (std::size_t i = 0; i < fc.task_count; ++i) {
            astra::TaskOptions topts;
            topts.deadline = astra::TaskDeadline(std::chrono::steady_clock::now() +
                                                 std::chrono::milliseconds(50 + (i % 8)));
            handles.push_back(sched.submit(topts, [i, iters] { return cpu_kernel(i + 1, iters); }));
        }
        std::uint64_t sum = 0;
        for (auto& h : handles) {
            sum += h.get();
        }
        return sum;
    }
    throw AdapterRunError(std::string("case not executable as astra-only: ") + fc.name);
}

// case 对 variant 的可执行性：缺失等价语义 ⇒ not comparable（R-090 例外边界）。
[[nodiscard]] inline bool case_comparable(const VariantAdapter& a, const CorpusCase& fc) {
    if (fc.kind == WorkloadKind::FanTasks || fc.kind == WorkloadKind::ForkJoin) {
        return a.supports(fc.kind);
    }
    return false;  // Astra-only 语义 case 对外部/baseline adapter 均 not comparable
}

// ---------------------------------------------------------------------------
// Baseline artifact（R-003）：保存/加载 Global baseline，版本不匹配显式失败。
// ---------------------------------------------------------------------------
struct BaselineRecord {
    std::string astra_version;
    std::uint64_t seed{0};
    std::vector<std::pair<std::string, std::uint64_t>> case_checksums;
};

inline void append_json_escaped(std::string& out, const std::string& text) {
    out += '"';
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default: out += c;
        }
    }
    out += '"';
}

[[nodiscard]] inline std::string save_baseline(const BaselineRecord& record) {
    std::string json = "{\"schema_version\":1,\"astra_version\":";
    append_json_escaped(json, record.astra_version);
    json += ",\"seed\":";
    json += std::to_string(record.seed);
    json += ",\"cases\":[";
    for (std::size_t i = 0; i < record.case_checksums.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        json += "{\"name\":";
        append_json_escaped(json, record.case_checksums[i].first);
        json += ",\"checksum\":";
        json += std::to_string(record.case_checksums[i].second);
        json += '}';
    }
    json += "]}";
    return json;
}

struct BaselineLoadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// 极简确定 JSON 解析：仅接受 save_baseline 的输出形状（固定字段顺序）。
[[nodiscard]] inline BaselineRecord load_baseline(const std::string& json) {
    auto extract = [&json](const std::string& key, std::size_t from) -> std::string {
        const std::string pat = "\"" + key + "\":";
        const auto pos = json.find(pat, from);
        if (pos == std::string::npos) {
            throw BaselineLoadError("baseline JSON missing key: " + key);
        }
        std::size_t vpos = pos + pat.size();
        while (vpos < json.size() && json[vpos] == ' ') {
            ++vpos;
        }
        if (vpos < json.size() && json[vpos] == '"') {
            const auto end = json.find('"', vpos + 1);
            if (end == std::string::npos) {
                throw BaselineLoadError("baseline JSON malformed string");
            }
            return json.substr(vpos + 1, end - vpos - 1);
        }
        const auto end = json.find_first_of(",}", vpos);
        return json.substr(vpos, end - vpos);
    };

    BaselineRecord record;
    const std::string schema = extract("schema_version", 0);
    if (schema != "1") {
        throw BaselineLoadError("baseline schema_version mismatch: " + schema);
    }
    record.astra_version = extract("astra_version", 0);
    if (record.astra_version != std::string(astra::library_version_string())) {
        throw BaselineLoadError("baseline library version mismatch: baseline=" +
                                record.astra_version + " current=" +
                                std::string(astra::library_version_string()));
    }
    record.seed = std::stoull(extract("seed", 0));
    std::size_t pos = 0;
    while (true) {
        const auto name_pos = json.find("\"name\":\"", pos);
        if (name_pos == std::string::npos) {
            break;
        }
        const auto name_end = json.find('"', name_pos + 8);
        const std::string name = json.substr(name_pos + 8, name_end - name_pos - 8);
        const std::string checksum = extract("checksum", name_end);
        record.case_checksums.emplace_back(name, std::stoull(checksum));
        pos = name_end;
    }
    return record;
}

}  // namespace astra::bench

#endif  // ASTRA_BENCH_CORPUS_HPP
