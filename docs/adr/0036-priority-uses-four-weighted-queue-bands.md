---
status: accepted
date: 2026-08-26
decisions: [D-129, D-130, D-131]
---

# Priority uses four weighted queue bands

Task admission解析不可变的Low、Normal、High或Critical base Priority。无options External Task为Normal，same-Runtime Internal Task默认继承current Task；Callable、Coroutine和Graph统一通过`TaskOptions`显式覆盖，TaskHandle不支持动态boost。

Global与每个Worker Local Source都分成四个queue band；Local每band保持独立Chase-Lev端点语义。已经批准的Local burst/Global probe仍控制外层source选择，source内部使用`Critical:High:Normal:Low = 8:4:2:1`的work-conserving加权日历，持续低优先级work不会被永久饿死。

Priority只影响尚未claim的Ready work，不抢占Running segment、不映射OS thread priority，也不承诺wall-clock latency。

决策细节见 [D-129 至 D-131](../../.scratch/astra-scheduler-runtime/decision-log.md)。
