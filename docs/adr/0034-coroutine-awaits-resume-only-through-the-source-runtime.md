---
status: accepted
date: 2026-08-26
decisions: [D-120, D-121, D-122, D-124, D-125]
---

# Coroutine awaits resume only through the source Runtime

左值TaskHandle/GraphRun可由Astra-managedTask异步await，awaiter复制capability并用arm-trigger registration挂起；target completion或source stop的唯一winner只在source Runtime发布resume ticket，绝不inline resume或替target发起取消。Await resume分别传播原Task Outcome或返回GraphReport；rvalue await删除以保护共享引用lifetime，direct self-await/self-run在副作用前拒绝。

`cancellation_point`不挂起，`yield`真正suspend并强制Global requeue以提供公平service opportunity。ColdTask本身不可直接await，必须显式spawn获得Handle；核心API也不增加wait_until、stop-token blocking wait或任意completion callback，把异步组合收敛到Coroutine内部registration seam。

决策细节见 [D-120 至 D-122、D-124 与 D-125](../../.scratch/astra-scheduler-runtime/decision-log.md)。
