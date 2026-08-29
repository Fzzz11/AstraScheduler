# 测试保持独立可执行程序以隔离进程级 Runtime/Finalization 状态。public
# tests 链接真实 static/shared 变体；shared 下需要 hidden detail seam 的测试
# 统一链接一份 test-only static runtime，避免每个测试重复编译实现源码。
set(ASTRA_INTERNAL_TEST_LIBRARY AstraScheduler)
if(BUILD_SHARED_LIBS)
  add_library(AstraSchedulerTestRuntime STATIC ${ASTRA_SCHEDULER_SOURCES})
  target_compile_features(AstraSchedulerTestRuntime PUBLIC cxx_std_20)
  target_link_libraries(AstraSchedulerTestRuntime PUBLIC Threads::Threads)
  target_include_directories(AstraSchedulerTestRuntime PUBLIC
    ${PROJECT_BINARY_DIR}/include
    ${PROJECT_SOURCE_DIR}/include
  )
  target_compile_options(AstraSchedulerTestRuntime PRIVATE
    -Wall
    -Wextra
    -Werror
    -fvisibility=hidden
    -fvisibility-inlines-hidden
  )
  set(ASTRA_INTERNAL_TEST_LIBRARY AstraSchedulerTestRuntime)
endif()

function(astra_add_test target source library)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE ${library})
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )
  target_compile_features(${target} PRIVATE cxx_std_20)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

function(astra_add_public_test target source)
  astra_add_test(${target} ${source} AstraScheduler)
endfunction()

function(astra_add_internal_test target source)
  astra_add_test(${target} ${source} ${ASTRA_INTERNAL_TEST_LIBRARY})
endfunction()

astra_add_public_test(astra_selftest selftest.cpp)
astra_add_public_test(astra_scheduler_contract_test test_scheduler_contract.cpp)
astra_add_internal_test(astra_startup_transaction_test test_startup_transaction.cpp)
astra_add_internal_test(astra_runtime_state_handoff_test test_runtime_state_handoff.cpp)
astra_add_internal_test(astra_reaper_coordinator_test test_reaper_coordinator.cpp)
astra_add_internal_test(astra_global_worker_runtime_test test_global_worker_runtime.cpp)
astra_add_internal_test(astra_move_only_submit_test test_move_only_submit.cpp)
astra_add_internal_test(astra_admission_backpressure_test test_admission_backpressure.cpp)
astra_add_public_test(astra_task_outcome_state_test test_task_outcome_state.cpp)
astra_add_public_test(astra_helping_wait_test test_helping_wait.cpp)
astra_add_public_test(astra_task_cancellation_test test_task_cancellation.cpp)
astra_add_public_test(astra_graceful_drain_test test_graceful_drain.cpp)
astra_add_public_test(astra_shutdown_guards_test test_shutdown_guards.cpp)
astra_add_public_test(astra_immediate_escalation_test test_immediate_escalation.cpp)
astra_add_public_test(astra_last_handle_raii_test test_last_handle_raii.cpp)
astra_add_public_test(astra_finalization_control_api_test test_finalization_control_api.cpp)
astra_add_internal_test(astra_finalization_begin_test test_finalization_begin.cpp)
astra_add_internal_test(astra_finalization_wait_test test_finalization_wait.cpp)
astra_add_internal_test(astra_finalization_escalation_test test_finalization_escalation.cpp)
astra_add_public_test(astra_locked_local_routing_test test_locked_local_routing.cpp)
astra_add_internal_test(astra_steal_round_test test_steal_round.cpp)
astra_add_internal_test(astra_park_handshake_test test_park_handshake.cpp)
astra_add_internal_test(astra_chase_lev_ordering_test test_chase_lev_ordering.cpp)
astra_add_internal_test(astra_chase_lev_growth_test test_chase_lev_growth.cpp)
astra_add_internal_test(astra_chase_lev_indices_test test_chase_lev_indices.cpp)
astra_add_public_test(astra_task_graph_freeze_test test_task_graph_freeze.cpp)
astra_add_public_test(astra_graph_admission_test test_graph_admission.cpp)
astra_add_public_test(astra_graph_edge_policies_test test_graph_edge_policies.cpp)
astra_add_public_test(astra_graph_run_control_test test_graph_run_control.cpp)
astra_add_public_test(astra_coroutine_spawn_test test_coroutine_spawn.cpp)
astra_add_public_test(astra_coroutine_resume_handshake_test test_coroutine_resume_handshake.cpp)
astra_add_internal_test(astra_suspended_cancellation_test test_suspended_cancellation.cpp)
astra_add_public_test(astra_source_runtime_await_test test_source_runtime_await.cpp)
astra_add_internal_test(astra_graph_coroutine_identity_test test_graph_coroutine_identity.cpp)
astra_add_public_test(astra_worker_timers_test test_worker_timers.cpp)
astra_add_internal_test(astra_base_priority_test test_base_priority.cpp)
astra_add_public_test(astra_priority_bands_test test_priority_bands.cpp)
astra_add_public_test(astra_task_deadline_test test_task_deadline.cpp)
astra_add_public_test(astra_global_edf_test test_global_edf.cpp)
astra_add_public_test(astra_runtime_metrics_test test_runtime_metrics.cpp)
astra_add_public_test(astra_metrics_snapshot_test test_metrics_snapshot.cpp)
astra_add_internal_test(astra_process_metrics_test test_process_metrics.cpp)
astra_add_internal_test(astra_trace_collector_test test_trace_collector.cpp)
astra_add_internal_test(astra_trace_event_schema_test test_trace_event_schema.cpp)
astra_add_internal_test(astra_trace_export_test test_trace_export.cpp)
astra_add_public_test(astra_wait_await_diagnostics_test test_wait_await_diagnostics.cpp)
astra_add_public_test(astra_coroutine_frame_lifetime_test test_coroutine_frame_lifetime.cpp)
astra_add_internal_test(astra_platform_invariants_test test_platform_invariants.cpp)

# Package gate 资源使用回归：并行度有界、测试复用库产物、consumer 单次执行。
add_test(NAME check_package_efficiency COMMAND
         ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
         python3 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_package_efficiency.py)
set_tests_properties(check_package_efficiency PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# AST-052 / R-111：Linux-only tier matrix 与 package 声明审计（以仓库根为工作目录）。
add_test(NAME check_platform_matrix COMMAND
         ${CMAKE_COMMAND} -E env python3 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_platform_matrix.py)
set_tests_properties(check_platform_matrix PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
