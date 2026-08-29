"""AstraScheduler CMake package 门禁：编译、安装并验证独立 Linux consumer smoke。

Source tickets:
- .scratch/astra-scheduler-runtime/issues/02-cmake-package.md（R-110 primary、R-111 supporting）
- .scratch/astra-scheduler-runtime/issues/03-version-contract.md（R-093 primary）
- .scratch/astra-scheduler-runtime/issues/04-scheduler-public-contract.md（R-098, R-099, R-100, R-101 primary）
- .scratch/astra-scheduler-runtime/issues/05-startup-transaction.md（R-023, R-024, R-097 primary）
- .scratch/astra-scheduler-runtime/issues/06-runtime-state-handoff.md（R-020, R-021, R-022 primary）
- .scratch/astra-scheduler-runtime/issues/07-reaper-coordinator.md（R-025, R-026, R-028, R-107 primary）
- .scratch/astra-scheduler-runtime/issues/08-global-worker-runtime.md（R-001, R-002 primary）
- .scratch/astra-scheduler-runtime/issues/09-move-only-submit.md（R-048, R-058, R-102 primary）

这些测试在 WSL/Linux 中构建 C++20 compiled library、安装为可消费的
CMake package，并用仓库外的独立最小 consumer 工程执行 find_package/
link/run smoke；同时审计 install 布局、符号可见性与 consumer compile
line 不携带内部诊断选项或私有 include 路径。R-093 增加版本契约门禁：
同一安装 header/library 版本一致、查询无副作用、CMake exact-version
边界拒绝错误版本请求、手工链接时篡改 header 的 mismatch 可被诊断。

本地运行（R-112：必须在 WSL Linux 用户空间执行）：

    wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_cmake_package.py"
"""

from __future__ import annotations

import atexit
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from collections import namedtuple
from functools import lru_cache
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"
CONSUMER_TEMPLATE = REPO_ROOT / "tests" / "consumer"
CXX = os.environ.get("CXX", "g++")
DEFAULT_BUILD_JOBS = 2

# install 树中允许出现的文件后缀（R-111：仅 Linux 产物）。
FORBIDDEN_INSTALL_SUFFIXES = (".dll", ".lib", ".exe")

# 单一版本源解析结果（project VERSION 三元组）。
ProjectVersion = namedtuple("ProjectVersion", "major minor patch")


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


def cmake_build_command(build):
    """Return a CMake build command with an explicit, memory-safe job limit."""
    raw_jobs = os.environ.get(
        "CMAKE_BUILD_PARALLEL_LEVEL", str(DEFAULT_BUILD_JOBS)
    )
    try:
        jobs = int(raw_jobs)
    except ValueError as exc:
        raise ValueError(
            "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer"
        ) from exc
    if jobs < 1:
        raise ValueError(
            "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer"
        )
    return ["cmake", "--build", str(build), "--parallel", str(jobs)]


@lru_cache(maxsize=None)
def run_consumer_once(binary):
    """Run one built consumer once and share its successful result across gates."""
    binary = Path(binary)
    if not binary.is_file():
        raise FileNotFoundError(f"consumer binary missing: {binary}")
    return run_checked([str(binary)], context=f"run consumer {binary}")


