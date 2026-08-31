# Chase-Lev 生产 ReadyQueues 接线审查

状态：审查完成，未改代码  
日期：2026-08-31  
对象：未提交工作区相对 `HEAD`（`0736f95`）的 AST-074 接线，以及既有 `ChaseLevDeque` 生产实现

本文合并两轮独立审查：Codex 对 AST-074 工作区的 Standards/Spec 审查，以及随后在本仓库内对照源码、Ticket、Spec 与 ADR 的复核。只记录代码事实与规范缺口，不把 Ticket 自述或 Debug 全量通过当成规范已满足。

## 1. 范围与方法

- **代码**：`src/scheduling/chase_lev_deque.hpp`、`src/runtime/ready_queues.{hpp,cpp}`、`src/runtime/runtime_state.cpp`、`src/runtime/scheduler.cpp`、`include/astra/capabilities.hpp`、`tests/test_chase_lev_ready_queues.cpp`、`tests/test_chase_lev_indices.cpp`、根 `CMakeLists.txt`。
- **规范**：R-066、R-067、R-068、R-081、R-101；ADR-0028、ADR-0029；Ticket AST-074。
- **验证**：WSL Debug 全量 `cmake -S . -B build/wsl-gcc-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build/wsl-gcc-debug -j2 && ctest --test-dir build/wsl-gcc-debug --output-on-failure` 于 2026-08-31 得到 54/54 passed。ASan/UBSan 与 TSan 未在本轮复核。
- **非范围**：`docs/工程代码审查.md` 的同步改写不影响接线正确性，也不属于 AST-074 核心实现。

## 2. 总体结论

生产路径**已经接到** Chase-Lev：`Scheduler` 用 `ReadyQueues::preferred_local_backend()` 冻结 `SchedulerCapabilities`，`RuntimeState` 把同一 backend 交给 `ReadyQueues`；所需 atomic 不 lock-free 时仍走 Locked fallback，不再把加锁队列虚报为 `ChaseLevLockFree`。Debug 全量通过，说明常见路径能跑通。

接线正确不等于规范闭合。剩余问题分成两轴：

| 轴 | 硬问题 | 最严重项 |
|---|---|---|
| Standards | 2 | 生产 deque 未满足 R-066/R-068 的 uint64 index 与生产期 quiescent rebase 契约 |
| Spec | 5 | Local growth failure 尚未形成 ADR-0029 要求的 allocation-free Global fallback |

判断性建议 2 条。Ticket AST-074 当前标 `done` 过早。

## 3. 已确认成立的接线

- 每个 Worker 的四个 Local priority band 在 `ChaseLevLockFree` 路径使用 `ChaseLevDeque`；owner 从 bottom push/pop，thief 从 top steal，不再获取 Local mutex。
- capability 与实例 backend 同源，生命周期内冻结。
- Local push 在 deque 扩容或 `ChaseNode` 分配失败时，把任务送回 Global FIFO，不 inline 执行。
- 新增 `astra_chase_lev_ready_queues_test` 已进入 `tests/CMakeLists.txt`，并随 Debug 全量执行。

这些事实只说明“虚报 lock-free”的旧问题已消除，不能覆盖第 4、5 节。

## 4. Standards

### S-1（硬）：生产 deque 仍用有符号 `int64_t` 保存 top/bottom，rebase 未进入生产生命周期

R-066 要求 production 使用 **uint64** atomic top/bottom。ADR-0028 要求 `uint64_t` index 在高水位前通过 **active-thief guard** 进入极冷 quiescent rebase，且不得依赖 wrap。

实际实现：

```345:346:src/scheduling/chase_lev_deque.hpp
    std::atomic<std::int64_t> top_;
    std::atomic<std::int64_t> bottom_;
```

`push`/`pop`/`steal`/`grow` 用 `b - t` 有符号减法。`maybe_quiescent_rebase()` 存在，但只在 `b == t` 时直接把 top/bottom 写成 0，没有 active-thief guard：

```295:307:src/scheduling/chase_lev_deque.hpp
    bool maybe_quiescent_rebase(
        std::int64_t high_watermark = kDefaultRebaseHighWatermark) noexcept {
        std::int64_t b = bottom_.load(std::memory_order_relaxed);
        std::int64_t t = top_.load(std::memory_order_relaxed);
        if (b == t && b >= high_watermark) {
            top_.store(0, std::memory_order_relaxed);
            bottom_.store(0, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
```

全仓库生产调用点为零。唯一调用在 `tests/test_chase_lev_indices.cpp`，还依赖 `set_test_indices()` 人为抬高水位。长期运行后索引会逼近有符号边界，R-068 的 index exhaustion 语义因此未完成。

### S-2（硬）：每次 Local publication 额外分配 `ChaseNode`，失败后仍可能再分配

R-067 / ADR-0029 要求 Ready Task 使用单一侵入式 Scheduling Reference；Local growth/allocation 失败回退到 **TCB 内嵌 intrusive link 的 allocation-free Global Queue**。

实际实现：

```64:76:src/runtime/ready_queues.cpp
    std::unique_ptr<ChaseNode> node;
    try {
        node = std::make_unique<ChaseNode>(std::move(task));
    } catch (...) {
        return false;
    }
    node->published.store(true, std::memory_order_release);
    if (!chase_lev_bands[band_index]->push(node.get())) {
        task = std::move(node->task);
        return false;
    }
    (void)node.release();
    return true;
```

