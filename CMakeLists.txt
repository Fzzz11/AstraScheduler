cmake_minimum_required(VERSION 3.21)

# AstraScheduler — Phase 0 compiled library、可安装 CMake package 与版本契约
#（AST-002 / R-110；AST-003 / R-093）。
# Supported Configuration 仅 64-bit Linux（R-111 / D-167）。

project(AstraScheduler VERSION 0.1.0 LANGUAGES CXX)

# R-111：Supported Configuration 仅 64-bit Linux；非 Linux / 32-bit 在 configure
# 边界即被拒绝，不得进入 release gate 或 package 支持声明。
if(NOT UNIX OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "AstraScheduler 仅支持 64-bit Linux（R-111 / D-167）；"
        "当前平台不受支持。")
endif()

# 固定 install 布局为 lib/（测试与文档均按此布局审计）。
set(CMAKE_INSTALL_LIBDIR "lib" CACHE PATH "Library installation directory")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# R-093（AST-003）：单一版本源——project VERSION 经 configure_file 生成
# public header 版本宏；CMake version file 与 library 版本符号均源于此。
configure_file(
  cmake/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/include/astra/version.hpp"
  @ONLY
)

# R-110：tests/examples/benchmarks/tools 由 ASTRA_BUILD_* 选项控制；
# 当前仓库只有 tests 入口（ASTRA_BUILD_TESTS），examples/benchmarks/tools
# 选项在对应目录引入时按同一命名加入。作为子工程被消费时不构建测试。
option(ASTRA_BUILD_TESTS "Build the AstraScheduler test entry points" ON)

find_package(Threads REQUIRED)

set(ASTRA_SCHEDULER_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/version.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/trace_collector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/trace_export.cpp
)
add_library(AstraScheduler ${ASTRA_SCHEDULER_SOURCES})
add_library(AstraScheduler::AstraScheduler ALIAS AstraScheduler)

# R-110：CMake target 声明 cxx_std_20（通过 PUBLIC compile feature 传播给 consumer）。
target_compile_features(AstraScheduler PUBLIC cxx_std_20)
target_link_libraries(AstraScheduler PUBLIC Threads::Threads)

target_include_directories(AstraScheduler
  PUBLIC
    # R-093：生成的 version.hpp（构建期由 configure_file 产出）。
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# R-110 Exceptions：warnings-as-errors 等内部诊断选项为 PRIVATE，不传播给 consumer。
# -fvisibility=hidden 使未标记符号默认 hidden，public symbol 仅经 export macro 控制。
target_compile_options(AstraScheduler PRIVATE
  -Wall
  -Wextra
  -Werror
  -fvisibility=hidden
  -fvisibility-inlines-hidden
)

set_target_properties(AstraScheduler PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

# R-110：实现目录不安装；public headers 仅位于 include/astra。
# R-093：生成的 version.hpp 是 public header，与源树 header 同位安装。
install(TARGETS AstraScheduler
  EXPORT AstraSchedulerTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/astra
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/astra/version.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/astra
)

install(EXPORT AstraSchedulerTargets
  FILE AstraSchedulerTargets.cmake
  NAMESPACE AstraScheduler::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AstraScheduler
)

configure_package_config_file(
  cmake/AstraSchedulerConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/AstraSchedulerConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AstraScheduler
)

# R-093：CMake exact-version 检查是主要 mismatch 边界——请求版本与安装
# 版本不一致（含同 major 的更旧组合）在 configure 边界即被拒绝；
# D-145：binary consumer 升级时必须 rebuild/relink。
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/AstraSchedulerConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY ExactVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/AstraSchedulerConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/AstraSchedulerConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AstraScheduler
)

# 测试入口（Phase 0 gate：测试入口可运行）。consumer 模板位于 tests/consumer/，
# 不参与本工程构建，由 tools/check_cmake_package.py 复制到仓库外验证。
if(PROJECT_IS_TOP_LEVEL AND ASTRA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Benchmarks（AST-049 / R-089 / D-141）：与 core 隔离的开发组件，默认 OFF；
# consumer build 不构建/下载 benchmark 依赖。
option(ASTRA_BUILD_BENCHMARKS "Build the AstraScheduler benchmark harness and scenarios" OFF)
if(PROJECT_IS_TOP_LEVEL AND ASTRA_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
