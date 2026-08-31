#!/usr/bin/env python3
"""R-114/R-117 public/internal boundary gate."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
PUBLIC_TEST_RE = re.compile(r"astra_add_public_test\([^\s]+\s+([^\s\)]+)\)")
FORBIDDEN_PUBLIC_TEST_TEXT = (
    "astra::detail::",
    '"test_seam.hpp"',
    '"reaper_registry.hpp"',
    '"trace_collector.hpp"',
    '"graph_shared_state.hpp"',
    '"chase_lev_deque.hpp"',
)
FORBIDDEN_PUBLIC_TEST_RE = re.compile(r"(?:\.|::)[A-Za-z_][A-Za-z0-9_]*_internal\s*\(")


NEGATIVE_PROBES = {
    "frozen_node_storage": """
#include <astra/graph.hpp>
int main() { astra::FrozenTaskGraph::NodeData node; (void)node; }
""",
    "frozen_nodes_accessor": """
#include <astra/graph.hpp>
int main() { astra::FrozenTaskGraph graph; (void)graph.nodes_internal(); }
""",
    "task_raw_frame": """
#include <astra/coroutine.hpp>
astra::Task<void> task() { co_return; }
int main() { auto value = task(); (void)value.release_handle(); }
""",
    "task_handle_state": """
#include <astra/coroutine.hpp>
int main() { astra::TaskHandle<void> handle; (void)handle.shared_state_internal(); }
""",
    "graph_completion_storage": """
#include <astra/coroutine.hpp>
int main() { astra::GraphRun run; run.add_completion_callback_internal([] {}); }
""",
    "scheduler_test_seam": """
#include <astra/scheduler.hpp>
int main() { astra::Scheduler s; astra::detail::run_test_task_on_worker(s, [] {}); }
""",
    "await_handshake": """
#include <astra/coroutine.hpp>
int main() { astra::AwaitHandshake handshake; (void)handshake; }
""",
    "R120_await_handshake": """
#include <astra/coroutine.hpp>
int main() { astra::detail::AwaitHandshake handshake; (void)handshake; }
""",
    "R119_task_shared_state_base": """
#include <astra/task_handle.hpp>
#include <astra/id.hpp>
int main() {
  astra::detail::TaskSharedStateBase base{astra::TaskId{}};
  (void)base;
}
""",
    "R119_task_shared_state": """
#include <astra/task_handle.hpp>
#include <astra/id.hpp>
int main() {
  astra::detail::TaskSharedState<int> state{astra::TaskId{}};
  (void)state;
}
""",
    "R119_result_cell_independent": """
#include <astra/task_handle.hpp>
int main() { astra::TaskHandle<int>::ResultCell cell; (void)cell; }
""",
    "R119_task_control_block": """
#include <astra/task_handle.hpp>
#include <astra/id.hpp>
int main() {
  astra::detail::TaskControlBlock block{astra::TaskId{}};
  (void)block;
}
""",
    "ready_queue_link_fields": """
