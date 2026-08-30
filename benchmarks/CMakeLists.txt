# AstraScheduler benchmarks（AST-049 / R-089 / D-141）。
# Benchmark 是与 core 隔离的开发组件：仅由 ASTRA_BUILD_BENCHMARKS 启用；
# benchmark 依赖不进入 public headers、安装接口或普通 consumer link graph。
# 消费者构建默认不构建/下载 benchmark 依赖（R-089 例外边界）。

# 仓库自有 scenario runner + harness self-test（无外部依赖，always built）。
add_executable(astra_bench_scenarios
  astra_bench_scenarios.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_collector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_export.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/task_control_block.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/await_protocol.cpp
)
target_link_libraries(astra_bench_scenarios PRIVATE Threads::Threads)
target_include_directories(astra_bench_scenarios PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(astra_bench_scenarios PRIVATE cxx_std_20)

add_executable(astra_bench_harness_selftest astra_bench_harness_selftest.cpp)
target_include_directories(astra_bench_harness_selftest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
)
target_compile_features(astra_bench_harness_selftest PRIVATE cxx_std_20)
target_link_libraries(astra_bench_harness_selftest PRIVATE Threads::Threads)
add_test(NAME astra_bench_harness_selftest COMMAND astra_bench_harness_selftest)

# --- 固定 benchmark corpus（AST-050 / R-003 / R-090 / D-142）---
add_executable(astra_bench_corpus
  astra_bench_corpus.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_collector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_export.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/task_control_block.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/await_protocol.cpp
)
target_compile_definitions(astra_bench_corpus PRIVATE
  ASTRA_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  ASTRA_BENCH_SANITIZER="$<IF:$<BOOL:${ASTRA_ENABLE_SANITIZERS}>,asan,none>"
  ASTRA_BENCH_CXX_FLAGS="${CMAKE_CXX_FLAGS}"
)
target_link_libraries(astra_bench_corpus PRIVATE Threads::Threads)
target_include_directories(astra_bench_corpus PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
)
target_compile_features(astra_bench_corpus PRIVATE cxx_std_20)

add_executable(astra_bench_corpus_selftest astra_bench_corpus_selftest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_collector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_export.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/task_control_block.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/await_protocol.cpp
)
target_link_libraries(astra_bench_corpus_selftest PRIVATE Threads::Threads)
target_include_directories(astra_bench_corpus_selftest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
)
target_compile_features(astra_bench_corpus_selftest PRIVATE cxx_std_20)
add_test(NAME astra_bench_corpus_selftest COMMAND astra_bench_corpus_selftest)

add_executable(astra_bench_stats_selftest astra_bench_stats_selftest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_collector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_export.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/task_control_block.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/await_protocol.cpp
)
target_link_libraries(astra_bench_stats_selftest PRIVATE Threads::Threads)
target_include_directories(astra_bench_stats_selftest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
)
target_compile_features(astra_bench_stats_selftest PRIVATE cxx_std_20)
target_compile_definitions(astra_bench_stats_selftest PRIVATE
  ASTRA_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  ASTRA_BENCH_SANITIZER="$<IF:$<BOOL:${ASTRA_ENABLE_SANITIZERS}>,asan,none>"
  ASTRA_BENCH_CXX_FLAGS="${CMAKE_CXX_FLAGS}"
)
add_test(NAME astra_bench_stats_selftest COMMAND astra_bench_stats_selftest)
# gate CLI 以仓库相对路径调用 tools/，需以仓库根为工作目录。
set_tests_properties(astra_bench_stats_selftest PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# --- pinned Google Benchmark micro cases（R-089 / D-141）---
# 固定 v1.8.3：版本更新通过独立 review，不随网络最新版本漂移。
# 下载失败（无网络环境）时跳过 micro target，不影响 scenario runner。
set(ASTRA_GB_VERSION v1.8.3)
set(ASTRA_GB_URL "https://github.com/google/benchmark/archive/refs/tags/${ASTRA_GB_VERSION}.tar.gz")
set(ASTRA_GB_ARCHIVE "${CMAKE_BINARY_DIR}/google-benchmark-${ASTRA_GB_VERSION}.tar.gz")
# 幂等下载：临时文件成功后原子改名，失败不破坏已有完好 archive（可重试）。
set(ASTRA_GB_TMP "${ASTRA_GB_ARCHIVE}.tmp")
if(NOT EXISTS "${ASTRA_GB_ARCHIVE}")
  set(ASTRA_GB_CODE 1)
  foreach(_attempt RANGE 1 5)
    if(NOT ASTRA_GB_CODE EQUAL 0)
      file(DOWNLOAD "${ASTRA_GB_URL}" "${ASTRA_GB_TMP}"
           STATUS ASTRA_GB_STATUS
           TIMEOUT 180)
      list(GET ASTRA_GB_STATUS 0 ASTRA_GB_CODE)
      if(ASTRA_GB_CODE EQUAL 0)
        file(RENAME "${ASTRA_GB_TMP}" "${ASTRA_GB_ARCHIVE}")
      else()
        file(REMOVE "${ASTRA_GB_TMP}")
        math(EXPR _delay "${_attempt} * 2")
        execute_process(COMMAND ${CMAKE_COMMAND} -E sleep ${_delay})
      endif()
    endif()
  endforeach()
else()
  set(ASTRA_GB_CODE 0)
endif()
if(ASTRA_GB_CODE EQUAL 0 AND EXISTS "${ASTRA_GB_ARCHIVE}")
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googlebenchmark URL "${ASTRA_GB_ARCHIVE}")
  FetchContent_MakeAvailable(googlebenchmark)

  add_executable(astra_bench_micro astra_bench_micro.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_collector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/trace_export.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/task_control_block.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/await_protocol.cpp
  )
  target_link_libraries(astra_bench_micro PRIVATE benchmark::benchmark Threads::Threads)
  target_include_directories(astra_bench_micro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_BINARY_DIR}/../include
  )
  target_compile_features(astra_bench_micro PRIVATE cxx_std_20)
  message(STATUS "AstraScheduler: Google Benchmark ${ASTRA_GB_VERSION} pinned; astra_bench_micro enabled")
else()
  message(WARNING
    "AstraScheduler: Google Benchmark ${ASTRA_GB_VERSION} 不可用（无网络环境）；"
    "astra_bench_micro 跳过，scenario runner 与 harness self-test 仍构建。")
endif()
