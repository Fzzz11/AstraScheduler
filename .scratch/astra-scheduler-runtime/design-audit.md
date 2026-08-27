# AstraScheduler 全项目设计审计

状态：`spec-approved`

日期：2026-08-26

## 结论

全项目最终目标均有已接受决策、ADR或明确的稳定契约覆盖。决策台账共 166 条：163 accepted、2 superseded、1 rejected；46 份 ADR。当前没有未决设计问题；项目 owner 已于 2026-08-26 批准包含 104 条 active 与 6 条 superseded Rule 的完整 spec，尚未拆 Tickets。

## 功能覆盖矩阵

| 项目目标 | 主要决策 | 主要 ADR | 最终里程碑 |
|---|---|---|---|
| Global Injection Queue / v0.1 correctness baseline | D-001, D-083–D-096, D-147 | ADR-0024–0027 | v0.1 / v0.2 |
| Per-Worker Local Queue / Work Stealing | D-090–D-096, D-147, D-162 | ADR-0026, ADR-0027, ADR-0041 | v0.2 |
| Chase-Lev Deque / reclamation / memory ordering | D-097–D-103, D-162 | ADR-0028, ADR-0029, ADR-0041 | v0.3 |
| TaskHandle / Result / wait / exception propagation | D-041–D-051, D-061–D-082, D-153, D-165 | ADR-0019, ADR-0020, ADR-0022, ADR-0023 | v0.1 |
| Cooperative Cancellation | D-052–D-060, D-080, D-154 | ADR-0021, ADR-0033 | v0.1；Coroutine组合在v0.5闭合 |
| Scheduler lifecycle / independent Runtime State / Reaper | D-002–D-040, D-148, D-155, D-156, D-159, D-160, D-166 | ADR-0001–0018, ADR-0042–0046 | v0.1 |
| Admission / backpressure / wakeup correctness | D-083–D-096 | ADR-0024–0027 | v0.1 / v0.2 |
| DAG Task Graph | D-104–D-113, D-123, D-152, D-161 | ADR-0030, ADR-0031 | v0.4 |
| C++20 Coroutine / await composition / Timer | D-114–D-128, D-147, D-154 | ADR-0032–0035 | v0.5 |
| Priority Scheduling | D-129–D-131 | ADR-0036 | v0.6 |
| Deadline Scheduling | D-132–D-134, D-147 | ADR-0037 | v0.6 |
| Runtime / process / wait-edge Metrics | D-135–D-137, D-148, D-149, D-151, D-152 | ADR-0038, ADR-0042 | v0.7 |
| Bounded Trace / Chrome Trace | D-138–D-140, D-149, D-151, D-153, D-158, D-163 | ADR-0039, ADR-0042 | v0.7 |
| Benchmark Framework / baselines / artifacts | D-141–D-143, D-150, D-162, D-164 | ADR-0040, ADR-0041 | v0.8 |
| Build / support matrix / version / release roadmap | D-144–D-146, D-159, D-162, D-164 | ADR-0041 | Phase 0–v1.0 |

## 生命周期扩展覆盖

- `begin_finalization()` 永久关闭注册、启动 Graceful Finalization并立即返回：D-023、D-024、D-026、D-030、D-032。
- `wait()` 是只在真实 Finalized 后返回的无界等待：D-027、D-037、D-039。
- `wait_for(timeout)` 只返回 Completed/TimedOut，超时不伪造完成且后台继续：D-028、D-035、D-036、D-039。
- 超时后注册入口保持关闭、Reaper/coordinator不重启：D-022、D-023、D-028、D-032、D-038。
- 调用方可继续等待、显式 `request_immediate()` 升级或终止进程：D-029、D-039；Immediate仍是cooperative，不能保证有界完成。

## 审计中消除的关键漂移

- Ready routing precedence 统一：D-147。
- Metrics Off 与 unobserved failure 不再产生隐藏计数：D-151。
- GraphReport observation 按真实 Failed Node 计数：D-152。
- Immediate 允许 already-started Coroutine resume、禁止 first-start：D-154。
- Scheduler 删除 public Created/start/restart，构造即 startup transaction：D-155、D-156。
- `NodeId` 取代历史 `GraphNodeId` 拼写：D-161。
- Capability 区分 None/Locked/ChaseLevLockFree：D-162。
- TraceCapture explicit stop / destructor abort：D-163。
- header/library version 查询分离：D-164。
- v0.1 submit 即支持 move-only decay-owned one-shot work：D-165。
- 同Runtime Worker调用同步shutdown统一抛logic_error：D-166。
- 总设计的旧版本路线、Optional/Advanced措辞和 `std::function` 首版建议已与台账对齐。

## 明确排除或延期

- Dynamic Worker Scaling、CPU affinity、NUMA、lock-free Global Queue、Timer Wheel、I/O Runtime/io_uring、Distributed/GPU Runtime。
- Typed DAG dataflow、动态 reprioritize/priority donation、completion callback API、在线 wait-for graph/cycle resolution。
- foreign awaitable 强制取消、线程强杀、硬实时 deadline/wakeup、跨版本/跨工具链 C++ ABI。
- 多个 DSO 各自静态嵌入一份实现时的 process-wide singleton 语义。

这些是已知边界，不是当前 spec blocker；若未来纳入范围，需要新的决策、spec revision 与 Tickets。

## 进入 spec 的前置检查

- 决策台账 traceability validator：通过（166 decisions）。
- Open Questions：无。
- 需求清单：全部有覆盖。
- 总设计：未发现 `TBD`、`TODO`、`后续决定`、`尚未定义` 等残留。
- `.scratch/astra-scheduler-runtime/spec.md` 已基于当前 ledger、ADRs、CONTEXT 与总设计批准为全项目规范，并通过不带 `--allow-draft` 的 traceability 校验；后续可交给 `to-tickets` 按里程碑拆分。
