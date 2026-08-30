# AST-058 — 将项目 VERSION 升至 1.2.0 并固定不可改写的旧 manifest

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-122)
Milestone: v1.2.0
Blocked by: AST-057
Status: done
Claimed by: agent

## Rules and decisions

- R-122 [primary] — 协议内收后 VERSION 为 1.2.0 且既有 manifest 不可改写；source: D-171
- R-093 [supporting] — 单一版本源与 exact-version 钉住；source: D-145, D-164
- R-094 [supporting] — 里程碑矩阵列入 v1.2.0；source: D-146
- R-113 [supporting] — 已发布 manifest 不可改写，1.2.0 使用新 semantic manifest；source: D-169, D-170

## What to build

`project(AstraScheduler VERSION …)`、installed `version.hpp` 宏、CMake package version 与 consumer `find_package` 钉住值均为 1.2.0。里程碑矩阵与 release-gate 允许列表列入 v1.2.0。新增 `v1.2.0` semantic API manifest；不得改写 `v1.0.0.json` 与 `v1.1.0.json`。documented public observable semantics 不因版本上调而改变。

## Invariants

- documented public 调用的返回值、异常、取消、等待语义保持不变。
- 钉住 1.1.0 的 find_package 对 1.2.0 安装必须在 configure 失败。
- 不把此次上调当成 documented public 的 major break。

## Test-first seam

- Public seam: CMake project VERSION、consumer exact-version、API freeze manifest、里程碑矩阵。
- RED evidence: VERSION 仍为 1.1.0 时 R-093 一致性失败；缺少 v1.2.0 行时 R-094 失败；改写旧 manifest 使 API freeze 失败。

## Acceptance criteria

- [x] `[R-122]` project VERSION、installed 宏与 package version 均为 1.2.0，且 consumer 钉住 1.2.0。
- [x] `[R-122]` `v1.0.0.json` 与 `v1.1.0.json` 字节不可被本票改写；1.2.0 使用新 semantic manifest。
- [x] `[R-093]` 单一版本源三处一致；钉住 1.1.0 的 find_package 对 1.2.0 安装 configure 失败。
- [x] `[R-094]` 里程碑矩阵与 ALLOWED_MILESTONES 按交付顺序列出 v1.2.0。
- [x] `[R-113]` 旧 release manifest 不可变；新版本独立 manifest 与 project 版本一致。

## Out of scope

- 不移动 TaskControlBlock / handshake / F 信封（R-118 至 R-121）。
- 不改变 shared 导出 allowlist（R-123）。
- 不深化 GraphExecution（R-116）。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-122
- Decisions: `.scratch/astra-scheduler-runtime/decision-log.md` — D-171
- ADR: `docs/adr/0048-compiled-task-control-block-leaves-installed-headers.md`
- Verification: WSL GCC Debug；`project(AstraScheduler VERSION 1.2.0)`、consumer `find_package(AstraScheduler 1.2.0 REQUIRED)`、`tools/api_manifest/v1.2.0.json` 新增且未改写 v1.0.0/v1.1.0；`python3 -X utf8 tools/check_release_gates.py` 15/15；`python3 -X utf8 tools/check_cmake_package.py` 含 R-093 exact-version 50/50。
