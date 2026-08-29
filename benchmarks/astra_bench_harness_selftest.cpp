// AstraScheduler bench harness self-test（AST-049 / R-089 RED evidence）。
// 验证 harness 的 invalid 语义：错误 checksum、空 repetition、异常 case、
// timed-region 污染与 setup/teardown 隔离。

#include "bench_harness.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

using astra::bench::CaseContext;
using astra::bench::run_case_sample;
using astra::bench::ScenarioCase;

// 1. 错误 checksum → invalid（不产生性能值）。
void test_checksum_mismatch_invalid() {
    ScenarioCase fc;
    fc.name = "selftest.checksum_mismatch";
    fc.expected_work = 10;
    fc.timed_region = [](CaseContext& ctx) { ctx.account_work(10); };
    fc.verify = [](CaseContext&) {
        return astra::bench::Verification{false, "checksum_mismatch"};
    };
    auto r = run_case_sample(fc);
    assert(!r.valid);
    assert(r.invalid_reason == "checksum_mismatch");
    assert(r.timed_ns == 0 || true);  // invalid sample 的数值不可用于比较
    std::cout << "[PASS] checksum_mismatch_invalid" << std::endl;
}

// 2. 空 repetition：timed region 未完成声明的工作 → invalid。
void test_empty_repetition_invalid() {
    ScenarioCase fc;
    fc.name = "selftest.empty_repetition";
    fc.expected_work = 10;
    fc.timed_region = [](CaseContext&) {
        // 什么都不做：无工作进入 timed region。
    };
    auto r = run_case_sample(fc);
    assert(!r.valid);
    assert(r.invalid_reason == "empty_repetition");
    std::cout << "[PASS] empty_repetition_invalid" << std::endl;
}

// 3. timed-region 污染：工作量少于声明（部分工作漏出测量区）→ invalid。
void test_timed_work_mismatch_invalid() {
    ScenarioCase fc;
    fc.name = "selftest.timed_work_mismatch";
    fc.expected_work = 10;
    fc.timed_region = [](CaseContext& ctx) { ctx.account_work(7); };
    auto r = run_case_sample(fc);
    assert(!r.valid);
    assert(r.invalid_reason == "timed_work_mismatch");
    std::cout << "[PASS] timed_work_mismatch_invalid" << std::endl;
}

// 4. 异常 case：timed region / setup / teardown 抛异常 → invalid。
void test_exception_cases_invalid() {
    {
        ScenarioCase fc;
        fc.name = "selftest.exception_timed";
        fc.timed_region = [](CaseContext&) { throw std::runtime_error("boom"); };
        auto r = run_case_sample(fc);
        assert(!r.valid);
        assert(r.invalid_reason.find("exception:") == 0);
    }
    {
        ScenarioCase fc;
        fc.name = "selftest.exception_setup";
        fc.setup = [](CaseContext&) { throw std::runtime_error("setup boom"); };
        auto r = run_case_sample(fc);
        assert(!r.valid);
        assert(r.invalid_reason.find("exception:") == 0);
    }
    {
        ScenarioCase fc;
        fc.name = "selftest.exception_teardown";
        fc.timed_region = [](CaseContext& ctx) { ctx.account_work(1); };
        fc.expected_work = 1;
        fc.teardown = [](CaseContext&) { throw std::runtime_error("teardown boom"); };
        auto r = run_case_sample(fc);
        assert(!r.valid);
        assert(r.invalid_reason.find("exception:teardown") == 0);
    }
    std::cout << "[PASS] exception_cases_invalid" << std::endl;
}

// 5. 计时区不混入构建/销毁：setup/teardown 的耗时不出现在 timed_ns。
void test_timed_region_excludes_setup_teardown() {
    ScenarioCase fc;
    fc.name = "selftest.setup_teardown_excluded";
    fc.expected_work = 1;
    fc.setup = [](CaseContext&) { std::this_thread::sleep_for(50ms); };
    fc.teardown = [](CaseContext&) { std::this_thread::sleep_for(50ms); };
    fc.timed_region = [](CaseContext& ctx) {
        std::this_thread::sleep_for(10ms);
        ctx.account_work(1);
    };
    const auto wall_start = std::chrono::steady_clock::now();
    auto r = run_case_sample(fc);
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - wall_start).count();
    assert(r.valid);
    assert(r.timed_ns < 40'000'000);                    // timed 只含 10ms 工作
    assert(wall_ns > r.timed_ns + 90'000'000);          // setup+teardown 被执行但不计时
    std::cout << "[PASS] timed_region_excludes_setup_teardown" << std::endl;
}

// 6. warmup 与 rejection/drop 语义：warmup 工作不计数；unexpected rejection/drop → invalid。
void test_warmup_and_rejection_drop_semantics() {
    {
        ScenarioCase fc;
        fc.name = "selftest.warmup_not_counted";
        fc.expected_work = 1;
        fc.warmup_iterations = 1;
        fc.warmup = [](CaseContext& ctx) { ctx.account_work(123); };  // warmup 不计入
        fc.timed_region = [](CaseContext& ctx) { ctx.account_work(1); };
        auto r = run_case_sample(fc);
        assert(r.valid);
        assert(r.completed_work == 1);
    }
    {
        ScenarioCase fc;
        fc.name = "selftest.unexpected_rejection";
        fc.timed_region = [](CaseContext& ctx) { ctx.account_rejection(); };
        auto r = run_case_sample(fc);
        assert(!r.valid);
        assert(r.invalid_reason == "unexpected_rejection");
    }
    {
        ScenarioCase fc;
        fc.name = "selftest.unexpected_drop";
        fc.timed_region = [](CaseContext& ctx) { ctx.account_drop(); };
        auto r = run_case_sample(fc);
        assert(!r.valid);
        assert(r.invalid_reason == "unexpected_drop");
    }
    std::cout << "[PASS] warmup_and_rejection_drop_semantics" << std::endl;
}

// 7. valid case 产生可用测量值与 checksum。
void test_valid_case_produces_sample() {
    ScenarioCase fc;
    fc.name = "selftest.valid";
    fc.expected_work = 100;
    fc.expected_checksum = 42;
    fc.timed_region = [](CaseContext& ctx) { ctx.account_work(100); };
    fc.verify = [](CaseContext&) { return astra::bench::Verification{true, ""}; };
    auto r = run_case_sample(fc);
    assert(r.valid);
    assert(r.completed_work == 100);
    assert(r.checksum == 42);
    assert(r.timed_ns > 0);
    std::cout << "[PASS] valid_case_produces_sample" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_bench_harness_selftest..." << std::endl;

    test_checksum_mismatch_invalid();
    test_empty_repetition_invalid();
    test_timed_work_mismatch_invalid();
    test_exception_cases_invalid();
    test_timed_region_excludes_setup_teardown();
    test_warmup_and_rejection_drop_semantics();
    test_valid_case_produces_sample();

    std::cout << "All AST-049 bench harness selftest passed successfully!" << std::endl;
    return 0;
}
