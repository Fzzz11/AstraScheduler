// AstraScheduler bench corpus self-test（AST-050 / R-003 / R-090 RED evidence）。
// 验证：跨 variant checksum 一致（工作量等价）、baseline 可加载/版本不匹配
// 显式失败、not-comparable 标注、worker matrix 与固定 seed。

#include "bench_corpus.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace astra::bench;

// 1. 工作量等价：可比 variant 在同 case 上产生相同 checksum（R-003 / D-142）。
void test_checksum_equivalence_across_variants() {
    const auto corpus = default_corpus();
    auto astra = make_astra_adapter();
    auto fifo = make_global_fifo_adapter();

    for (const auto& fc : corpus) {
        if (fc.kind != WorkloadKind::FanTasks) {
            continue;
        }
        const auto body = make_fan_body(fc);
        const std::uint64_t cs_astra = astra.run_fan_tasks(2, fc.task_count, body);
        const std::uint64_t cs_fifo = fifo.run_fan_tasks(2, fc.task_count, body);
        assert(cs_astra == cs_fifo);
        // 同 case 重复运行 checksum 稳定（确定性 kernel + 固定 seed）。
        assert(astra.run_fan_tasks(4, fc.task_count, body) == cs_astra);
    }
    std::cout << "[PASS] checksum_equivalence_across_variants" << std::endl;
}

// 2. fork-join 递归：Astra 与 Global FIFO baseline 同 checksum；std::async 不可比较。
void test_fork_join_equivalence_and_not_comparable() {
    const auto corpus = default_corpus();
    const CorpusCase* fj = nullptr;
    for (const auto& fc : corpus) {
        if (fc.kind == WorkloadKind::ForkJoin) {
            fj = &fc;
        }
    }
    assert(fj != nullptr);
    const auto leaf = [fj](std::uint64_t idx) { return cpu_kernel(idx + 1, fj->kernel_iters); };

    auto astra = make_astra_adapter();
    auto fifo = make_global_fifo_adapter();
    auto async_adapter = make_std_async_adapter();

    const std::uint64_t cs_astra = astra.run_fork_join(2, fj->fork_depth, leaf);
    const std::uint64_t cs_fifo = fifo.run_fork_join(2, fj->fork_depth, leaf);
    assert(cs_astra == cs_fifo);

    // R-090：std::async 不支持 ForkJoin ⇒ not comparable。
    assert(!case_comparable(async_adapter, *fj));
    assert(!async_adapter.supports(WorkloadKind::ForkJoin));
    assert(async_adapter.limitations.find("must not be used as primary regression oracle") !=
           std::string::npos);
    std::cout << "[PASS] fork_join_equivalence_and_not_comparable" << std::endl;
}

// 3. Astra-only 语义 case（DAG/Coroutine/Timer/Priority/Deadline）对外部
//    adapter 与 Global FIFO baseline 均 not comparable。
void test_semantic_cases_not_comparable_for_baseline() {
    const auto fifo = make_global_fifo_adapter();
    const auto async_adapter = make_std_async_adapter();
    for (const auto kind : {WorkloadKind::Dag, WorkloadKind::Coroutine, WorkloadKind::Timer,
                            WorkloadKind::Priority, WorkloadKind::Deadline}) {
        assert(!case_comparable(fifo, CorpusCase{"x", kind, 0, 0, 0, nullptr, {}}));
        assert(!case_comparable(async_adapter, CorpusCase{"x", kind, 0, 0, 0, nullptr, {}}));
    }
    std::cout << "[PASS] semantic_cases_not_comparable_for_baseline" << std::endl;
}

