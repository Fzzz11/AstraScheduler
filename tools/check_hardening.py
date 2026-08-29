#!/usr/bin/env python3
"""AstraScheduler Tier-1 hardening 证据编排（AST-053 / R-111 / D-167）。

构建并运行三套 sanitizer 证据（每套 = 全新 configure + build + 全量 ctest）：
  - ASan+UBSan（ASTRA_ENABLE_SANITIZERS=ON）
  - TSan（ASTRA_ENABLE_TSAN=ON，与 ASan/UBSan 互斥）
并聚合既有审计：平台矩阵（check_platform_matrix）、包效率
（check_package_efficiency）、package consumer（可选 --with-package）。

Tier-2（native Linux AArch64 weak-memory）：仅当运行宿主为 native aarch64 时
在本机执行 weak-memory stress；x86_64 宿主显式记录 deferred（不伪造证据）。
结果写入 docs/hardening-evidence.json；任何 suite 失败 → exit 1。
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
EVIDENCE_PATH = REPO_ROOT / "docs" / "hardening-evidence.json"


def run(command: list, env: dict | None = None, timeout: int = 1800) -> dict:
    merged_env = None
    if env:
        merged_env = dict(os.environ)
        merged_env.update(env)
    started = time.time()
    proc = subprocess.run(
        command,
        cwd=str(REPO_ROOT),
        env=merged_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
    )
    return {
        "command": " ".join(str(c) for c in command),
        "exit_code": proc.returncode,
        "duration_s": round(time.time() - started, 1),
        "output_tail": proc.stdout[-4000:],
    }


def sanitizer_suite(label: str, cmake_option: list, jobs: int) -> dict:
    """全新 configure/build + 全量 ctest 的 sanitizer 证据套件。"""
    workdir = Path(tempfile.mkdtemp(prefix=f"astra-hardening-{label}-"))
    steps = []
    configure = run(
        ["cmake", "-S", str(REPO_ROOT), "-B", str(workdir),
         "-DCMAKE_BUILD_TYPE=Debug", *cmake_option,
         f"-DCMAKE_BUILD_PARALLEL_LEVEL={jobs}"],
    )
    steps.append({"step": "configure", **configure})
    build = run(["cmake", "--build", str(workdir), "--parallel", str(jobs)])
    steps.append({"step": "build", **build})
    ctest = run(["ctest", "--test-dir", str(workdir), "--output-on-failure"])
    steps.append({"step": "ctest", **ctest})

    ok = all(s["exit_code"] == 0 for s in steps)
    passed = 0
    total = 0
    for line in ctest["output_tail"].splitlines():
        if "tests passed" in line and "out of" in line:
            parts = line.replace("% tests passed", "").split()
            try:
                passed, total = int(parts[-4]), int(parts[-1].rstrip())
            except (ValueError, IndexError):
                pass
    return {
        "suite": label,
        "ok": ok,
        "tests_passed": passed,
        "tests_total": total,
        "steps": steps,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="AST-053 hardening evidence orchestrator")
    parser.add_argument("--jobs", type=int, default=4, help="build parallelism (bounded)")
    parser.add_argument("--with-package", action="store_true",
                        help="also run the full CMake package consumer check")
    parser.add_argument("--skip-tsan", action="store_true", help="skip the TSan suite")
    args = parser.parse_args()

    machine = platform.machine()
    evidence = {
        "schema_version": 1,
        "tool": "check_hardening",
        "runner_machine": machine,
        "tier_1": {},
        "tier_2": {
            "platform": "linux-aarch64",
            "status": "deferred",
            "reason": (
                "native Linux AArch64 runner unavailable on this host; "
                "weak-memory stress carrier tests/test_weak_memory_stress.cpp is "
                "provided and must run on native AArch64 hardware (D-167)"
            ),
        },
        "audits": {},
    }

    suites = [("asan-ubsan", ["-DASTRA_ENABLE_SANITIZERS=ON"])]
    if not args.skip_tsan:
        suites.append(("tsan", ["-DASTRA_ENABLE_TSAN=ON"]))

    for label, option in suites:
        print(f"[hardening] running suite: {label} ...", flush=True)
        result = sanitizer_suite(label, option, args.jobs)
        evidence["tier_1"][label] = result
        print(f"[hardening] suite {label}: "
              f"{'OK' if result['ok'] else 'FAIL'} "
              f"({result['tests_passed']}/{result['tests_total']} tests)", flush=True)

    evidence["audits"]["platform_matrix"] = run(
        ["python3", "tools/check_platform_matrix.py"])
    evidence["audits"]["package_efficiency"] = run(
        ["python3", "tools/check_package_efficiency.py"])
    if args.with_package:
        evidence["audits"]["package_consumer"] = run(
            ["python3", "tools/check_cmake_package.py"], timeout=3600)

    if machine == "aarch64":
        stress = run(["ctest", "--test-dir", "build/wsl-gcc-debug",
                      "-R", "astra_weak_memory_stress_test", "--output-on-failure"])
        evidence["tier_2"] = {
            "platform": "linux-aarch64",
            "status": "executed",
            "stress_test": stress,
        }

    audits_ok = all(a["exit_code"] == 0 for a in evidence["audits"].values())
    suites_ok = all(s["ok"] for s in evidence["tier_1"].values())
    evidence["verdict"] = "ok" if (audits_ok and suites_ok) else "fail"

    EVIDENCE_PATH.write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(f"[hardening] verdict: {evidence['verdict']} (evidence: {EVIDENCE_PATH})")
    return 0 if evidence["verdict"] == "ok" else 1


if __name__ == "__main__":
    sys.exit(main())
