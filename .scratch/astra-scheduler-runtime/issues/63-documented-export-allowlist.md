# AST-063 — shared 库只导出 documented allowlist

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-123)
Milestone: v1.2.0
Blocked by: AST-062
Status: done
Claimed by: agent

## Rules and decisions

- R-123 [primary] — 共享库默认隐藏并只导出 documented allowlist；source: D-175, D-170
- R-110 [supporting] — 独立 CMake consumer 仍能链接安装的 public target；source: D-145, D-167
- R-113 [supporting] — v1.2.0 semantic manifest 与导出 allowlist 对齐；source: D-169, D-170

## What to build

Linux shared `libAstraScheduler.so` hidden-by-default（version script 或等价可见性控制），只导出与 v1.2.0 semantic manifest / `public_contract.cpp` 对齐的 documented 符号。`astra::detail` 协议类型、`record_metrics_*`、handshake 与 TaskControlBlock mutator 不得出现在默认 dynsym。package gate 封闭集等于该 allowlist，不得再要求 detail 协议符号存在。安装头若仍引用被隐藏的协议符号，必须链接失败，不得把符号加回导出表。

## Invariants

- 不承诺跨 toolchain ABI（R-093）。
- static install 无 dynsym 面，但仍受 R-118 至 R-121 的安装头约束。
- internal tests 不得依赖已隐藏的 public dynsym 去测协议。
- v1.0.0 与 v1.1.0 manifest 仍不可改写。

## Test-first seam

- Public seam: `nm -D --defined-only`、独立 consumer 链接、package consumer 封闭集。
- RED evidence: nm 仍见到协议 detail 导出时 package gate 失败；独立 consumer 不能链接 documented 符号时失败。

## Acceptance criteria

- [x] `[R-123]` shared 库默认 dynsym 不含协议 detail 类型、`record_metrics_*`、handshake 与 TCB mutator。
- [x] `[R-123]` package 封闭集等于 documented allowlist，不再要求 detail 协议符号。
- [x] `[R-123]` 残留 header 引用隐藏符号时链接失败，而不是把符号加回导出表。
- [x] `[R-110]` 独立 consumer 仍能 find_package / 链接 / 运行 documented 入口。
- [x] `[R-113]` v1.2.0 semantic manifest 与导出 allowlist 一致，且未改写旧 manifest。

## Out of scope

- 不承诺跨版本/跨工具链 C++ ABI。
- 不列出本票之外的新 public 符号。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-123
- Decisions: `.scratch/astra-scheduler-runtime/decision-log.md` — D-175, D-170
- ADR: `docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md`
- Verification: WSL shared `libAstraScheduler.so` 使用 `-fvisibility=hidden` + `cmake/AstraScheduler.version`；dynsym 不含 `TaskSharedStateBase` / `record_metrics_first_start` / handshake 完成型 / `add_completion_callback_internal`；package 封闭集改为当前 documented + header 模板桥接（`tcb_*`、`wrap_submitted_invoker`、`record_wait_call` / `record_self_wait_rejection` / `record_metrics_submission_attempt`），不再要求旧 `TaskSharedStateBase` 协议符号；`check_cmake_package.py` 50/50；v1.0.0/v1.1.0 manifest 未改写。