#include <astra/task_handle.hpp>
int main() {
  astra::detail::TaskInvokerBase* invoker = nullptr;
  invoker->ready_next = nullptr;
  invoker->ready_is_external = false;
}
""",
}


FORBIDDEN_COMPLETE_TYPES = (
    (r"class\s+AwaitHandshake\s*\{", "AwaitHandshake"),
    (r"class\s+TaskSharedStateBase\s*\{", "TaskSharedStateBase"),
    (r"class\s+TaskSharedState\s*(:|<|\{)", "TaskSharedState"),
    (r"class\s+TaskControlBlock\s*\{", "TaskControlBlock"),
)


def audit_installed_headers() -> list[str]:
    problems: list[str] = []
    include_dir = ROOT / "include" / "astra"
    invoker_try_start = re.compile(
        r"class TaskInvokerModel[\s\S]*?void execute\(\) override \{[\s\S]*?try_start",
        re.MULTILINE,
    )
    for header in include_dir.rglob("*.hpp"):
        text = header.read_text(encoding="utf-8")
        rel = header.relative_to(ROOT).as_posix()
        for pattern, name in FORBIDDEN_COMPLETE_TYPES:
            if re.search(pattern, text):
                problems.append(f"installed header {rel} completes protocol type {name}")
        if "std::function<" in text and "class TaskInvokerModel" in text:
            # F envelope must store F, not std::function.
            if re.search(r"std::function\s*<[^>]+>\s+fn_", text):
                problems.append(f"installed header {rel} erases F with std::function")
        if invoker_try_start.search(text):
            problems.append(f"installed header {rel} inlines try_start in TaskInvokerModel::execute")
        if "ready_next" in text or "ready_is_external" in text:
            problems.append(
                f"installed header {rel} exposes Ready Queue intrusive link fields"
            )
    return problems


R124_MODULE_FILES = (
    "src/runtime/admission_controller.hpp",
    "src/runtime/admission_controller.cpp",
    "src/runtime/timer_queue.hpp",
    "src/runtime/timer_queue.cpp",
    "src/runtime/runtime_metrics.hpp",
    "src/runtime/runtime_metrics.cpp",
)
R124_OWNED_STATE = {
    "src/runtime/admission_controller.hpp": ("pending_", "slot_cv_", "capacity_"),
    "src/runtime/timer_queue.hpp": ("heap_", "map_"),
    "src/runtime/runtime_metrics.hpp": ("worker_shards", "control_shard"),
}
IMPL_PTR_RE = re.compile(r"Scheduler\s*::\s*Impl\s*\*")


def audit_impl_deep_modules() -> list[str]:
    problems: list[str] = []
    for rel in R124_MODULE_FILES:
        path = ROOT / rel
        if not path.is_file():
            problems.append(f"R-124 module missing: {rel}")
            continue
        text = path.read_text(encoding="utf-8")
        if IMPL_PTR_RE.search(text):
            problems.append(f"{rel} holds Scheduler::Impl* (R-124 forbids shallow wrappers)")
    for rel, names in R124_OWNED_STATE.items():
        path = ROOT / rel
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        for name in names:
            if name not in text:
                problems.append(f"{rel} missing owned state {name} (R-124 deletion test)")
    return problems


def audit_internal_module_boundaries() -> list[str]:
    problems: list[str] = []

    graph_execution = ROOT / "src" / "graph" / "graph_execution.cpp"
    if not graph_execution.is_file():
        problems.append("R-126 module missing: src/graph/graph_execution.cpp")
    else:
        graph_text = graph_execution.read_text(encoding="utf-8")
        for forbidden in ("Scheduler::Impl", "scheduler.impl_", "Scheduler&"):
            if forbidden in graph_text:
                problems.append(
                    f"src/graph/graph_execution.cpp depends on {forbidden} (R-125)"
                )

    scheduler = ROOT / "src" / "runtime" / "scheduler.cpp"
    if not scheduler.is_file():
        problems.append("R-127 module missing: src/runtime/scheduler.cpp")
    else:
        scheduler_text = scheduler.read_text(encoding="utf-8")
        for forbidden in (
            "GraphRun detail::GraphExecution::run",
            "void Scheduler::Impl::worker_main",
            "g_runtime_registry",
        ):
            if forbidden in scheduler_text:
                problems.append(f"scheduler.cpp still owns {forbidden} (R-126/R-127)")
        for forbidden in (
            "struct QueuedTask",
            "struct EdfEntry",
            "struct WaitDiagnosticsGuard",
            "void record_metrics_submission_attempt",
        ):
            if forbidden in scheduler_text:
                problems.append(f"scheduler.cpp still owns {forbidden} (R-129/R-130)")

    deep_runtime_files = {
        "src/runtime/ready_queues.hpp": ("global_edf_heaps_", "local_queues_", "claimed_count_"),
        "src/runtime/runtime_state.hpp": ("ReadyQueues ready_queues", "worker_threads", "RuntimeDiagnostics diagnostics"),
        "src/runtime/runtime_diagnostics.hpp": ("RuntimeDiagnostics final", "WaitDiagnosticsGuard"),
        "src/runtime/worker_loop.cpp": ("void run_worker_loop",),
    }
    for rel, owned_markers in deep_runtime_files.items():
        path = ROOT / rel
        if not path.is_file():
            problems.append(f"R-129/R-130 module missing: {rel}")
            continue
        text = path.read_text(encoding="utf-8")
        if IMPL_PTR_RE.search(text):
            problems.append(f"{rel} depends on Scheduler::Impl* (R-129/R-130)")
        for marker in owned_markers:
            if marker not in text:
                problems.append(f"{rel} missing owned marker {marker} (R-129/R-130)")

    worker_header = ROOT / "src" / "runtime" / "worker_loop.hpp"
    if worker_header.is_file() and "template <" in worker_header.read_text(encoding="utf-8"):
        problems.append("worker_loop.hpp still template-instantiates Scheduler internals (R-130)")

    registry = ROOT / "src" / "runtime" / "runtime_registry.hpp"
    if registry.is_file():
        registry_text = registry.read_text(encoding="utf-8")
        if "void*" in registry_text or "RuntimeDiagnostics*" not in registry_text:
            problems.append("runtime registry is not a narrow RuntimeDiagnostics registry (R-130)")

    allowed_root_files = {"version.cpp"}
    for path in (ROOT / "src").iterdir():
        if path.is_file() and path.name not in allowed_root_files:
            problems.append(f"flat private source remains at {path.relative_to(ROOT)} (R-128)")
    for path in (ROOT / "src").rglob("*"):
        if path.suffix not in {".hpp", ".cpp"}:
            continue
        if '#include "../' in path.read_text(encoding="utf-8"):
            problems.append(f"cross-module ../ include in {path.relative_to(ROOT)} (R-128)")

    return problems


def audit_public_tests() -> list[str]:
    cmake = (ROOT / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    problems: list[str] = []
    for source_name in PUBLIC_TEST_RE.findall(cmake):
        source = ROOT / "tests" / source_name
        text = source.read_text(encoding="utf-8")
        for marker in FORBIDDEN_PUBLIC_TEST_TEXT:
            if marker in text:
                problems.append(f"public test {source_name} uses internal marker {marker}")
        if FORBIDDEN_PUBLIC_TEST_RE.search(text):
            problems.append(f"public test {source_name} calls an *_internal entry point")
    return problems


def probe_compiler() -> str:
    env = os.environ.get("ASTRA_CXX")
    if env:
        return env
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path:
            return path
    return "c++"


def probe_build_include() -> Path:
    env = os.environ.get("ASTRA_BUILD_INCLUDE")
    if env:
        return Path(env)
    build_root = ROOT / "build"
    if build_root.is_dir():
        for candidate in sorted(build_root.glob("*/include")):
            if (candidate / "astra" / "version.hpp").is_file():
                return candidate
    return ROOT / "build" / "wsl-gcc-debug" / "include"


def run_negative_probes() -> list[str]:
    problems: list[str] = []
    cxx = probe_compiler()
    generated_include = probe_build_include()
    with tempfile.TemporaryDirectory(prefix="astra-encapsulation-") as directory:
        probe = Path(directory) / "probe.cpp"
        for name, source in NEGATIVE_PROBES.items():
            probe.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [cxx, "-std=c++20", "-fsyntax-only", "-I", str(ROOT / "include"),
                 "-I", str(generated_include), str(probe)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if result.returncode == 0:
                problems.append(f"internal API unexpectedly compiled: {name}")
    return problems


def main() -> int:
    problems = (
        audit_public_tests()
        + audit_installed_headers()
        + run_negative_probes()
        + audit_impl_deep_modules()
        + audit_internal_module_boundaries()
    )
    if problems:
        print("FAIL: encapsulation boundary drift:")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print(
        f"Encapsulation gate OK: public tests isolated; {len(NEGATIVE_PROBES)} internal probes rejected; "
        "R-124 modules own state; R-125..R-130 Graph/Worker/ReadyQueues/RuntimeState/diagnostics boundaries hold."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
