---
status: accepted
date: 2026-08-26
decisions: [D-097, D-098, D-101, D-102, D-103]
---

# Chase-Lev follows the proven portable memory ordering

Phase 3 先保留 seq_cst Chase-Lev oracle，再启用严格映射 2013 portable C11 proof 的 C++20 production variant；`consume` 提升为 `acquire`，不使用架构专用汇编。Push publication、pop/steal seq_cst fence 与 last-item top CAS 的顺序成为可审计契约，未来弱化必须另有 memory-model 证据和决策。

Deque 内部区分 Success/Empty/Retry，只有成功 CAS/owner claim 转移 Scheduling Reference。`uint64_t` index 在高水位前通过 active-thief guard 进入极冷 quiescent rebase；关键 atomic 非 lock-free 的平台回退带锁 deque。因此项目只对具体平台的正常 fast path 声称 lock-free，不把 resize、rebase 或 portability fallback 包装成绝对保证。

固定宽度翻译额外使用 checked arithmetic：空 canonical state 在 unsigned decrement 前返回 Empty，`size >= capacity - 1` 即 grow 以保留一个 cell，doubling 失败走 Global fallback；不能把语言层 defined unsigned wrap 当作算法正确性。

GCC TSan 不建模 `atomic_thread_fence`。生产 `ChaseLevDeque` 在 fence 旁的 `__tsan_acquire` / `__tsan_release` 只改检测器 happens-before，不生成 CPU 屏障，也不改变 C++ memory order。TSan 相关测试通过不能当成弱内存（含 native AArch64）证明；弱内存证据仍是本 ADR 对照的 portable 序与 Tier-2 复核。

决策细节见 [D-097、D-098 与 D-101 至 D-103](../../.scratch/astra-scheduler-runtime/decision-log.md)。
