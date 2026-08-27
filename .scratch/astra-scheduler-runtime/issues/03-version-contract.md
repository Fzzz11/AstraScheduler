# AST-003 — 提供 header/library 版本查询与 mismatch 诊断

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-093)
Milestone: Phase 0
Blocked by: AST-002
Status: done
Claimed by: CodeBuddy agent (2026-08-27)

## Rules and decisions

- R-093 [primary] — SemVer保证source/semantic并分离header/library version；source: D-145, D-164

## What to build

实现 SemVer header 宏、`header_version()`、无副作用 `library_version()`/`library_version_string()`，定义 header/library mismatch 的可诊断行为。

## Invariants

- `[R-093]` 0.x minor可经decision+migration产生breaking change而patch不得计划性breaking，v1起documented source/observable semantics按SemVer；不保证跨版本/toolchain ABI。必须公开可比较的 `Version{uint32 major,minor,patch}`、ASTRA_VERSION_MAJOR/MINOR/PATCH、constexpr header_version()及无分配/无锁/不初始化Runtime的library_version()/library_version_string()，后者string_view指向进程期静态规范SemVer文本；schema版本独立，CMake exact-version检查为主要mismatch边界。 例外边界：runtime查询不使错误header/binary组合成为受支持。

## Test-first seam

- Public seam: release、shared package、artifact与consumer诊断。
- RED evidence: 先写版本相等、查询不启动 Runtime/Reaper、模拟 mismatch 可被发现的 consumer 测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-093]` 同一安装header/library版本一致，查询不启动Reaper或分配。

## Out of scope

- 不实现 Runtime 调度行为；仅允许为编译、安装和测试建立最小私有 seam。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-093
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-145, D-164
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-27；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-093 | `tools/check_cmake_package.py::R093VersionContractGates` — 单一版本源一致性（installed `version.hpp` 宏、CMake version file `PACKAGE_VERSION`、consumer 模板 exact-version 钉住值均等于 `project(AstraScheduler VERSION 0.1.0)`）；独立 consumer（static+shared）运行期断言 `header_version()==library_version()==ASTRA_VERSION_*`、查询前后 `/proc/self/status` 线程数不变（不启动 Reaper/线程）、`library_version_string()` 两次调用地址与内容稳定且文本三元组与 `library_version()` 一致、三个查询 `noexcept`/`Version` 可平凡复制/`header_version()` 常量求值均为编译期 static_assert；`find_package` 请求同 major 更旧版本（0.0.0 对 0.1.0 安装）必须在 configure 边界失败（CMake exact-version 主要 mismatch 边界）；手工链接（绕过 package）+ 篡改 `ASTRA_VERSION_MINOR` 的 header 副本编译的 consumer 运行期发现 mismatch 并以诊断退出；`tests/selftest.cpp` 提供 in-tree 等价断言；`R110PackageConsumerGates` 的 `nm -D` 断言共享库仅导出 `_ZN5astra15library_versionEv`/`_ZN5astra22library_version_stringEv`（版本查询 API 取代 AST-002 探针成为导出面） | 总体 RED：`Ran 0 tests ... FAILED (errors=3)`，`fatal error: astra/version.hpp: No such file or directory`（selftest/consumer 编译失败，版本契约不存在）。聚焦 RED（exact-version 边界缺失）：纯净基线 `91712ff` 安装 0.1.0，`find_package(AstraScheduler 0.0.9)` configure 被错误接受（`PROBE_CONFIGURE=ACCEPTED`） | `Ran 18 tests in 7.311s ... OK` |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 18 tests in 7.311s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake -S . -B build/wsl-gcc-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 1`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 /mnt/c/Users/fzt/.agents/skills/decision-ledger/scripts/validate_traceability.py --ledger .scratch/astra-scheduler-runtime/decision-log.md --spec .scratch/astra-scheduler-runtime/spec.md --tickets-dir .scratch/astra-scheduler-runtime/issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `91712ff`，改动为工作区未提交文件）：Standards 轴无硬违规；已处理建议项——`R093VersionContractGates` 等三个测试类改用模块级共享构建缓存+`atexit` 清理（每轮运行 3 次完整 build/install 降为 1 次，`Ran 18 tests` 20.5s→7.3s）；consumer 模板 `0.1.0` 钉住值纳入单一版本源一致性断言（消除双处维护漂移风险，升级时门禁给出明确失败信息而非模糊的 find_package 失败）；`version.cpp` 字符串化宏移出匿名 namespace（宏无作用域）；selftest static_assert 补 message；`_project_version()` 改 `namedtuple`。保留判断项：selftest 与 consumer 的编译期契约断言双份（in-tree 与 installed 是两个独立证据面，各自携带契约声明属策略选择）；mismatch 诊断 printf 与 Python 断言的输出耦合（断言诊断输出本身即 gate 目标）。
- Spec 轴：无缺失项、无 scope creep；`Version`/宏/三查询与 D-164 代码块逐字一致，`ASTRA_EXPORT` 声明符合 D-145 导出纪律。验证手段边界记录：「不分配」以类型级证明（trivially-copyable 按值返回 + 非拥有 `string_view` + `noexcept` + 静态存储地址/内容稳定性实测）为证据，未做 malloc 计量实测；「Finalization 前后结果相同」因 Runtime 尚不存在留待后续 Ticket（D-164 该不变量随 Runtime 交付补测）。
- 实现发现（CMake 语义）：`COMPATIBILITY ExactVersion` 的兼容判定是归一化三组件字符串精确相等（请求缺组件即拒绝），consumer 必须钉住完整 `major.minor.patch`；原 `SameMajorVersion` 的实际洞是接受同 major 的更旧版本（0.x minor 可含 breaking change 的错误组合），已由负向用例覆盖并改为 `ExactVersion`。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

