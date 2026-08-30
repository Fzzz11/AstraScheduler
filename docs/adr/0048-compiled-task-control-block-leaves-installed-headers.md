---
status: accepted
date: 2026-08-30
decisions: [D-170, D-171, D-172, D-173, D-174, D-175]
---

# Compiled task protocol leaves installed headers

v1.1 已把误暴露入口改成 private，但 `TaskSharedStateBase`、`AwaitHandshake` 和 invoker 仍完整出现在 installed headers 里。consumer 翻译单元必须实例化任意 `T` 的 `get()` 与 `co_await`，所以不能靠「只把非模板搬到 src/」把协议移出安装面。

1.2.0 把运行协议编进 compiled TaskControlBlock：安装头只留结果格（TaskHandle 的 private nested 模板）、F 信封和公开 awaitable 的薄包装。mutex、回调、rescheduler、timer、handshake 状态机不进安装头，也不出现在 shared 库默认导出。共享库用 version script / hidden-by-default，只导出 documented allowlist。这不是 documented public 的 major break，也不是继续钉在 1.1.0——exact-version 必须能区分两套安装面。

拒绝整条 await 路径类型擦除、拒绝 `std::function` 收窄 callable、拒绝只改门禁探针或只改导出封闭集两栏。v1.0.0 与 v1.1.0 manifest 不可改写。

决策细节见 [D-170 至 D-175](../../.scratch/astra-scheduler-runtime/decision-log.md)。
