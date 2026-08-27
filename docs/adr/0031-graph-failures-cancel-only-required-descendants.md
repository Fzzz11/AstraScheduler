---
status: accepted
date: 2026-08-26
decisions: [D-109, D-110, D-111, D-112, D-113, D-152]
---

# Graph failures cancel only required descendants

TaskGraph是void control-DAG。Edge默认`RequireSuccess`，显式`AfterCompletion`允许cleanup/summary在前驱任意终态后运行；Node Failed/Cancelled只把RequireSuccess descendants传播为Cancelled，independent branch和AfterCompletion continuation继续。全图fail-fast/用户取消必须显式调用`GraphRun::request_cancel()`，其对每Node复用pre-start cancel或Running cooperative stop。

GraphRun发布按NodeId规范排序的不可变GraphReport，保留所有真实Failed exception_ptr和成功/失败/取消计数；状态按Failed > Cancelled > Succeeded聚合，不自动挑一个异常重抛。等待复用caller-relative Helping，但正在执行的Node等待所属GraphRun是必然self-run deadlock，必须在副作用前拒绝。

`get_report()`或`co_await GraphRun`返回report时一次性标记其中全部真实Node exceptions已观察；只做state/wait不算。GraphRun shared state最终释放仍未观察时，按真实Failed Node数量执行与普通Task相同的启用态Metrics/Trace诊断，不为aggregate Failed再制造一个synthetic exception。

决策细节见 [D-109 至 D-113 与 D-152](../../.scratch/astra-scheduler-runtime/decision-log.md)。
