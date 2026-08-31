# Chase-Lev ReadyQueues 后续缺口与修复计划

状态：A–E 已按 TDD 分步落地并分别提交（HEAD `1e3d6da`）  
日期：2026-08-31  
对象：`HEAD` 上 AST-074 接线之后的生产路径（含 sanitizer 证据提交 `a7311a4`）

本文核实第三轮审查意见。结论：**所列问题基本都真实存在**；其中安装头字段对 R-118/R-121 的引用过满，其余 P0/P1 按规范成立。修复按下列顺序落地，避免再引入与 owner-only 约束冲突的 Local 回写。

## 1. 核实范围

- **代码**：`src/runtime/ready_queues.{hpp,cpp}`、`src/runtime/scheduler.cpp`、`src/scheduling/chase_lev_deque.hpp`、`include/astra/task_handle.hpp`、`src/runtime/worker_loop.cpp`、`tests/test_chase_lev_ready_queues.cpp`。
- **规范**：R-059、R-063、R-066、R-068、R-101、R-118、R-121；D-098、D-100、D-101；ADR-0028。
- **非目标**：不在本文重开 AST-074 接线是否发生的讨论；不把 TSan 全量 54 与 Reaper/`shared_ptr<Impl>` 生命周期报告算进本批缺口。

## 2. 核实结论

| ID | 审查意见 | 核实 | 说明 |
|---|---|---|---|
| S-P0 / R-P0 | Immediate cleanup 控制线程写 Local bottom | **成立** | `request_shutdown_mode` 在控制线程调用 `cancel_unstarted`；Chase-Lev 路径对 resume 调用 `LocalQueues::push()` → `ChaseLevDeque::push()` |
| S-P1a | `ready_next` / `ready_is_external` 进入安装头 | **现象成立，规则引用过满** | 字段在 `include/astra/task_handle.hpp`；R-118 字面禁止的是完成 TCB/Handshake 类型，现有 encapsulation 门禁不会因此失败 |
| S-P1b / R-P1d | rebase 失败后继续 push | **成立** | `push()` 对 `maybe_quiescent_rebase()` 做 `(void)`，高水位索引继续 `+1` |
| S-P1c | `is_lock_free()` 漏检 maintenance / thief guard | **成立** | 只查 `uint64_t`、`Buffer*`、`T` |
| R-P1b | thief guard 仅 acq/rel，可双向漏看 | **成立** | 两个独立 atomic 上的 release/acquire 不能禁止 owner 的 store-load 重排 |
| — | TSan 注解不能代替 C++ 内存序 | **成立** | `__tsan_acquire/release` 只改检测器视图 |
| 低优 | `IntrusiveFifo` 析构与 move 赋值重复清空 | **成立** | 维护性重复，非正确性缺陷 |

先前把 resume 留在 Local，是为 Immediate 下仍能被取到。核实 worker 循环后：**Immediate 仍会 `claim_global`**，只是 claim 之后对非 resume 走 `cancel_pre_start`。resume 放到 Global FIFO 后仍可被执行。那次“必须写回 Local”的判断过严，且与 R-066 owner-only bottom 冲突。

## 3. 问题描述

### 3.1 P0：控制线程写 owner-only Local bottom

Immediate 升级在控制线程持 `lifecycle_mutex` 时清理未开始任务：

```346:350:src/runtime/scheduler.cpp
                    if (requested_mode == ShutdownMode::Immediate) {
                        {
                            std::lock_guard<std::mutex> lock(lifecycle_mutex);
                            cancel_all_unstarted_tasks_locked();
```

Chase-Lev `LocalQueues::cancel_unstarted` 对每个 band `steal`（非 owner 合法），resume 却再走 `push()`：

```302:305:src/runtime/ready_queues.cpp
        for (auto& resume : retained_resumes) {
            const Priority resume_priority = resume.invoker->priority();
            if (!push(resume, resume_priority) && resume.invoker) {
```

`LocalQueues::push()` 的 Chase-Lev 分支调用 `ChaseLevDeque::push()`，只允许所属 Worker 写 bottom（R-066）。与 owner worker 并发时破坏单 writer 前提，可损坏 deque。

### 3.2 P1：调度链接字段在安装头

`TaskInvokerBase` 位于 `include/astra/task_handle.hpp`，带 `ready_next` 与 `ready_is_external`。这是 Ready Queue 侵入式协议，不是 F 信封。独立 consumer 能看见并改写。应迁到编译库内部节点（TCB 或 src 内调度记录），与 D-100 / R-118 的封装方向一致。

### 3.3 P1：高水位 rebase 失败仍前进索引

```238:240:src/scheduling/chase_lev_deque.hpp
        if (b >= kDefaultRebaseHighWatermark || t >= kDefaultRebaseHighWatermark) {
            (void)maybe_quiescent_rebase();
```

`maybe_quiescent_rebase()` 在快照 `vector` 分配失败时返回 false。调用方忽略返回值后仍 `store_cell` 并 `bottom+1`。水位为 `2^58`，一次失败不会立刻 UINT64 wrap，但不满足 R-068 / D-101：高水位必须成功 rebase，否则该 Ready 回退 Global，不得继续累加索引。

### 3.4 P1：maintenance / active-thief 握手偏弱

当前协议：