def _cmake_build_install(build, install, extra_args, label):
    """configure → build → ctest → install 的统一构建流程。"""
    run_checked(
        ["cmake", "-S", str(REPO_ROOT), "-B", str(build),
         "-DCMAKE_BUILD_TYPE=Release", *extra_args],
        context=f"configure {label}",
    )
    run_checked(
        cmake_build_command(build),
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


def _build_consumer(workdir, prefix, name):
    """把 consumer 模板复制到仓库外并构建 smoke。"""
    source = workdir / name
    shutil.copytree(CONSUMER_TEMPLATE, source)
    build = source / "build"
    run_checked(
        ["cmake", "-S", str(source), "-B", str(build),
         "-DCMAKE_PREFIX_PATH=" + str(prefix),
         "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"],
        context=f"configure {name}",
    )
    run_checked(
        cmake_build_command(build),
        context=f"build {name}",
    )
    return source


# 模块级共享构建缓存：多个测试类复用同一次 build/install/consumer；
# 成功与失败都只执行一次，进程退出时经 atexit 清理临时目录。
_SHARED_BUILDS = None
_SHARED_BUILD_ERROR = None


def _create_shared_builds():
    # RED 失败点：仓库尚未提供 compiled library / CMake package。
    if not ROOT_CMAKE.is_file():
        raise FileNotFoundError(
            f"required CMake project is missing: {ROOT_CMAKE}"
        )
    if not (CONSUMER_TEMPLATE / "main.cpp").is_file():
        raise FileNotFoundError(
            f"consumer template is missing: {CONSUMER_TEMPLATE}"
        )

    workdir = Path(tempfile.mkdtemp(prefix="astra-package-gates-"))
    atexit.register(shutil.rmtree, str(workdir), True)

    # static（默认变体）与 shared（BUILD_SHARED_LIBS=ON 可选变体）
    # 共用同一套 in-tree tests（R-110：static/shared 共用语义/tests）。
    static_build = workdir / "build-static"
    static_install = workdir / "install-static"
    _cmake_build_install(static_build, static_install, (), "static")

    shared_build = workdir / "build-shared"
    shared_install = workdir / "install-shared"
    _cmake_build_install(
        shared_build, shared_install, ("-DBUILD_SHARED_LIBS=ON",), "shared",
    )

    # 仓库外独立 consumer（static 与 shared 各一）
    static_consumer = _build_consumer(workdir, static_install, "consumer-static")
    shared_consumer = _build_consumer(workdir, shared_install, "consumer-shared")

    return {
        "workdir": workdir,
        "static_build": static_build,
        "static_install": static_install,
        "shared_build": shared_build,
        "shared_install": shared_install,
        "static_consumer": static_consumer,
        "shared_consumer": shared_consumer,
    }


def _get_shared_builds():
    global _SHARED_BUILDS, _SHARED_BUILD_ERROR
    if _SHARED_BUILDS is not None:
        return _SHARED_BUILDS
    if _SHARED_BUILD_ERROR is not None:
        raise _SHARED_BUILD_ERROR

    try:
        _SHARED_BUILDS = _create_shared_builds()
    except Exception as error:
        _SHARED_BUILD_ERROR = error
        raise
    return _SHARED_BUILDS


def setUpModule():
    """Build both package variants once before any rule-specific unittest runs."""
    _get_shared_builds()


class PackageBuildFixture(unittest.TestCase):
    """公共 fixture：复用模块级共享构建（仓库外 static/shared 变体与 consumer）。"""

    workdir = None

    @classmethod
    def setUpClass(cls):
        builds = _get_shared_builds()
        cls.workdir = builds["workdir"]
        cls.static_build = builds["static_build"]
        cls.static_install = builds["static_install"]
        cls.shared_build = builds["shared_build"]
        cls.shared_install = builds["shared_install"]
        cls.static_consumer = builds["static_consumer"]
        cls.shared_consumer = builds["shared_consumer"]

    def _consumer_binary(self, consumer):
        binary = consumer / "build" / "consumer"
        self.assertTrue(binary.is_file(), f"consumer binary missing: {binary}")
        return binary

    def _run_consumer(self, consumer):
        return run_consumer_once(self._consumer_binary(consumer))

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
        for header_name in (
            "export.hpp",
            "version.hpp",
            "capabilities.hpp",
            "id.hpp",
            "scheduler.hpp",
            "scheduler_options.hpp",
            "status.hpp",
            "error.hpp",
            "task_handle.hpp",
            "finalization.hpp",
        ):
            header = self.static_install / "include" / "astra" / header_name
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
        # AST-003 / AST-004 / AST-018 / AST-028 / AST-029：公开 API 经 export macro 导出，内部符号 hidden。
        expected_symbols = [
            "_ZN5astra15library_versionEv",
            "_ZN5astra22library_version_stringEv",
            "_ZN5astra24recommended_worker_countEv",
            "_ZN5astra18begin_finalizationEv",
            "_ZN5astra19FinalizationControlC1ESt10shared_ptrINS0_4ImplEE",
            "_ZN5astra19FinalizationControlC2ESt10shared_ptrINS0_4ImplEE",
            "_ZN5astra6detail25current_worker_runtime_idEv",
            "_ZN5astra6detail19perform_caller_waitERKNS0_19TaskSharedStateBaseESt8optionalINSt6chrono10time_pointINS5_3_V212steady_clockENS5_8durationIlSt5ratioILl1ELl1000000000EEEEEEE",
            "_ZN5astra6detail25TaskExecutionContextGuardC1ENS_6TaskIdE",
            "_ZN5astra6detail25TaskExecutionContextGuardC2ENS_6TaskIdE",
            "_ZN5astra6detail25TaskExecutionContextGuardD1Ev",
            "_ZN5astra6detail25TaskExecutionContextGuardD2Ev",
            "_ZN5astra9SchedulerC1ENS_16SchedulerOptionsE",
            "_ZN5astra9SchedulerC2ENS_16SchedulerOptionsE",
            "_ZN5astra9SchedulerC1ERKS0_",
            "_ZN5astra9SchedulerC2ERKS0_",
            "_ZN5astra9SchedulerC1EOS0_",
            "_ZN5astra9SchedulerC2EOS0_",
            "_ZN5astra9SchedulerD1Ev",
            "_ZN5astra9SchedulerD2Ev",
            "_ZN5astra9Scheduler8shutdownEv",
            "_ZN5astra9Scheduler12shutdown_nowEv",
            "_ZN5astra9Scheduler3runEONS_15FrozenTaskGraphE",
            "_ZN5astra9ScheduleraSERKS0_",
            "_ZN5astra9ScheduleraSEOS0_",
            "_ZNK5astra19FinalizationControl13wait_for_implENSt6chrono8durationIlSt5ratioILl1ELl1000000000EEEE",
            "_ZNK5astra19FinalizationControl17request_immediateEv",
            "_ZNK5astra19FinalizationControl4waitEv",
            "_ZNK5astra9Scheduler5validEv",
            "_ZNK5astra9Scheduler10runtime_idEv",
            "_ZNK5astra9Scheduler6statusEv",
            "_ZNK5astra9Scheduler12capabilitiesEv",
            "_ZNK5astra9Scheduler17acquire_admissionEbb",
            "_ZNK5astra9Scheduler17post_task_invokerESt10unique_ptrINS_6detail15TaskInvokerBaseESt14default_deleteIS3_EEb",
            "_ZNK5astra9Scheduler22rollback_external_slotEv",
            "_ZNK5astra8GraphRun10node_countEv",
            "_ZNK5astra8GraphRun12is_completedEv",
            "_ZNK5astra8GraphRun14request_cancelEv",
            "_ZNK5astra8GraphRun2idEv",
            "_ZNK5astra8GraphRun4waitEv",
            "_ZNK5astra8GraphRun5stateEv",
            "_ZNK5astra8GraphRun8wait_forENSt6chrono8durationIlSt5ratioILl1ELl1000000000EEEE",
            "_ZNKR5astra8GraphRun10get_reportEv",
            "_ZNO5astra9TaskGraph6freezeEv",
        ]
        for sym in expected_symbols:
            self.assertIn(
                sym, dynamic,
                f"export macro failed to publish {sym}",
            )
        # 动态表中 astra 命名空间导出符号必须仅为公开 API 集合；
        # 其余符号（含版本字符串静态存储、内部 Impl、Helper 等）保持 hidden。
        exported = re.findall(r"_ZN5astra\w+|_ZNK5astra\w+|_ZNKR5astra\w+|_ZNO5astra\w+", dynamic)
        self.assertCountEqual(
            exported,
            expected_symbols,
            f"unexpected astra dynamic symbols leaked: {dynamic}",
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


class R093VersionContractGates(PackageBuildFixture):
    """R-093：header/library 版本查询与 mismatch 诊断。"""

    def _project_version(self):
        """从 CMakeLists.txt 读取单一版本源（project VERSION）。"""
        text = ROOT_CMAKE.read_text(encoding="utf-8")
        match = re.search(
            r"project\(AstraScheduler\s+VERSION\s+(\d+)\.(\d+)\.(\d+)", text
        )
        self.assertIsNotNone(
            match, "CMakeLists.txt must declare project(AstraScheduler VERSION x.y.z)"
        )
        return ProjectVersion(
            int(match.group(1)), int(match.group(2)), int(match.group(3))
        )

    def test_R093_installed_version_sources_stay_consistent(self):
        # 单一版本源：installed header 宏、CMake version file 与 consumer
        # 模板的 exact-version 钉住值必须都来自 project VERSION（各生成/
        # 维护路径不得漂移；升级时钉住值随门禁失败一并更新）。
        major, minor, patch = self._project_version()
        header = (self.static_install / "include" / "astra" / "version.hpp").read_text(
            encoding="utf-8"
        )
        for name, value in (
            ("ASTRA_VERSION_MAJOR", major),
            ("ASTRA_VERSION_MINOR", minor),
            ("ASTRA_VERSION_PATCH", patch),
        ):
            self.assertRegex(
                header, rf"#define {name} {value}\b",
                f"installed version.hpp macro {name} drifted from project version",
            )
        version_file = (
            self.static_install / "lib" / "cmake" / "AstraScheduler"
            / "AstraSchedulerConfigVersion.cmake"
        ).read_text(encoding="utf-8")
        self.assertIn(
            f'set(PACKAGE_VERSION "{major}.{minor}.{patch}")', version_file,
            "CMake package version file drifted from project version",
        )
        consumer_template = (CONSUMER_TEMPLATE / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            f"find_package(AstraScheduler {major}.{minor}.{patch} REQUIRED)",
            consumer_template,
            "consumer template must pin the full version triple declared by "
            "project VERSION (R-093 exact-version boundary)",
        )

    def test_R093_same_install_consumers_verify_version_contract(self):
        # consumer 内部断言 header==library、查询不启动线程、string_view
        # 稳定且三元组一致；任一失败都会以非零退出。
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)

    def test_R093_find_package_rejects_version_mismatch_at_configure(self):
        # R-093：CMake exact-version 检查是主要 mismatch 边界——请求同
        # major 的更旧版本（0.x minor 可含 breaking change 的错误组合）
        # 必须在 configure 边界被拒绝。RED：宽版本兼容模式下该请求会被
        # 错误接受（纯净 HEAD 上请求 0.0.9 对 0.1.0 安装 configure 成功）。
        major, minor, patch = self._project_version()
        if patch > 0:
            requested = f"{major}.{minor}.{patch - 1}"
        elif minor > 0:
            requested = f"{major}.{minor - 1}.0"
        else:
            self.skipTest("no same-major older version exists to request")
        source = self.workdir / "consumer-mismatch-cmake"
        shutil.copytree(CONSUMER_TEMPLATE, source)
        cmake_lists = source / "CMakeLists.txt"
        cmake_lists.write_text(
            re.sub(
                r"find_package\(AstraScheduler [^)]*\)",
                f"find_package(AstraScheduler {requested} REQUIRED)",
                cmake_lists.read_text(encoding="utf-8"),
            ),
            encoding="utf-8",
        )
        proc = subprocess.run(
            ["cmake", "-S", str(source), "-B", str(source / "build"),
             "-DCMAKE_PREFIX_PATH=" + str(self.static_install)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertNotEqual(
            proc.returncode, 0,
            f"find_package(AstraScheduler {requested}) must be rejected at "
            f"configure time against installed {major}.{minor}.{patch} "
            f"(exact-version mismatch boundary):\n{proc.stdout}",
        )
        self.assertIn(
            "version", proc.stdout.lower(),
            f"configure failure must report the version mismatch:\n{proc.stdout}",
        )

    def test_R093_manual_link_detects_doctored_header_mismatch(self):
        # R-093 例外边界：绕过 CMake package 手工链接时，运行期
        # header/library 比较必须能发现被篡改的 header 组合（可诊断
        # 不等于受支持）。
        major, minor, patch = self._project_version()
        doctored_include = self.workdir / "doctored-include"
        shutil.copytree(self.static_install / "include", doctored_include)
        doctored_header = doctored_include / "astra" / "version.hpp"
        original = doctored_header.read_text(encoding="utf-8")
        doctored_header.write_text(
            re.sub(
                r"#define ASTRA_VERSION_MINOR \d+",
                "#define ASTRA_VERSION_MINOR 42",
                original,
            ),
            encoding="utf-8",
        )
        doctored_text = doctored_header.read_text(encoding="utf-8")
        self.assertIn(
            "#define ASTRA_VERSION_MINOR 42", doctored_text,
            "test fixture failed to doctor the installed header copy",
        )
        source = self.workdir / "version_mismatch_consumer.cpp"
        source.write_text(
            (CONSUMER_TEMPLATE / "version_mismatch.cpp").read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        binary = self.workdir / "version_mismatch_consumer"
        run_checked(
            [CXX, "-std=c++20", "-I", str(doctored_include), str(source),
             "-L", str(self.static_install / "lib"), "-lAstraScheduler",
             "-o", str(binary)],
            context="compile manual-link mismatch consumer",
        )
        proc = subprocess.run(
            [str(binary)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertEqual(
            proc.returncode, 1,
            f"doctored header/library combination must be diagnosed at "
            f"runtime:\n{proc.stdout}",
        )
        self.assertIn("mismatch", proc.stdout)
        self.assertIn("42", proc.stdout)
        self.assertIn(f"{major}.{minor}.{patch}", proc.stdout)


class AST004PublicContractGates(PackageBuildFixture):
    """AST-004：Scheduler 公共 options、状态、逻辑 ID 与 capability 契约（R-098/R-099/R-100/R-101）。"""

    def test_AST004_consumer_runs_all_public_contract_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-098/R-099/R-100/R-101 契约
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST005StartupTransactionGates(PackageBuildFixture):
    """AST-005：Scheduler startup transaction 与 Finalization gate 契约（R-023/R-024/R-097）。"""

    def test_AST005_consumer_runs_all_startup_transaction_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-023/R-024/R-097 与 error 异常契约
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST006RuntimeStateHandoffGates(PackageBuildFixture):
    """AST-006：解耦 Runtime State 并实现最后 Worker Handle handoff（R-020/R-021/R-022）。"""

    def test_AST006_consumer_runs_all_runtime_state_handoff_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-020/R-021/R-022
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST007ReaperCoordinatorGates(PackageBuildFixture):
    """AST-007：实现唯一 Reaper coordinator 的 pending/join/idle 循环（R-025/R-026/R-028/R-107）。"""

    def test_AST007_consumer_runs_all_reaper_coordinator_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-025/R-026/R-028/R-107
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST008GlobalWorkerRuntimeGates(PackageBuildFixture):
    """AST-008：交付 Global-only Worker Runtime 基线（R-001/R-002）。"""

    def test_AST008_consumer_runs_all_global_worker_runtime_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-001/R-002
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST009MoveOnlySubmitGates(PackageBuildFixture):
    """AST-009：实现 move-only submit 与共享 TaskHandle 基础面（R-048/R-058/R-102）。"""

    def test_AST009_consumer_runs_all_move_only_submit_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-048/R-058/R-102
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST010AdmissionBackpressureGates(PackageBuildFixture):
    """AST-010：实现 External Pending Capacity 与强 admission transaction（R-061/R-062）。"""

    def test_AST010_consumer_runs_all_admission_backpressure_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-061/R-062
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST011TaskOutcomeStateGates(PackageBuildFixture):
    """AST-011：发布一致的 TaskState、Terminal Outcome 与重复 get（R-049/R-050/R-051/R-057/R-060）。"""

    def test_AST011_consumer_runs_all_task_outcome_state_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-049/R-050/R-051/R-057/R-060
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST012HelpingWaitGates(PackageBuildFixture):
    """AST-012：实现 Unbounded/Helping wait 与 timeout 边界（R-052/R-055/R-056/R-059）。"""

    def test_AST012_consumer_runs_all_helping_wait_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-052/R-055/R-056/R-059
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST013TaskCancellationGates(PackageBuildFixture):
    """AST-013：实现显式 Task cancellation 的首次 start 分类（R-053/R-054）。"""

    def test_AST013_consumer_runs_all_task_cancellation_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-053/R-054
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST014GracefulDrainGates(PackageBuildFixture):
    """AST-014：实现 Graceful admission closure 与 Drain Work Closure（R-006/R-007/R-012/R-019）。"""

    def test_AST014_consumer_runs_all_graceful_drain_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-006/R-007/R-012/R-019
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST015ShutdownGuardsGates(PackageBuildFixture):
    """AST-015：实现 shutdown caller guard 与共享完成边界（R-010/R-011/R-013/R-016/R-108）。"""

    def test_AST015_consumer_runs_all_shutdown_guards_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-010/R-011/R-013/R-016/R-108
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST016ImmediateEscalationGates(PackageBuildFixture):
    """AST-016：实现单向 Immediate escalation 与启动状态分类（R-009/R-014/R-015/R-106）。"""

    def test_AST016_consumer_runs_all_immediate_escalation_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-009/R-014/R-015/R-106
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST017LastHandleRaiiGates(PackageBuildFixture):
    """AST-017：实现最后非 Worker Handle 的 noexcept 同步 RAII（R-103/R-105）。"""

    def test_AST017_consumer_runs_all_last_handle_raii_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-103/R-105
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST018FinalizationControlApiGates(PackageBuildFixture):
    """AST-018：固定 FinalizationControl 公共 capability surface（R-035/R-036/R-043/R-044/R-045/R-046）。"""

    def test_AST018_consumer_runs_all_finalization_control_api_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-035/R-036/R-043/R-044/R-045/R-046
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST019FinalizationBeginGates(PackageBuildFixture):
    """AST-019：实现 begin_finalization、核算集合与 startup 竞态（R-031/R-037/R-038/R-104）。"""

    def test_AST019_consumer_runs_all_finalization_begin_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-031/R-037/R-038/R-104
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST020FinalizationWaitGates(PackageBuildFixture):
    """AST-020：实现 Finalization 无界 wait、wait_for 与唯一 coordinator join（R-032/R-033/R-039/R-040/R-041/R-042）。"""

    def test_AST020_consumer_runs_all_finalization_wait_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-032/R-033/R-039/R-040/R-041/R-042
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST021FinalizationEscalationGates(PackageBuildFixture):
    """AST-021：实现 Finalization escalation 与控制面 fail-fast（R-034/R-047）。"""

    def test_AST021_consumer_runs_all_finalization_escalation_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-034/R-047
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST022LockedLocalRoutingGates(PackageBuildFixture):
    """AST-022：加入 Locked Local Deque 与 Ready Routing Precedence（R-063/R-101）。"""

    def test_AST022_consumer_runs_all_locked_local_routing_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-063/R-101
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST023StealRoundGates(PackageBuildFixture):
    """AST-023：实现 bounded non-repeating Steal Round（R-064）。"""

    def test_AST023_consumer_runs_all_steal_round_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-064
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST024ParkHandshakeGates(PackageBuildFixture):
    """AST-024：实现无丢唤醒 Park Handshake（R-065）。"""

    def test_AST024_consumer_runs_all_park_handshake_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-065
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST025ChaseLevOrderingGates(PackageBuildFixture):
    """AST-025：建立 Chase-Lev seq_cst oracle 与 portable memory order（R-066）。"""

    def test_AST025_consumer_runs_all_chase_lev_ordering_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-066
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST026ChaseLevGrowthGates(PackageBuildFixture):
    """AST-026：实现 Chase-Lev growth、旧 buffer retention 与单一调度引用（R-067）。"""

    def test_AST026_consumer_runs_all_chase_lev_growth_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-067
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST027ChaseLevIndicesGates(PackageBuildFixture):
    """AST-027：固定 Chase-Lev index 算术、边界状态与 backend truth（R-068 / R-101）。"""

    def test_AST027_consumer_runs_all_chase_lev_indices_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-068 / R-101
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST028TaskGraphFreezeGates(PackageBuildFixture):
    """AST-028：实现 consuming TaskGraph freeze 与 NodeId 验证（R-069）。"""

    def test_AST028_consumer_runs_all_task_graph_freeze_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-069
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST029GraphAdmissionGates(PackageBuildFixture):
    """AST-029：实现 GraphRun 原子 admission 与依赖发布（R-070）。"""

    def test_AST029_consumer_runs_all_graph_admission_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-070
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST030GraphEdgePoliciesGates(PackageBuildFixture):
    """AST-030：实现 void 控制图与两类 Edge policy（R-071）。"""

    def test_AST030_consumer_runs_all_graph_edge_policy_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-071
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST031GraphRunControlGates(PackageBuildFixture):
    """AST-031：实现 GraphRun cancel、完整报告与 caller-relative wait（R-072）。"""

    def test_AST031_consumer_runs_all_graph_run_control_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-072
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST032CoroutineSpawnGates(PackageBuildFixture):
    """AST-032：实现 cold Coroutine Task 与 spawn 强保证（R-073）。"""

    def test_AST032_consumer_runs_all_coroutine_spawn_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-073
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST033CoroutineResumeHandshakeGates(PackageBuildFixture):
    """AST-033：实现唯一 resume ownership 与 await handshake（R-074）。"""

    def test_AST033_consumer_runs_all_coroutine_resume_handshake_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-074
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST034SuspendedCancellationGates(PackageBuildFixture):
    """AST-034：实现 Suspended cancellation 与 Immediate cooperative resume（R-075）。"""

    def test_AST034_consumer_runs_all_suspended_cancellation_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-075
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


class AST035SourceRuntimeAwaitGates(PackageBuildFixture):
    """AST-035：实现 source-Runtime await 与受限组合 API（R-076 / R-078）。"""

    def test_AST035_consumer_runs_all_source_runtime_await_checks(self):
        # 独立 consumer（static+shared）运行期断言 R-076 / R-078
        self._run_consumer(self.static_consumer)
        self._run_consumer(self.shared_consumer)


if __name__ == "__main__":
    unittest.main(verbosity=2)