失败后 `ReadyQueues::publish` 走 `global_fifo_queues_[band].push_back(...)`。`std::deque` 在内存压力下仍可能再次分配失败或抛异常。这不是“注入扩容失败且 Global 分配成功”的测试路径所能覆盖的。

### S-3（判断）：`LocalQueues` 多处按 backend 分支

`ctor` / `dtor` / `push` / `claim_back` / `steal_front` / `empty` / `cancel_unstarted` / `set_growth_failure_for_testing` 反复判断 `backend == ChaseLevLockFree`。属于 possible Repeated Switches，不是正确性缺陷。

### S-4（判断）：多个测试重复计算 expected backend

`tests/selftest.cpp`、`tests/consumer/main.cpp`、`tests/test_scheduler_contract.cpp`、`tests/test_chase_lev_indices.cpp`、`tests/test_global_worker_runtime.cpp`、`tests/test_locked_local_routing.cpp`、`tests/test_chase_lev_ready_queues.cpp` 各自复制：

```cpp
std::atomic<std::int64_t>::is_always_lock_free &&
std::atomic<void*>::is_always_lock_free
```

内部测试可改为调用 `ReadyQueues::preferred_local_backend()`。该副本还比 deque 的 `is_lock_free()` 少检查 `atomic<T>`。

## 5. Spec

### P-1：R-068 高水位 rebase 只存在于独立测试

与 S-1 同一缺口的规范面：index exhaustion 语义未进入生产生命周期。单测证明函数可在人为静止条件下归零，不证明 Runtime 会在高水位前 rebase。

### P-2：AST-074 所称 growth failure 回退 Global 只在注入失败且 Global 分配成功时成立

Ticket 接受标准写“Local growth 失败时工作回退 Global Queue，不丢失、不重复、不 inline 执行”。`test_growth_failure_falls_back_to_global_without_loss` 通过 `set_inject_growth_failure` 制造失败，再断言 `global_size() == 1`。真正内存压力下 Global `push_back` 仍可能失败；当前路径也没有 TCB 内嵌 link 可走。规范要求的是 allocation-free fallback，不是“第二次堆分配碰巧成功”。

### P-3：`ChaseNode::published` 未纳入 lock-free 探测，且 load 结果被丢弃

`preferred_local_backend()` 委托 `ChaseLevDeque<ChaseNode*>::is_lock_free()`，只检查：

```289:293:src/scheduling/chase_lev_deque.hpp
    static constexpr bool is_lock_free() noexcept {
        return std::atomic<std::int64_t>::is_always_lock_free &&
               std::atomic<Buffer*>::is_always_lock_free &&
               std::atomic<T>::is_always_lock_free;
    }
```

`ChaseNode::published` 是额外的 `std::atomic<bool>`，探测未覆盖。R-101 要求所有所需 atomic 始终 lock-free 才上报 `ChaseLevLockFree`。该字段的 store/load 结果均被丢弃，优先删除而不是补探测。

### P-4：Immediate cleanup 改变 Coroutine resume 位置

Ticket 要求 Immediate cleanup 与 Coroutine resume 语义保持不变。加锁路径把 resume 留在 Local band；Chase-Lev 路径对四个 band 做非 owner `steal`，把 resume 再发布到 Global FIFO。owner LIFO 丢失。Immediate 下 helping 会跳过 `claim_global`，这些 resume 存在滞留风险。

### P-5：析构 drain 与 steal Retry 被当成 empty

- `~LocalQueues` 只在 `steal == Success` 时继续，遇到 `Retry` 立即停止，可能漏删 `ChaseNode`。
- `claim_chase_lev` 对 thief 最多重试 3 次 `Retry` 后将该 band 视为空并改选其他 band。R-068 / D-102：Retry 不得当作 empty proof；R-081 只允许跳过**空** band。

新测试覆盖 capability、owner LIFO、`stolen > 0`、注入扩容回退，未覆盖 steal-oldest、四 Local band 权重、Immediate cleanup、resume 保留。

## 6. 不属于 AST-074 核心、但随工作区出现的项

- `docs/工程代码审查.md` 把 P0-2 写成“已由 AST-074 完成”，并记录 ASan/TSan 通过。接线已发生，但第 4、5 节说明该条不能再写成规范闭合；ASan/TSan 也缺少带独立 build 目录的完整 WSL 命令。
- 根 `CMakeLists.txt` 为 GCC TSan 增加 `-Wno-error=tsan`，作用于全部 TU。可以解释为 R-066 fence 与 GCC TSan 不建模 `atomic_thread_fence` 的工程例外，但超出 Ticket 所述范围。
- Ticket Verification 把 Debug 全量写成已通过是可复核的；ASan/UBSan、TSan 条目没有同等命令证据。

## 7. 建议整改顺序

1. 生产 `ChaseLevDeque` 改为 uint64 top/bottom，并在 owner 空闲路径加入带 active-thief guard 的 quiescent rebase。
2. 去掉 `ChaseNode` 堆分配：cell 存侵入式 Scheduling Reference；Local 失败走 allocation-free Global fallback；删除无用的 `published` 字段。
3. Immediate cleanup 保持 resume 在 Local（或证明迁到 Global 后 helping 仍能取到且不违反 R-063），析构必须排空 `Retry`。
4. steal `Retry` 不得升级为 empty band；补 steal-oldest、四 band、Immediate cleanup、resume 测试。
5. 用真实 WSL ASan/UBSan、TSan 命令更新 Ticket 证据后再考虑把 AST-074 标为 done。

在 1、2 完成前，不应把 `docs/工程代码审查.md` 的 P0-2 或 AST-074 当作规范已闭合。