- owner：`maintenance_.store(true, release)`，再 `active_thieves_.load(acquire)`
- thief：对 `maintenance_` 做 acquire 双检，中间 `fetch_add(acq_rel)`

两个不同对象上的 release/acquire **不能**禁止 owner 把 load(thieves) 排到 store(maintenance) 之前（store-load）。可同时出现：thief 第二次看见 `maintenance==false` 并进入 steal，owner 看见 `active_thieves==0` 并 rebase。D-101 要求设置 maintenance 后不漏新 entrant，需要全序握手（seq_cst 或等价 fence）。TSan 注解放在 cell buffer 上，不修复这条序。

`is_lock_free()` 也未把 `atomic<bool> maintenance_`、`atomic<uint32_t> active_thieves_` 算进 R-101 探针。Tier-1 x86_64 上二者通常仍 lock-free，现网 capability 多半不会报错，但探针不完整。

### 3.5 低优：IntrusiveFifo 清空逻辑重复

析构与 `operator=(IntrusiveFifo&&)` 各有一段相同的 `while (head) delete`。抽 `clear()` 即可。

## 4. 修复计划

顺序：先消除并发正确性（P0 + rebase 失败回退 + seq_cst 握手），再收封装与探针，最后做局部去重。每步先改测试再改实现；验证必须来自 WSL 独立 build 目录。

### 步骤 A — Immediate cleanup 不再写 Local bottom（P0）

- Chase-Lev `cancel_unstarted`：`steal` 后，普通任务仍 `cancel_pre_start`；resume **只**经现有 `resumes` 向量交给 `ReadyQueues::cancel_unstarted` 的 Global intrusive FIFO，禁止 `LocalQueues::push()`。
- 加锁 Local 路径可继续在持 `locked_mutex` 下保留 resume（该路径本就不是 owner-only deque）。
- 改测试：`test_immediate_cleanup_keeps_resume_on_local` 改为断言 resume 在 Global、Local 为空、resume 未被预取消；必要时加一条“cleanup 线程不得调用 Chase-Lev push”的行为测试（通过 Global 可见性观察，不测私有方法）。
- Worker 在 Immediate 下仍 `claim_global`，resume 可执行；与 R-059 例外一致。

### 步骤 B — rebase 失败则 `push()` 返回 false（P1）

- 高水位：`maybe_quiescent_rebase()` 失败则 `push()` 立即 `return false`，由 `ReadyQueues::publish` 走 allocation-free Global fallback。
- 空队列 pop 路径的 rebase 失败只影响索引维护，不得伪造 Empty/Success。
- 测试：注入 rebase/快照失败（或把水位降到可测范围后让 rebase 失败），断言 `push` 为 false，且既有 growth-failure 回退测试仍通过。

### 步骤 C — maintenance / thief 改为 seq_cst 握手（P1）

- thief：`maintenance_` seq_cst 加载；未置位则 `active_thieves_.fetch_add(seq_cst)`；再 seq_cst 加载 `maintenance_`，已置位则 `fetch_sub` 并返回 Retry。
- owner rebase：`maintenance_.store(true, seq_cst)`，再循环 `active_thieves_.load(seq_cst)==0`；结束后 `store(false, seq_cst)`。
- 不把 TSan 注解当成内存序修复；注解可保留为检测器提示。
- `is_lock_free()` 增加 `std::atomic<bool>` 与 `std::atomic<std::uint32_t>`。
- 测试：现有 `set_maintenance_for_testing` / steal Retry 仍通过；capability 期望值改为与完整探针一致。

### 步骤 D — 侵入式链接离开安装头（P1）

- 从 `TaskInvokerBase` 删除 `ready_next`、`ready_is_external`。
- 在 `src/` 增加仅实现可见的调度节点（优先挂在已编译的 TCB 或 `ReadyQueues` 内部记录上），Local cell 与 Global FIFO 只存该内部指针。
- 不得再引入每任务额外堆节点来“绕过”封装（那会退回已否决的 `ChaseNode`）。
- 增加/确认 encapsulation 负向探针：独立 consumer 翻译单元看不到这两个字段。
- 内部测试继续覆盖 growth fallback、steal、Immediate resume。

### 步骤 E — IntrusiveFifo::clear()（低优）

- 析构与 move assignment 共用私有 `clear()`。无行为变化。

## 5. 验证

每完成 A–D 中一步，在 WSL 跑：

```text
cmake --build build/wsl-gcc-debug -j2 && ctest --test-dir build/wsl-gcc-debug --output-on-failure
```

A+B+C 全部完成后补：

```text
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/wsl-gcc-asan --output-on-failure

TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/wsl-gcc-tsan -R 'astra_chase_lev_|astra_immediate_escalation|astra_steal_round|astra_priority_bands|astra_weak_memory_stress|astra_coroutine_frame_lifetime' --output-on-failure
```

证据写入 AST-074 或后续 Ticket 的 Verification，不在聊天里宣称完成。

## 6. 明确不做

本批 A–E 仍不包含下列四项。分流、问题描述与后续入口见 [`docs/AST-074闭合后剩余四项分流.md`](AST-074闭合后剩余四项分流.md)。

- 不让控制线程“代 owner 写 bottom”。
- 不把 Global FIFO/EDF 改成 lock-free。
- 不在本批修复 Reaper waiter 与 `shared_ptr<Impl>` 析构的 TSan 报告。
- 不把 TSan 注解升级成“已满足弱内存证明”。
