# AST-053 — 执行 Linux sanitizer、weak-memory 与 package consumer hardening

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-111, R-093, R-094, R-110)
Milestone: v0.9.0
Blocked by: AST-002, AST-003, AST-047, AST-051, AST-052
Status: in-progress
Claimed by: agent

## Rules and decisions

- R-111 [supporting] — Supported Configuration 仅限 64-bit Linux；source: D-167
- R-093 [supporting] — SemVer保证source/semantic并分离header/library version；source: D-145, D-164
- R-094 [supporting] — Phase 0 至 v1.0 按纵向里程碑交付；source: D-146
- R-110 [supporting] — CMake package 隐藏实现并验证独立consumer；source: D-167, D-145

## What to build

运行Tier-1 Linux x86_64 GCC/Clang的ASan/UBSan/TSan证据、Tier-2 native Linux AArch64 weak-memory stress、独立Linux package consumer与header/library version矩阵，并审计不存在Windows/MSVC release支持声明。

## Invariants

- `[R-111]` AstraScheduler的Supported Configuration必须仅包含64-bit Linux：Tier-1为Linux x86_64 GCC13+/Clang17+，Tier-2为native Linux AArch64 GCC/Clang weak-memory验证；Windows/MSVC、macOS、其他非Linux OS与32-bit目标必须标记为unsupported且不得进入release gate、package支持声明或性能/正确性承诺，偶然编译成功不得升级支持状态。 例外边界：compiled CMake package由R-110约束，single implementation instance由R-107约束；Linux atomic能力不足时按R-101报告Locked fallback而不改变平台支持状态。
- `[R-093]` 0.x minor可经decision+migration产生breaking change而patch不得计划性breaking，v1起documented source/observable semantics按SemVer；不保证跨版本/toolchain ABI。必须公开可比较的 `Version{uint32 major,minor,patch}`、ASTRA_VERSION_MAJOR/MINOR/PATCH、constexpr header_version()及无分配/无锁/不初始化Runtime的library_version()/library_version_string()，后者string_view指向进程期静态规范SemVer文本；schema版本独立，CMake exact-version检查为主要mismatch边界。 例外边界：runtime查询不使错误header/binary组合成为受支持。
- `[R-094]` 路线必须依次以Phase0 scaffold、v0.1 Global Runtime+Task/lifecycle、v0.2 locked WS、v0.3 Chase-Lev、v0.4 DAG、v0.5 Coroutine+Timer、v0.6 Priority+Deadline、v0.7 Observability、v0.8 Benchmark、v0.9 hardening、v1 stable source API交付，每tag满足approved-rule测试、Tier-1 build、并发证据、docs/package/schema/benchmark gates。 例外边界：private seam可提前，未定public语义不得提前暴露。
- `[R-110]` public headers必须仅安装于include/astra且不泄漏internal/第三方依赖，CMake target声明cxx_std_20并导出AstraSchedulerConfig.cmake/version file；tests/examples/benchmarks/tools由ASTRA_BUILD_*控制且consumer默认不下载其依赖；static/shared共用语义/tests，public symbol经export macro控制，internal symbol hidden，独立find_package/link/run smoke必须通过；不支持-fno-exceptions，core不要求RTTI。 例外边界：warnings-as-errors、sanitizer与内部编译选项不传播给consumer。

## Test-first seam

- Public seam: build/install/release与process-wide Reaper/ID/metrics保证。；release、shared package、artifact与consumer诊断。；release规划与后续to-tickets。；Phase0、install/export、static/shared release package。
- RED evidence: 先建立会在缺失Linux Tier结果、出现Windows/MSVC release job、sanitizer失败、私有header泄漏或Linux consumer不能链接时失败的release job。
- 本集成 Ticket 没有 primary owner；验证只为 supporting 规则补充发布证据，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-111]` Linux x86_64 GCC/Clang与native Linux AArch64证据齐全，release/package metadata不声明任何非Linux支持。
- [ ] `[R-093]` 同一安装header/library版本一致，查询不启动Reaper或分配。
- [ ] `[R-094]` 每个实现Ticket有目标版本且每个tag可独立构建运行。
- [ ] `[R-110]` 安装目录可被独立最小工程消费，consumer compile line不包含项目内部依赖或强制诊断选项。

## Out of scope

- 不承诺跨 compiler、stdlib、CRT 或版本的 binary ABI，也不把仅能编译的平台标为 Supported。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-111, R-093, R-094, R-110
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-167, D-145, D-164, D-146
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:（in-progress）已交付：真实 sanitizer 构建开关（此前 ASTRA_ENABLE_SANITIZERS 为无消费变量的空操作，历次"ASan 通过"实为普通构建——本次修正）；tools/check_hardening.py 证据编排（ASan+UBSan / TSan 双套件全新构建+全量 ctest，输出 docs/hardening-evidence.json）；tests/test_weak_memory_stress.cpp weak-memory 压力载体（TSan 下通过，为 Tier-2 native AArch64 提供同一测试）。
  TSan 首轮发现 8 个失败测试（真实竞争家族）：~Impl 非原子读 handoff_dispatched（已修复：atomic<bool>+acquire/release，~Impl:583 与 ~Scheduler:2422 报告清除）；剩余已知问题 chase_lev_deque.hpp grow() 与 steal 读 cells_ 的发布竞争（涉及 R-067/R-068 growth 发布协议设计）、graph.cpp GraphRun::wait 谓词竞争、reaper handoff 深层生命周期（coroutine_resume_handshake 下 SEGV）——待后续继续修复。
  Tier-2 AArch64：本环境无 native 硬件，stress 载体已交付并显式记录 deferred（D-167 要求 native 证据，不伪造）。
