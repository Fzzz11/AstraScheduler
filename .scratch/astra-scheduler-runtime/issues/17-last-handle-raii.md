# AST-017 — 实现最后非 Worker Handle 的 noexcept 同步 RAII

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-103, R-105)
Milestone: v0.1.0
Blocked by: AST-007, AST-014, AST-015
Status: done
Claimed by: Antigravity agent (2026-08-28)

## Rules and decisions

- R-103 [primary] — 只有最后一个非Worker Scheduler Handle释放触发同步RAII；source: D-014, D-017, D-018, D-155
- R-105 [primary] — 最后非Worker Handle析构是noexcept同步完成边界；source: D-014, D-155

## What to build

仅最后一个非 Worker Scheduler Handle 释放触发 Graceful fallback，并作为 `noexcept` 同步完成/回收边界；非最后副本释放不关停。

## Invariants

- `[R-103]` Scheduler必须是copyable/movable shared Handle，普通副本销毁不得关停；仅最后一个Handle释放触发RAII，非目标Worker上Running发起Graceful、Stopping保留mode，目标Worker则按R-021/R-022 handoff；空/moved-from操作除valid/destruction外抛logic_error。 例外边界：已Stopped最后释放只回收；显式shutdown可先完成。
- `[R-105]` 最后一个Scheduler Handle在非目标Worker释放时，析构必须noexcept并等待Drain Closure、全部Worker join与Stopped真实发布，允许无界阻塞且不得detach或伪造完成。 例外边界：目标Worker最后释放使用R-021/R-022的异步handoff。

## Test-first seam

- Public seam: Scheduler shared Handle lifetime与RAII策略选择。；R-103选择的非Worker RAII路径。
- RED evidence: 先覆盖多 Handle 释放顺序、最后释放阻塞到真实完成、异常任务和析构路径不传播异常。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-103]` 销毁一个非最后副本不改变status/admission，最后释放才按caller选择RAII或handoff。
- [x] `[R-105]` 析构返回后无Worker访问Runtime，不合作任务保持析构未返回。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-103, R-105
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-014, D-017, D-018, D-155
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Done（2026-08-28；全部验证命令来自 WSL Linux）。

### Rule evidence

| Rule | Test or verification | RED evidence | GREEN result |
|---|---|---|---|
| R-103 | `tests/test_last_handle_raii.cpp::test_R103_non_last_handle_destruction_does_not_shutdown`、`test_R103_empty_moved_from_throws_logic_error` — 证明 `Scheduler` 作为 shared Handle 副本销毁不改变状态与准入；moved-from 空 Handle 调用抛出 `std::logic_error`。 | 运行期 RED：副本销毁误触发停机或 moved-from Handle 未抛出 logic_error。 | 仅最后 Handle 释放才触发关停，空 Handle 行为安全一致。 |
| R-105 | `tests/test_last_handle_raii.cpp::test_R105_last_non_worker_handle_destructor_is_synchronous_noexcept` — 证明最后一个非 Worker Handle 在析构时严格执行 `noexcept` 同步 Graceful 关停等待 Drain Closure 与所有 Worker 线程 join，不向外泄漏异常。 | 运行期 RED：析构提前返回或任务异常导致析构抛出 std::terminate。 | `~Scheduler() noexcept` 完整同步等待 Drain Closure 与全部 Worker join。 |

### Verification commands

- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"` → `Ran 32 tests in 36.206s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"` → `Ran 15 tests in 0.194s ... OK`
- `wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && cmake --build build/wsl-gcc-debug && ctest --test-dir build/wsl-gcc-debug --output-on-failure"` → `100% tests passed, 0 tests failed out of 15`
- `python "C:\Users\fzt\.gemini\config\skills\decision-ledger\scripts\validate_traceability.py" --ledger "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\decision-log.md" --spec "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\spec.md" --tickets-dir "D:\code\cppStudy\AstraScheduler\.scratch\astra-scheduler-runtime\issues"` → `Traceability valid: decisions=168, rules=105, tickets=55, covered_rules=105`

### Review record

- 两轴 code-review（Standards/Spec，固定基线=commit `613a54a`）：
  - Standards 轴：`Scheduler::~Scheduler() noexcept` 显式标记并根据 `impl_.use_count() == 1` 判断最后 Handle；优雅处理启动栅栏竞态；无任何异常外泄风险。
  - Spec 轴：R-103（最后非 Worker Handle 释放触发 RAII）、R-105（最后非 Worker Handle 析构是 noexcept 同步完成边界）100% 满足 Approved Spec。
- 编译环境：WSL GCC 13.1.0（R-111 Tier-1）、CMake 3.28.6、Python 3.8.10。