// 4. Astra-only 语义 case 全部可执行且 checksum 确定可重复（固定 seed）。
void test_astra_only_semantic_cases() {
    const auto corpus = default_corpus();
    for (const auto& fc : corpus) {
        if (fc.kind == WorkloadKind::FanTasks || fc.kind == WorkloadKind::ForkJoin) {
            continue;
        }
        std::cerr << "running semantic case: " << fc.name << std::endl;
        const std::uint64_t c1 = run_astra_only_case(fc, 2);
        const std::uint64_t c2 = run_astra_only_case(fc, 2);
        assert(c1 == c2);
        assert(c1 != 0 || fc.task_count == 0);
    }
    std::cout << "[PASS] astra_only_semantic_cases" << std::endl;
}

// 5. worker matrix：唯一 1,2,4,... 幂次 + 最大值（不超过 hardware concurrency）。
void test_worker_matrix() {
    const auto matrix = worker_matrix(6);
    assert((matrix == std::vector<std::size_t>{1, 2, 4, 6}));
    const auto pow2 = worker_matrix(8);
    assert((pow2 == std::vector<std::size_t>{1, 2, 4, 8}));
    const auto single = worker_matrix(1);
    assert((single == std::vector<std::size_t>{1}));
    std::cout << "[PASS] worker_matrix" << std::endl;
}

// 6. baseline 保存/加载成功（同版本）。
void test_baseline_save_load() {
    BaselineRecord record;
    record.astra_version = std::string(astra::library_version_string());
    record.seed = kCorpusSeed;
    record.case_checksums = {{"micro_empty", 4096}, {"cpu_calibrated", 12345}};
    const std::string json = save_baseline(record);

    const BaselineRecord loaded = load_baseline(json);
    assert(loaded.astra_version == record.astra_version);
    assert(loaded.seed == kCorpusSeed);
    assert(loaded.case_checksums.size() == 2);
    assert(loaded.case_checksums[0].first == "micro_empty");
    assert(loaded.case_checksums[0].second == 4096);
    std::cout << "[PASS] baseline_save_load" << std::endl;
}

// 7. baseline 版本不匹配 → 显式失败（不静默比较）。
void test_baseline_version_mismatch_rejected() {
    BaselineRecord record;
    record.astra_version = "0.0.1-wrong";
    record.seed = kCorpusSeed;
    record.case_checksums = {{"micro_empty", 1}};
    const std::string json = save_baseline(record);

    bool thrown = false;
    try {
        (void)load_baseline(json);
    } catch (const BaselineLoadError& e) {
        thrown = true;
        assert(std::string(e.what()).find("version mismatch") != std::string::npos);
    }
    assert(thrown);

    // schema 版本不匹配同样显式失败。
    const std::string bad_schema =
        "{\"schema_version\":99,\"astra_version\":\"" +
        std::string(astra::library_version_string()) +
        "\",\"seed\":1,\"cases\":[]}";
    thrown = false;
    try {
        (void)load_baseline(bad_schema);
    } catch (const BaselineLoadError& e) {
        thrown = true;
        assert(std::string(e.what()).find("schema_version mismatch") != std::string::npos);
    }
    assert(thrown);
    std::cout << "[PASS] baseline_version_mismatch_rejected" << std::endl;
}

// 8. 固定 seed 记录：corpus seed 为编译期常量并写入 baseline。
void test_fixed_seed_recorded() {
    assert(kCorpusSeed == 0x4153545244303530ull);
    BaselineRecord record;
    record.astra_version = std::string(astra::library_version_string());
    record.seed = kCorpusSeed;
    (void)save_baseline(record);
    std::cout << "[PASS] fixed_seed_recorded" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_bench_corpus_selftest..." << std::endl;

    test_checksum_equivalence_across_variants();
    test_fork_join_equivalence_and_not_comparable();
    test_semantic_cases_not_comparable_for_baseline();
    test_astra_only_semantic_cases();
    test_worker_matrix();
    test_baseline_save_load();
    test_baseline_version_mismatch_rejected();
    test_fixed_seed_recorded();

    std::cout << "All AST-050 bench corpus selftest passed successfully!" << std::endl;
    return 0;
}
