---
status: accepted
date: 2026-08-26
decisions: [D-141, D-142, D-143, D-150]
---

# Benchmarks are verified reproducible experiments

Benchmark组件不进入core consumer build。Google Benchmark承载primitive microcases，自有scenario runner承载多阶段Runtime workload、子进程隔离与versioned JSON；timed region前后有严格checksum/outcome verification，错误样本不得形成性能结论。

Corpus同时保留Global FIFO、locked Work-Stealing和Chase-Lev in-tree baselines，并覆盖DAG、Coroutine、Timer、Priority、Deadline、shutdown与观测开销。oneTBB仅是可选可比子集背景；显式`std::async(std::launch::async)`只用于受限粗粒度独立任务context，不进入primary regression ranking。

Standard profile使用2秒warmup和10个至少1秒repetition，不自动删除outlier，保存环境与全部原始结果。普通CI不以噪声数字阻塞；正式回归只在专用稳定runner按versioned policy同时满足实践影响和统计置信条件时判定。

决策细节见 [D-141 至 D-143 与 D-150](../../.scratch/astra-scheduler-runtime/decision-log.md)。
