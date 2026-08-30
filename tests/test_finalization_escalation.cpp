#include <astra/error.hpp>
#include <astra/finalization.hpp>
#include <astra/scheduler.hpp>
#include "lifecycle/reaper_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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
// R-034: 显式 Finalization Escalation 覆盖全部未完成 Runtime（含 Pending/Orphan）
// -----------------------------------------------------------------------------
void test_R034_request_immediate_escalation_covers_all_uncompleted() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    // 1. 活动 Scheduler
    astra::Scheduler s1;
    (void)s1.submit([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });

    // 2. 发生 Worker Orphan Handoff 的 Pending Scheduler
    std::atomic<bool> orphan_started{false};
    {
        astra::Scheduler s2;
        (void)s2.submit([s2_copy = s2, &orphan_started]() mutable {
            orphan_started.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            // 内部释放最后 handle 触发 handoff
        });
    }

    while (!orphan_started.load()) {
        std::this_thread::yield();
    }

    auto ctrl = astra::begin_finalization();
    // 显式升级为 Immediate
    ctrl.request_immediate();

    // 升级请求可靠发布且不阻塞，随后 wait() 可完成
    ctrl.wait();

    TEST_ASSERT(s1.status().state == astra::SchedulerState::Stopped);
}

// -----------------------------------------------------------------------------
// R-047: Reaper 控制面不可恢复故障 fail-fast（子进程隔离验证 std::terminate）
// -----------------------------------------------------------------------------
void test_R047_coordinator_fail_fast_terminates() {
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0);

    if (pid == 0) {
        // 子进程内测试控制面故障注入
        astra::detail::ReaperRegistry::instance().reset_for_testing();
        {
            astra::Scheduler s;
            astra::detail::ReaperRegistry::instance().inject_coordinator_failure(true);
            // 等待 coordinator 触发 terminate
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::_Exit(0);  // 若未 terminate 则退出 0
    }

    int status = 0;
    TEST_ASSERT(waitpid(pid, &status, 0) == pid);
    // 必须被 SIGABRT 异常终止，或者非正常退出
    bool terminated = WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT);
    TEST_ASSERT(terminated || (WIFEXITED(status) && WEXITSTATUS(status) != 0));
}

}  // namespace

int main() {
    std::printf("Running astra_finalization_escalation_test...\n");
    test_R034_request_immediate_escalation_covers_all_uncompleted();
    test_R047_coordinator_fail_fast_terminates();
    std::printf("All AST-021 finalization escalation tests passed successfully!\n");
    return 0;
}
