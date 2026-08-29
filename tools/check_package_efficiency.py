"""CMake package gate resource-usage regression tests."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import check_cmake_package

INTERNAL_TEST_TARGETS = {
    "astra_admission_backpressure_test",
    "astra_base_priority_test",
    "astra_chase_lev_growth_test",
    "astra_chase_lev_indices_test",
    "astra_chase_lev_ordering_test",
    "astra_finalization_begin_test",
    "astra_finalization_escalation_test",
    "astra_finalization_wait_test",
    "astra_global_worker_runtime_test",
    "astra_graph_coroutine_identity_test",
    "astra_move_only_submit_test",
    "astra_park_handshake_test",
    "astra_platform_invariants_test",
    "astra_process_metrics_test",
    "astra_reaper_coordinator_test",
    "astra_runtime_state_handoff_test",
    "astra_startup_transaction_test",
    "astra_steal_round_test",
    "astra_suspended_cancellation_test",
    "astra_trace_collector_test",
    "astra_trace_event_schema_test",
    "astra_trace_export_test",
}


class PackageBuildParallelismTests(unittest.TestCase):
    """The package CLI must translate the standard CMake limit to a bounded job count."""

    def test_default_parallel_level_is_two(self):
        with patch.dict(os.environ, {}, clear=True):
            command = check_cmake_package.cmake_build_command(Path("/tmp/build"))

        self.assertEqual(
            command,
            ["cmake", "--build", "/tmp/build", "--parallel", "2"],
        )

    def test_standard_parallel_level_is_forwarded_as_numeric_limit(self):
        with patch.dict(os.environ, {"CMAKE_BUILD_PARALLEL_LEVEL": "3"}):
            command = check_cmake_package.cmake_build_command(Path("/tmp/build"))

        self.assertEqual(
            command,
            ["cmake", "--build", "/tmp/build", "--parallel", "3"],
        )

    def test_invalid_parallel_level_is_rejected(self):
        for value in ("0", "invalid"):
            with self.subTest(value=value):
                with patch.dict(
                    os.environ, {"CMAKE_BUILD_PARALLEL_LEVEL": value}
                ):
                    with self.assertRaisesRegex(ValueError, "positive integer"):
                        check_cmake_package.cmake_build_command(
                            Path("/tmp/build")
                        )


class PackageTestBuildGraphTests(unittest.TestCase):
    """In-tree tests must reuse the library variant selected by CMake."""

    def test_test_executables_link_library_without_recompiling_implementation(self):
        repo_root = Path(__file__).resolve().parent.parent
        implementation_dir = (repo_root / "src").resolve()

        with tempfile.TemporaryDirectory(prefix="astra-build-graph-") as temp:
            build = Path(temp)
            query = build / ".cmake" / "api" / "v1" / "query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(repo_root),
                    "-B",
                    str(build),
                    "-DCMAKE_BUILD_TYPE=Release",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )

            reply = build / ".cmake" / "api" / "v1" / "reply"
            index = json.loads(next(reply.glob("index-*.json")).read_text())
            codemodel_file = index["reply"]["codemodel-v2"]["jsonFile"]
            codemodel = json.loads((reply / codemodel_file).read_text())
            target_refs = codemodel["configurations"][0]["targets"]
            names_by_id = {target["id"]: target["name"] for target in target_refs}

            offenders = {}
            for target_ref in target_refs:
                if not target_ref["name"].startswith("astra_"):
                    continue
                target = json.loads((reply / target_ref["jsonFile"]).read_text())
                if target["type"] != "EXECUTABLE":
                    continue

                implementation_sources = []
                for source in target.get("sources", []):
                    path = Path(source["path"])
                    if not path.is_absolute():
                        path = repo_root / path
                    path = path.resolve()
                    if implementation_dir in path.parents:
                        implementation_sources.append(path.name)

                dependencies = {
                    names_by_id[dependency["id"]]
                    for dependency in target.get("dependencies", [])
                }
                if implementation_sources or "AstraScheduler" not in dependencies:
                    offenders[target_ref["name"]] = {
                        "implementation_sources": implementation_sources,
                        "dependencies": sorted(dependencies),
                    }

            self.assertEqual(
                offenders,
                {},
                "test executables must link AstraScheduler and must not compile "
                "implementation .cpp files directly",
            )

    def test_shared_internal_tests_reuse_one_static_test_runtime(self):
        repo_root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix="astra-shared-build-graph-") as temp:
            build = Path(temp)
            query = build / ".cmake" / "api" / "v1" / "query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(repo_root),
                    "-B",
                    str(build),
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DBUILD_SHARED_LIBS=ON",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )

            reply = build / ".cmake" / "api" / "v1" / "reply"
            index = json.loads(next(reply.glob("index-*.json")).read_text())
            codemodel_file = index["reply"]["codemodel-v2"]["jsonFile"]
            codemodel = json.loads((reply / codemodel_file).read_text())
            target_refs = codemodel["configurations"][0]["targets"]
            names_by_id = {target["id"]: target["name"] for target in target_refs}
            refs_by_name = {target["name"]: target for target in target_refs}

            self.assertIn("AstraSchedulerTestRuntime", refs_by_name)
            offenders = {}
            for name in INTERNAL_TEST_TARGETS:
                target_ref = refs_by_name[name]
                target = json.loads((reply / target_ref["jsonFile"]).read_text())
                dependencies = {
                    names_by_id[dependency["id"]]
                    for dependency in target.get("dependencies", [])
                }
                if "AstraSchedulerTestRuntime" not in dependencies:
                    offenders[name] = sorted(dependencies)

            self.assertEqual(
                offenders,
                {},
                "shared internal tests must reuse the single static test runtime",
            )


class PackageConsumerExecutionTests(unittest.TestCase):
    """Repeated rule gates must share one successful run per consumer binary."""

    def test_each_consumer_binary_runs_once(self):
        with tempfile.TemporaryDirectory(prefix="astra-consumer-cache-") as temp:
            workdir = Path(temp)
            counter = workdir / "runs.txt"
            consumer = workdir / "consumer"
            consumer.write_text(
                "#!/bin/sh\nprintf x >> '" + str(counter) + "'\n",
                encoding="utf-8",
            )
            consumer.chmod(0o755)

            first = check_cmake_package.run_consumer_once(consumer)
            second = check_cmake_package.run_consumer_once(consumer)

            self.assertEqual(first, second)
            self.assertEqual(counter.read_text(encoding="utf-8"), "x")


if __name__ == "__main__":
    unittest.main(verbosity=2)
