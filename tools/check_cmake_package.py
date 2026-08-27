"""AST-002 CMake package 门禁：编译、安装并验证独立 Linux consumer smoke。

Source ticket: .scratch/astra-scheduler-runtime/issues/02-cmake-package.md
Primary rules: R-110（primary）、R-111（supporting）。

这些测试在 WSL/Linux 中构建 C++20 compiled library、安装为可消费的
CMake package，并用仓库外的独立最小 consumer 工程执行 find_package/
link/run smoke；同时审计 install 布局、符号可见性与 consumer compile
line 不携带内部诊断选项或私有 include 路径。

本地运行（R-112：必须在 WSL Linux 用户空间执行）：

    wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"
CONSUMER_TEMPLATE = REPO_ROOT / "tests" / "consumer"
CXX = os.environ.get("CXX", "g++")

# install 树中允许出现的文件后缀（R-111：仅 Linux 产物）。
FORBIDDEN_INSTALL_SUFFIXES = (".dll", ".lib", ".exe")


def run_checked(command, cwd=None, context=""):
    """运行外部命令；失败时抛出带完整输出的断言错误。"""
    proc = subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if proc.returncode != 0:
        raise AssertionError(
            f"command failed ({context or ' '.join(command[:3])}):\n"
            f"$ {' '.join(str(part) for part in command)}\n{proc.stdout}"
        )
    return proc.stdout


class PackageBuildFixture(unittest.TestCase):
    """公共 fixture：在仓库外构建并安装 static/shared 两个变体与两个 consumer。"""

    workdir = None

    @classmethod
    def setUpClass(cls):
        # RED 失败点：仓库尚未提供 compiled library / CMake package。
        if not ROOT_CMAKE.is_file():
            raise FileNotFoundError(
                f"required CMake project is missing: {ROOT_CMAKE}"
            )
        if not (CONSUMER_TEMPLATE / "main.cpp").is_file():
            raise FileNotFoundError(
                f"consumer template is missing: {CONSUMER_TEMPLATE}"
            )

        cls.workdir = Path(tempfile.mkdtemp(prefix="astra-ast002-"))

        # static（默认变体）与 shared（BUILD_SHARED_LIBS=ON 可选变体）
        # 共用同一套 in-tree tests（R-110：static/shared 共用语义/tests）。
        cls.static_build = cls.workdir / "build-static"
        cls.static_install = cls.workdir / "install-static"
        cls._cmake_build_install(cls.static_build, cls.static_install, (), "static")

        cls.shared_build = cls.workdir / "build-shared"
        cls.shared_install = cls.workdir / "install-shared"
        cls._cmake_build_install(
            cls.shared_build, cls.shared_install,
            ("-DBUILD_SHARED_LIBS=ON",), "shared",
        )

        # 仓库外独立 consumer（static 与 shared 各一）
        cls.static_consumer = cls._build_consumer(cls.static_install, "consumer-static")
        cls.shared_consumer = cls._build_consumer(cls.shared_install, "consumer-shared")

    @classmethod
    def _cmake_build_install(cls, build, install, extra_args, label):
        """configure → build → ctest → install 的统一构建流程。"""
        run_checked(
            ["cmake", "-S", str(REPO_ROOT), "-B", str(build),
             "-DCMAKE_BUILD_TYPE=Release", *extra_args],
            context=f"configure {label}",
        )
        run_checked(
            ["cmake", "--build", str(build), "--parallel"],
            context=f"build {label}",
        )
        run_checked(
            ["ctest", "--test-dir", str(build), "--output-on-failure"],
            context=f"ctest {label}",
        )
        run_checked(
            ["cmake", "--install", str(build), "--prefix", str(install)],
            context=f"install {label}",
        )

    @classmethod
    def _build_consumer(cls, prefix, name):
        """把 consumer 模板复制到仓库外并构建、运行 smoke。"""
        source = cls.workdir / name
        shutil.copytree(CONSUMER_TEMPLATE, source)
        build = source / "build"
        run_checked(
            ["cmake", "-S", str(source), "-B", str(build),
             "-DCMAKE_PREFIX_PATH=" + str(prefix),
             "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"],
            context=f"configure {name}",
        )
        run_checked(
            ["cmake", "--build", str(build)],
            context=f"build {name}",
        )
        return source

    @classmethod
    def tearDownClass(cls):
        if cls.workdir is not None:
            shutil.rmtree(cls.workdir, ignore_errors=True)

    def _consumer_binary(self, consumer):
        binary = consumer / "build" / "consumer"
        self.assertTrue(binary.is_file(), f"consumer binary missing: {binary}")
        return binary

    def _run_consumer(self, consumer):
        run_checked([str(self._consumer_binary(consumer))], context="run consumer")

    def _compile_commands(self, consumer):
        path = consumer / "build" / "compile_commands.json"
        self.assertTrue(path.is_file(), f"compile_commands.json missing: {path}")
        return json.loads(path.read_text(encoding="utf-8"))

    def _library(self, install, filename):
        library = install / "lib" / filename
        self.assertTrue(library.is_file(), f"library missing: {library}")
        return library

    def _install_files(self, install):
        return [path for path in install.rglob("*") if path.is_file()]


class R110PackageConsumerGates(PackageBuildFixture):
    """R-110：CMake package 隐藏实现并验证独立 consumer。"""

    def test_R110_public_headers_install_only_under_include_astra(self):
        header = self.static_install / "include" / "astra" / "export.hpp"
        self.assertTrue(header.is_file(), f"public header missing: {header}")
        allowed_prefixes = (("include", "astra"), ("lib",), ("bin",))
        for path in self._install_files(self.static_install):
            parts = path.relative_to(self.static_install).parts
            self.assertNotEqual(
                path.suffix, ".cpp",
                f"implementation source leaked into install tree: {path}",
            )
            self.assertTrue(
                any(parts[:len(prefix)] == prefix for prefix in allowed_prefixes),
                f"file installed outside include/astra|lib|bin layout: {path}",
            )

    def test_R110_package_exports_config_targets_and_version_files(self):
        cmake_dir = self.static_install / "lib" / "cmake" / "AstraScheduler"
        for filename in (
            "AstraSchedulerConfig.cmake",
            "AstraSchedulerTargets.cmake",
            "AstraSchedulerConfigVersion.cmake",
        ):
            self.assertTrue(
                (cmake_dir / filename).is_file(),
                f"CMake package file missing: {cmake_dir / filename}",
            )
        targets = (cmake_dir / "AstraSchedulerTargets.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn("AstraScheduler::AstraScheduler", targets)

    def test_R110_independent_consumer_smoke_runs_static(self):
        self._run_consumer(self.static_consumer)

    def test_R110_independent_consumer_smoke_runs_shared(self):
        self._run_consumer(self.shared_consumer)

    def test_R110_consumer_compile_line_carries_no_internal_options_or_paths(self):
        for consumer in (self.static_consumer, self.shared_consumer):
            commands = self._compile_commands(consumer)
            self.assertTrue(commands, "consumer recorded no compile commands")
            for entry in commands:
                command = entry.get("command", "")
                self.assertNotIn(
                    "-Werror", command,
                    f"warnings-as-errors leaked to consumer: {command}",
                )
                self.assertNotRegex(
                    command, r"-fsanitize=\w+",
                    f"sanitizer flags leaked to consumer: {command}",
                )
                self.assertNotIn(
                    "/src/", command,
                    f"private implementation include leaked: {command}",
                )

    def test_R110_consumer_compile_line_uses_cxx20(self):
        for consumer in (self.static_consumer, self.shared_consumer):
            for entry in self._compile_commands(consumer):
                self.assertRegex(
                    entry.get("command", ""), r"-std=(c|gnu)\+\+20",
                    f"consumer must be compiled as C++20 (cxx_std_20): "
                    f"{entry.get('command', '')}",
                )

    def test_R110_shared_library_hides_internal_symbols(self):
        shared = self._library(self.shared_install, "libAstraScheduler.so")
        dynamic = run_checked(
            ["nm", "-D", "--defined-only", str(shared)],
            context="nm dynamic symbols",
        )
        self.assertIn(
            "package_probe_exported", dynamic,
            "export macro failed to publish the exported probe symbol",
        )
        self.assertIsNone(
            re.search(r"package_probe[^_]", dynamic),
            "internal (hidden) symbol leaked into the shared dynamic symbol "
            f"table: {dynamic}",
        )

    def test_R110_public_header_rejects_fno_exceptions(self):
        probe = self.workdir / "fno_exceptions_probe.cpp"
        probe.write_text("#include <astra/export.hpp>\n", encoding="utf-8")
        proc = subprocess.run(
            [CXX, "-std=c++20", "-fno-exceptions", "-fsyntax-only",
             "-I", str(self.static_install / "include"), str(probe)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertNotEqual(
            proc.returncode, 0,
            "public headers must reject -fno-exceptions builds (R-110)",
        )


class R111LinuxOnlyVariantGates(PackageBuildFixture):
    """R-111（supporting）：static 默认、shared 可选，仅 Linux install/consume。"""

    def test_R111_static_is_default_variant(self):
        self._library(self.static_install, "libAstraScheduler.a")
        self.assertFalse(
            (self.static_install / "lib" / "libAstraScheduler.so").exists(),
            "default build must produce the static variant only",
        )

    def test_R111_shared_is_opt_in_variant(self):
        self._library(self.shared_install, "libAstraScheduler.so")

    def test_R111_install_tree_contains_only_linux_artifacts(self):
        for install in (self.static_install, self.shared_install):
            for path in self._install_files(install):
                self.assertNotIn(
                    path.suffix, FORBIDDEN_INSTALL_SUFFIXES,
                    f"non-Linux artifact in install tree: {path}",
                )

    def test_R111_public_header_rejects_non_linux_compilation(self):
        # RED 负向用例：在非 Linux（移除预定义 __linux__）下 include public
        # header 必须编译失败（错误声明非 Linux 支持时该 gate 失败）。
        probe = self.workdir / "platform_probe.cpp"
        probe.write_text("#include <astra/export.hpp>\n", encoding="utf-8")
        include = ["-I", str(self.static_install / "include")]
        proc = subprocess.run(
            [CXX, "-std=c++20", "-fsyntax-only", "-U__linux__", *include,
             str(probe)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertNotEqual(
            proc.returncode, 0,
            "public headers must reject non-Linux compilation (R-111)",
        )

    def test_R111_public_header_rejects_32bit_compilation(self):
        # RED 负向用例：32-bit（-m32）下 __SIZEOF_POINTER__ != 8 必须触发
        # #error；无 multilib 时该编译同样失败，仅失败原因不同。
        probe = self.workdir / "platform32_probe.cpp"
        probe.write_text("#include <astra/export.hpp>\n", encoding="utf-8")
        include = ["-I", str(self.static_install / "include")]
        proc = subprocess.run(
            [CXX, "-std=c++20", "-m32", "-fsyntax-only", *include,
             str(probe)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertNotEqual(
            proc.returncode, 0,
            "public headers must reject 32-bit compilation (R-111)",
        )

    def test_R111_static_and_shared_both_install_and_consume(self):
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


if __name__ == "__main__":
    unittest.main(verbosity=2)
