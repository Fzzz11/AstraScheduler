---
status: accepted
date: 2026-08-26
decisions: [D-138, D-139, D-140, D-158, D-163]
---

# Trace capture is bounded and exported offline

显式共享`TraceCollector`可附加到多个Runtime并重复执行单一活动capture代际。Runtime只向预分配固定容量buffer写trivially-copyable versioned events；禁用是fast no-op，满时drop-newest而绝不阻塞、分配或写文件。

默认每Worker 16,384 events、shared external/control 65,536、每Reaper producer 4,096；Task/Wait/Coroutine/Graph/Timer/Runtime与steal-success category默认开启，逐次steal-attempt等Verbose事件显式opt-in。`start_capture`完成全部预分配后才发布Recording，失败保留上一snapshot和Stopped状态。

Event使用稳定逻辑Runtime/Worker/Task/Graph/segment identity及per-producer顺序，不保存raw pointer、字符串payload或异常内容。Metrics和Trace复用相同admission/start/suspend/terminal事件口径。

capture停止并使emitters quiescent后才产生不可变snapshot并离线导出确定性Chrome Trace JSON。所有loss显式进入metadata；只有零drop且schema有效才标记complete，日志仍是独立的低频控制面。

只有显式`TraceCapture::stop()`会提交并产出可复制immutable `TraceSnapshot`，重复stop共享同一backing snapshot。活动Capture析构则noexcept地disable/quiesce并丢弃该generation，使Collector回到Stopped；不会留下隐式结果或卡住下一代capture。

决策细节见 [D-138 至 D-140、D-158 与 D-163](../../.scratch/astra-scheduler-runtime/decision-log.md)。
