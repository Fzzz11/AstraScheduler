#include <astra/error.hpp>
#include <astra/finalization.hpp>
#include <astra/scheduler.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

namespace {

// -----------------------------------------------------------------------------
// R-035, R-044, R-045: 编译期类型萃取、构造限制与异常规格验证
// -----------------------------------------------------------------------------
void test_R035_R044_R045_type_traits_and_signatures() {
    // 1. R-035: 控制对象不得公开默认构造
    static_assert(!std::is_default_constructible_v<astra::FinalizationControl>,
                  "FinalizationControl must not be default-constructible");

    // 2. R-045: copy/move/destructor 必须为 noexcept
    static_assert(std::is_nothrow_copy_constructible_v<astra::FinalizationControl>,
                  "FinalizationControl copy ctor must be noexcept");
    static_assert(std::is_nothrow_copy_assignable_v<astra::FinalizationControl>,
                  "FinalizationControl copy assign must be noexcept");
    static_assert(std::is_nothrow_move_constructible_v<astra::FinalizationControl>,
                  "FinalizationControl move ctor must be noexcept");
    static_assert(std::is_nothrow_move_assignable_v<astra::FinalizationControl>,
                  "FinalizationControl move assign must be noexcept");
    static_assert(std::is_nothrow_destructible_v<astra::FinalizationControl>,
                  "FinalizationControl dtor must be noexcept");

    // 3. R-045: begin_finalization() 与 request_immediate() 为 noexcept
    static_assert(noexcept(astra::begin_finalization()),
                  "begin_finalization() must be noexcept");
    static_assert(noexcept(std::declval<const astra::FinalizationControl&>().request_immediate()),
                  "request_immediate() must be noexcept");

    // 4. R-045: wait() 与 wait_for() 不得标记 noexcept（以便承载 Worker 调用的 logic_error）
    static_assert(!noexcept(std::declval<const astra::FinalizationControl&>().wait()),
                  "wait() must not be noexcept");
    static_assert(!noexcept(std::declval<const astra::FinalizationControl&>().wait_for(std::chrono::seconds(1))),
                  "wait_for() must not be noexcept");

    // 5. R-044: FinalizationWaitResult 稳定枚举
    static_assert(static_cast<int>(astra::FinalizationWaitResult::Completed) == 0 ||
                  static_cast<int>(astra::FinalizationWaitResult::Completed) != static_cast<int>(astra::FinalizationWaitResult::TimedOut),
                  "FinalizationWaitResult enum values must be distinct");
}

// -----------------------------------------------------------------------------
// R-035, R-036, R-045: 运行时控制对象有效性与共享 capability 语义
// -----------------------------------------------------------------------------
void test_R035_R036_runtime_control_semantics() {
    auto ctrl1 = astra::begin_finalization();
    {
        auto ctrl2 = ctrl1;
        {
            auto ctrl3 = std::move(ctrl2);
            ctrl3.request_immediate();
            auto res = ctrl3.wait_for(std::chrono::milliseconds(10));
            TEST_ASSERT(res == astra::FinalizationWaitResult::Completed);
        }
    }
    // 全部副本销毁不影响剩余 ctrl1
    ctrl1.request_immediate();
    ctrl1.wait();
}

}  // namespace

int main() {
    std::printf("Running astra_finalization_control_api_test...\n");
    test_R035_R044_R045_type_traits_and_signatures();
    test_R035_R036_runtime_control_semantics();
    std::printf("All AST-018 finalization control API tests passed successfully!\n");
    return 0;
}
