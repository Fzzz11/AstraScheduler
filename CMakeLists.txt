cmake_minimum_required(VERSION 3.21)

# AstraScheduler — Phase 0 compiled library 与可安装 CMake package（AST-002 / R-110）。
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

# R-110：tests/examples/benchmarks/tools 由 ASTRA_BUILD_* 选项控制；
# 当前仓库只有 tests 入口（ASTRA_BUILD_TESTS），examples/benchmarks/tools
# 选项在对应目录引入时按同一命名加入。作为子工程被消费时不构建测试。
option(ASTRA_BUILD_TESTS "Build the AstraScheduler test entry points" ON)

add_library(AstraScheduler src/skeleton.cpp)
add_library(AstraScheduler::AstraScheduler ALIAS AstraScheduler)

# R-110：CMake target 声明 cxx_std_20（通过 PUBLIC compile feature 传播给 consumer）。
target_compile_features(AstraScheduler PUBLIC cxx_std_20)

target_include_directories(AstraScheduler
  PUBLIC
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
)

set_target_properties(AstraScheduler PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

# R-110：实现目录不安装；public headers 仅位于 include/astra。
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

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/AstraSchedulerConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
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
