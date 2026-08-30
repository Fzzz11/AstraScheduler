#!/usr/bin/env python3
"""R-114/R-117 public/internal boundary gate."""

from __future__ import annotations

import re
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


def run_negative_probes() -> list[str]:
    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="astra-encapsulation-") as directory:
        probe = Path(directory) / "probe.cpp"
        for name, source in NEGATIVE_PROBES.items():
            probe.write_text(source, encoding="utf-8")
            result = subprocess.run(
                ["g++", "-std=c++20", "-fsyntax-only", "-I", str(ROOT / "include"),
                 "-I", str(ROOT / "build" / "wsl-gcc-debug" / "include"), str(probe)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if result.returncode == 0:
                problems.append(f"internal API unexpectedly compiled: {name}")
    return problems


def main() -> int:
    problems = audit_public_tests() + audit_installed_headers() + run_negative_probes()
    if problems:
        print("FAIL: encapsulation boundary drift:")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print(f"Encapsulation gate OK: public tests isolated; {len(NEGATIVE_PROBES)} internal probes rejected.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
