# AST-002 — 建立 compiled library 与可安装 CMake package

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-110, R-111)
Milestone: Phase 0
Blocked by: AST-001
Status: done
Claimed by: CodeBuddy agent (2026-08-27)

## Rules and decisions

- R-110 [primary] — CMake package 隐藏实现并验证独立consumer；source: D-167, D-145
- R-111 [supporting] — Supported Configuration 仅限 64-bit Linux；source: D-167

## What to build

在WSL/Linux中创建 C++20 compiled library、隐藏实现目录、安装并导出 `AstraScheduler::AstraScheduler`，加入仓库外独立 Linux consumer 测试。

## Invariants

- `[R-110]` public headers必须仅安装于include/astra且不泄漏internal/第三方依赖，CMake target声明cxx_std_20并导出AstraSchedulerConfig.cmake/version file；tests/examples/benchmarks/tools由ASTRA_BUILD_*控制且consumer默认不下载其依赖；static/shared共用语义/tests，public symbol经export macro控制，internal symbol hidden，独立find_package/link/run smoke必须通过；不支持-fno-exceptions，core不要求RTTI。 例外边界：warnings-as-errors、sanitizer与内部编译选项不传播给consumer。
- `[R-111]` AstraScheduler的Supported Configuration必须仅包含64-bit Linux：Tier-1为Linux x86_64 GCC13+/Clang17+，Tier-2为native Linux AArch64 GCC/Clang weak-memory验证；Windows/MSVC、macOS、其他非Linux OS与32-bit目标必须标记为unsupported且不得进入release gate、package支持声明或性能/正确性承诺，偶然编译成功不得升级支持状态。 例外边界：compiled CMake package由R-110约束，single implementation instance由R-107约束；Linux atomic能力不足时按R-101报告Locked fallback而不改变平台支持状态。

## Test-first seam

- Public seam: Phase0、install/export、static/shared release package。；Linux build/install/release、CI与package metadata。
- RED evidence: 先在WSL写独立Linux consumer的configure/build测试，证明未安装、泄漏私有include或错误声明非Linux支持时失败。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-110]` 安装目录可被独立最小工程消费，consumer compile line不包含项目内部依赖或强制诊断选项。
- [x] `[R-111]` C++20 compiled static默认target与可选shared target均可在Linux完成install/consume smoke；完整Linux Tier matrix与single implementation instance验证留给AST-052/AST-053。

## Out of scope

- 不实现 Runtime 调度行为；仅允许为编译、安装和测试建立最小私有 seam。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-110, R-111
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-167, D-145
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-27；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-110 | `tools/check_cmake_package.py::R110PackageConsumerGates` — public headers 仅安装于 include/astra 且无 .cpp 泄漏（允许 include/astra\|lib\|bin 布局）；导出 AstraSchedulerConfig.cmake/Targets/Version 文件（含 `AstraScheduler::AstraScheduler`）；仓库外独立 consumer static+shared find_package/link/run smoke；consumer compile_commands 无 -Werror/-fsanitize=/src 泄漏且为 C++20；shared 库 `nm -D` 含 export 探针、不含 hidden 符号；public header 拒绝 -fno-exceptions；`CMakeLists.txt` 用 PRIVATE 诊断选项 + `cxx_std_20` PUBLIC + `$<INSTALL_INTERFACE:include>` | `FAILED (errors=2)`：`FileNotFoundError: required CMake project is missing: .../CMakeLists.txt`（未建立 package 即 RED） | `Ran 14 tests ... OK` |
| R-111 | `R111LinuxOnlyVariantGates` — 默认构建仅产 `libAstraScheduler.a`（无 .so），`BUILD_SHARED_LIBS=ON` 产 .so；install 树无 .dll/.lib/.exe；`-U__linux__` 与 `-m32` include public header 均编译失败（非 Linux/32-bit 负向用例）；CMake configure 边界 `NOT UNIX OR NOT CMAKE_SIZEOF_VOID_P != 8` 即 FATAL_ERROR；static 与 shared 变体共用 in-tree ctest | 同上 | `Ran 14 tests ... OK` |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 14 tests in 9.8s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake -S . -B build/wsl-gcc-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 1`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 /mnt/c/Users/fzt/.agents/skills/decision-ledger/scripts/validate_traceability.py --ledger .scratch/astra-scheduler-runtime/decision-log.md --spec .scratch/astra-scheduler-runtime/spec.md --tickets-dir .scratch/astra-scheduler-runtime/issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `929b69f`，改动为工作区未提交文件）：已处理必须项——CMake configure 边界新增 64-bit Linux guard（R-111 CMake 层强制）；`export.hpp` 改为 `__linux__` + `__SIZEOF_POINTER__` 检查（原 `__GNUC__` 判定对 macOS Clang/MinGW 过宽）；static/shared 两变体统一跑 in-tree ctest（R-110 共用 tests）；新增 `-U__linux__`/`-m32` 负向编译用例补足 RED 负面证据；构建流程提取 `_cmake_build_install` 去重；`ASTRA_BUILD_*` 注释改为准确描述；`CMAKE_INSTALL_LIBDIR` 显式固定为 `lib` 消除 multiarch 耦合。修复后从 WSL 复验 14 项门禁全部通过。
- 记录边界：`astra::detail::package_probe_exported` 是验证 export macro 机制的私有 seam（0.x 期间无 ABI 承诺，AST-003 的版本查询 API 将成为首个正式导出符号并取代该探针）；consumer smoke 不调用库符号（Phase 0 无 public API 可调，static 归档提取为空是必然结果，链接器仍校验 archive 有效性；AST-003 起 consumer 将真正调用 `library_version()`）；install 树无 Windows 产物的断言属防御性审计，非 Linux 支持的主证据来自 configure guard 与负向编译用例。
- 编译环境：WSL GCC 13.1.0（`gcc/g++` 默认已切换，R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。
