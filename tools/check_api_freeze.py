#!/usr/bin/env python3
"""AstraScheduler semantic public surface gate（AST-057 / R-113）。

对全新 configure/build/install 的产物计算 documented public contract 指纹并与
当前版本 manifest 比对：
  - installed public header 名称（不冻结实现文本）；
  - shared library 导出的非 detail/internal astra 符号；
  - 可独立编译的 canonical public consumer contract；
  - 版本三元组（CMake project VERSION、installed version.hpp 宏、CMake
    version file 三处一致性，R-093 单一版本源）。
另执行 public header 自包含编译矩阵（每个 header 单独 -fsyntax-only）。

用法：
  python3 tools/check_api_freeze.py                  # 比对 golden（gate 模式）
  python3 tools/check_api_freeze.py --update-golden  # 重新生成 golden（需 review）
  python3 tools/check_api_freeze.py --skip-compile-matrix

退出码：0 = 表面未漂移；1 = 表面漂移/编译矩阵失败；2 = 输入错误。
本工具不承诺跨 toolchain ABI（R-093/D-145）——冻结的是 source/semantic surface。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"
PUBLIC_CONTRACT = REPO_ROOT / "tools" / "api_manifest" / "public_contract.cpp"

SYMBOL_REGEX = r"_ZN5astra\w+|_ZNK5astra\w+|_ZNKR5astra\w+|_ZNO5astra\w+"


def run(command: list, context: str) -> str:
    proc = subprocess.run(
        command, cwd=str(REPO_ROOT), stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True,
    )
    if proc.returncode != 0:
        print(f"FAIL: command failed ({context}):\n$ {' '.join(command)}\n{proc.stdout[-3000:]}")
        raise SystemExit(1)
    return proc.stdout


def sha256_of(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_and_install(workdir: Path) -> tuple[Path, Path]:
    """static install 提供 headers；shared install 提供导出符号表。"""
    build = workdir / "build-static"
    install = workdir / "install-static"
    run(["cmake", "-S", str(REPO_ROOT), "-B", str(build),
         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_BUILD_PARALLEL_LEVEL=2"], "configure static")
    run(["cmake", "--build", str(build), "--parallel", "2"], "build static")
    run(["cmake", "--install", str(build), "--prefix", str(install)], "install static")

    shared_build = workdir / "build-shared"
    shared_install = workdir / "install-shared"
    run(["cmake", "-S", str(REPO_ROOT), "-B", str(shared_build),
         "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON"], "configure shared")
    run(["cmake", "--build", str(shared_build), "--parallel", "2"], "build shared")
    run(["cmake", "--install", str(shared_build), "--prefix", str(shared_install)], "install shared")
    return install, shared_install


def compute_fingerprint(static_install: Path, shared_install: Path) -> dict:
    include_dir = static_install / "include" / "astra"
    headers = sorted(header.name for header in include_dir.glob("*.hpp"))

    shared = shared_install / "lib" / "libAstraScheduler.so"
    nm = run(["nm", "-D", "--defined-only", str(shared)], "nm shared library")
    symbols = sorted(set(
        symbol for symbol in re.findall(SYMBOL_REGEX, nm)
        if "6detail" not in symbol
        and "9Scheduler4Impl" not in symbol
        and "14AwaitHandshake" not in symbol
        and "4Impl" not in symbol
        and "_internal" not in symbol
        and "8run_impl" not in symbol
        and "12cancel_timer" not in symbol
        and "14register_timer" not in symbol
        and "16allocate_task_id" not in symbol
        and "17acquire_admission" not in symbol
        and "17post_task_invoker" not in symbol
        and "22rollback_external_slot" not in symbol
    ))

    version_hpp = (include_dir / "version.hpp").read_text(encoding="utf-8")
    macros = {}
    for name in ("ASTRA_VERSION_MAJOR", "ASTRA_VERSION_MINOR", "ASTRA_VERSION_PATCH"):
        m = re.search(rf"#define {name} (\d+)", version_hpp)
        macros[name] = int(m.group(1)) if m else None

    cmake_version_file = (static_install / "lib" / "cmake" / "AstraScheduler" /
                          "AstraSchedulerConfigVersion.cmake").read_text(encoding="utf-8")
    m = re.search(r'set\(PACKAGE_VERSION "([\d.]+)"\)', cmake_version_file)
    package_version = m.group(1) if m else "unknown"

    return {
        "schema_version": 2,
        "headers": headers,
        "documented_exported_symbols": symbols,
        "public_contract_sha256": sha256_of(PUBLIC_CONTRACT.read_bytes()),
        "version": {
            "macros": macros,
            "cmake_package_version": package_version,
        },
    }


def check_version_consistency(fingerprint: dict) -> list[str]:
    problems = []
    macros = fingerprint["version"]["macros"]
    package_version = fingerprint["version"]["cmake_package_version"]
    triple = f"{macros['ASTRA_VERSION_MAJOR']}.{macros['ASTRA_VERSION_MINOR']}.{macros['ASTRA_VERSION_PATCH']}"
    if triple != package_version:
        problems.append(
            f"version drift: version.hpp macros={triple} vs CMake package version={package_version}")
    if None in macros.values():
        problems.append("version.hpp macros missing")
    cmake_text = ROOT_CMAKE.read_text(encoding="utf-8")
    m = re.search(r"project\(AstraScheduler\s+VERSION\s+([\d.]+)", cmake_text)
    if not m or m.group(1) != triple:
        problems.append(
            f"project VERSION drift: CMakeLists={m.group(1) if m else 'missing'} vs headers={triple}")
    return problems


def compile_matrix(static_install: Path, workdir: Path) -> list[str]:
    """每个 installed header 单独 -fsyntax-only（自包含性编译矩阵）。"""
    problems = []
    include = ["-I", str(static_install / "include")]
    probe = workdir / "header_probe.cpp"
    for header in sorted((static_install / "include" / "astra").glob("*.hpp")):
        probe.write_text(f"#include <astra/{header.name}>\n", encoding="utf-8")
        proc = subprocess.run(
            ["g++", "-std=c++20", "-fsyntax-only", *include, str(probe)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        if proc.returncode != 0:
            problems.append(f"header not self-contained: astra/{header.name}\n{proc.stdout[-500:]}")
    return problems


def compile_public_contract(static_install: Path) -> list[str]:
    proc = subprocess.run(
        ["g++", "-std=c++20", "-fsyntax-only", "-I", str(static_install / "include"),
         str(PUBLIC_CONTRACT)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    if proc.returncode != 0:
        return [f"public consumer contract failed to compile:\n{proc.stdout[-1500:]}"]
    return []


def project_version() -> str:
    text = ROOT_CMAKE.read_text(encoding="utf-8")
    match = re.search(r"project\(AstraScheduler\s+VERSION\s+([\d.]+)", text)
    if not match:
        raise SystemExit("FAIL: project VERSION missing")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description="AST-054 API freeze gate")
    parser.add_argument("--update-golden", action="store_true",
                        help="regenerate the golden manifest (requires review)")
    parser.add_argument("--skip-compile-matrix", action="store_true")
    args = parser.parse_args()

    workdir = Path(tempfile.mkdtemp(prefix="astra-api-freeze-"))
    static_install, shared_install = build_and_install(workdir)
    fingerprint = compute_fingerprint(static_install, shared_install)

    problems = check_version_consistency(fingerprint)

    golden_path = REPO_ROOT / "tools" / "api_manifest" / f"v{project_version()}.json"

    if args.update_golden:
        if golden_path.exists():
            print(f"FAIL: refusing to overwrite release manifest: {golden_path}")
            return 2
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        golden_path.write_text(json.dumps(fingerprint, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
        print(f"new version manifest created: {golden_path}")
        for p in problems:
            print(f"VERSION PROBLEM: {p}")
        return 1 if problems else 0

    if not golden_path.is_file():
        print(f"FAIL: golden manifest missing: {golden_path}")
        return 2

    golden = json.loads(golden_path.read_text(encoding="utf-8"))

    if golden.get("schema_version") != 2:
        print("FAIL: golden manifest schema_version mismatch")
        return 2

    drift = []
    old_headers = set(golden.get("headers", []))
    new_headers = set(fingerprint["headers"])
    for name in sorted(set(old_headers) | set(new_headers)):
        if name not in new_headers:
            drift.append(f"header REMOVED: astra/{name}")
        elif name not in old_headers:
            drift.append(f"header ADDED: astra/{name}")

    old_syms = set(golden.get("documented_exported_symbols", []))
    new_syms = set(fingerprint["documented_exported_symbols"])
    for sym in sorted(old_syms - new_syms):
        drift.append(f"symbol REMOVED: {sym}")
    for sym in sorted(new_syms - old_syms):
        drift.append(f"symbol ADDED: {sym}")

    if golden.get("version", {}).get("macros") != fingerprint["version"]["macros"]:
        drift.append("version macros drifted")
    if golden.get("public_contract_sha256") != fingerprint["public_contract_sha256"]:
        drift.append("documented public consumer contract changed")

    for p in problems:
        drift.append(p)

    if args.skip_compile_matrix:
        matrix_problems: list = []
    else:
        matrix_problems = compile_matrix(static_install, workdir)
        matrix_problems += compile_public_contract(static_install)
        for mp in matrix_problems:
            drift.append(mp)

    if drift or matrix_problems:
        print(f"FAIL: public surface drift detected ({len(drift)} finding(s)):")
        for d in drift:
            print(f"  - {d}")
        print("任何未记录的 surface 漂移都使 gate 失败（AST-054/R-093）。")
        return 1

    print(f"API freeze gate OK: {len(new_headers)} headers, "
          f"{len(new_syms)} documented exported symbols and consumer contract unchanged.")
    shutil.rmtree(str(workdir), True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
