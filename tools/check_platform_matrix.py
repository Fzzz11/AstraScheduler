#!/usr/bin/env python3
"""AstraScheduler Linux-only Tier matrix 与部署不变量审计（AST-052 / R-111 / R-107）。

验证：
  1. 平台矩阵只包含 Linux x86_64 GCC/Clang（Tier-1）与 native Linux AArch64
     （Tier-2）；Windows/MSVC、macOS、其他非 Linux OS 与 32-bit 均为 unsupported。
  2. export.hpp 的 non-Linux/32-bit/工具链/异常编译护栏存在（偶然编译成功不升级支持状态）。
  3. CMake 构建与 package 支持声明不含 Windows/MSVC 专用 release 配置。
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MATRIX = REPO / "tools" / "platform_matrix.json"
EXPORT_HPP = REPO / "include" / "astra" / "export.hpp"
PACKAGE_IN = REPO / "cmake" / "AstraSchedulerConfig.cmake.in"

SUPPORTED_TIER1 = "linux-x86_64"
SUPPORTED_TIER2 = "linux-aarch64"


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def main() -> int:
    # --- 1. matrix 内容审计 ---
    matrix = json.loads(MATRIX.read_text(encoding="utf-8"))
    tier1 = matrix.get("tier_1", [])
    tier2 = matrix.get("tier_2", [])
    unsupported = matrix.get("unsupported", [])
    if [t.get("platform") for t in tier1] != [SUPPORTED_TIER1]:
        fail(f"tier_1 must be exactly [{SUPPORTED_TIER1}]")
    tier1_compilers = [c for t in tier1 for c in t.get("compilers", [])]
    if not any("gcc>=13" in c for c in tier1_compilers) or not any(
        "clang>=17" in c for c in tier1_compilers
    ):
        fail("tier_1 must gate gcc>=13 and clang>=17")
    if [t.get("platform") for t in tier2] != [SUPPORTED_TIER2]:
        fail(f"tier_2 must be exactly [{SUPPORTED_TIER2}]")
    unsupported_platforms = {u.get("platform") for u in unsupported}
    for required in ("windows-msvc", "macos", "other-non-linux", "32-bit"):
        if required not in unsupported_platforms:
            fail(f"unsupported matrix must reject {required}")

    # --- 2. export.hpp 编译护栏审计（R-111 的 compile-time 执行）---
    export_src = EXPORT_HPP.read_text(encoding="utf-8")
    for guard in ("!defined(__linux__)", "__SIZEOF_POINTER__ != 8", "-fno-exceptions"):
        if guard not in export_src:
            fail(f"export.hpp missing compile guard for {guard!r}")

    # --- 3. package/构建声明审计：不存在 Windows/MSVC release 配置 ---
    package_src = PACKAGE_IN.read_text(encoding="utf-8")
    if "linux" not in package_src.lower():
        fail("package support declaration must state Linux-only supported configuration")
    cmake_files = list(REPO.glob("CMakeLists.txt")) + list((REPO / "cmake").glob("*")) + list(
        (REPO / "benchmarks").glob("CMakeLists.txt")
    )
    for f in cmake_files:
        text = f.read_text(encoding="utf-8", errors="ignore")
        for banned in ("MSVC", "WIN32"):
            if banned in text:
                fail(f"{f.name} contains Windows/MSVC-specific declaration {banned!r}")

    print("platform matrix audit ok: Linux-only tier matrix, no Windows/MSVC release declarations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
