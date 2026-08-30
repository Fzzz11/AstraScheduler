# AST-057 — 收紧 public/internal 边界并深模块化运行时协议

Parent: [AstraScheduler v0.1 → v1.1 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-113, R-114, R-115, R-116, R-117)
Milestone: v1.1.0
Blocked by: AST-056
Status: done
Claimed by: agent

## Rules and decisions

- R-113 [primary] — v1.0 manifest 不可变，v1.1 gate 识别 documented surface 变化；source: D-169
- R-114 [primary] — Task/Coroutine/Graph/Scheduler 控制入口不可被普通 consumer 调用；source: D-169
- R-115 [primary] — TaskId 由 Runtime 分配，admission 全路径共享平衡回滚；source: D-169
- R-116 [primary] — Graph 运行状态机由 GraphExecution 独占；source: D-169
- R-117 [primary] — public tests 与 internal seams 物理隔离；source: D-169

## What to build

把API freeze从整头文件/全部符号哈希改为documented semantic contract，保持v1.0 manifest不可变；私有化Task、Coroutine、Graph和Scheduler测试逃逸入口；把TaskId/admission所有权放入Runtime；引入内部GraphExecution边界；分离public与internal tests。

## Invariants

- 不改变documented public observable semantics。
- 不改写已发布v1.0 manifest。
- public consumer不能修改内部状态或使用测试控制入口。
- admission失败保持容量、pending、identity与ownership平衡。
- GraphExecution独占图运行协议状态。

## Test-first seam

- semantic API allowlist与独立consumer compile probes。
- 被收回入口的negative compile probes。
- TaskId overflow/admission rollback与GraphRun现有并发测试。
- public tests不带`src/` include path，internal tests显式选择该路径。

## Acceptance criteria

- [x] `[R-113]` v1.0 manifest不可变，v1.1 semantic public API gate可识别documented surface变化。
- [x] `[R-114]` 审查文档列出的Task/Coroutine/Graph/Scheduler控制入口不可由普通consumer调用。
- [x] `[R-115]` TaskId由Runtime分配，全部admission路径共享平衡回滚。
- [x] `[R-116]` Scheduler不再直接拥有Graph运行状态机，现有Graph行为测试通过。
- [x] `[R-117]` public tests不使用源码include/internal seam，internal tests仍可白盒验证。

## Out of scope

- 不增加新调度功能或改变既有策略语义。
- 不承诺跨toolchain ABI。
- 不进行与不变量边界无关的命名或格式化重写。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-113 through R-117
- Decision: `.scratch/astra-scheduler-runtime/decision-log.md` — D-169
- Review: `docs/封装性改进建议.md`
- Verification: WSL Debug、ASan/UBSan、package consumer、API gates 全部通过；WSL GCC Debug build通过；Debug串行全量`52/52`通过；ASan/UBSan相关admission/graph/coroutine/encapsulation `7/7`通过；semantic API gate通过（17 headers、62 documented symbols、consumer contract）；v1.0 manifest SHA-256与tag一致；`git diff --check`通过。
