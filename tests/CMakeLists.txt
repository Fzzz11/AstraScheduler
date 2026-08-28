# 测试入口：链接库自身并验证 BUILD_INTERFACE public include 可用
#（AST-002）；selftest 同时断言 in-tree 版本契约（AST-003 / R-093）。

add_executable(astra_selftest selftest.cpp)
target_link_libraries(astra_selftest PRIVATE AstraScheduler)
add_test(NAME astra_selftest COMMAND astra_selftest)

add_executable(astra_scheduler_contract_test test_scheduler_contract.cpp)
target_link_libraries(astra_scheduler_contract_test PRIVATE AstraScheduler)
add_test(NAME astra_scheduler_contract_test COMMAND astra_scheduler_contract_test)

add_executable(astra_startup_transaction_test
  test_startup_transaction.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_startup_transaction_test PRIVATE Threads::Threads)
target_include_directories(astra_startup_transaction_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_startup_transaction_test PRIVATE cxx_std_20)
add_test(NAME astra_startup_transaction_test COMMAND astra_startup_transaction_test)

add_executable(astra_runtime_state_handoff_test
  test_runtime_state_handoff.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_runtime_state_handoff_test PRIVATE Threads::Threads)
target_include_directories(astra_runtime_state_handoff_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_runtime_state_handoff_test PRIVATE cxx_std_20)
add_test(NAME astra_runtime_state_handoff_test COMMAND astra_runtime_state_handoff_test)

add_executable(astra_reaper_coordinator_test
  test_reaper_coordinator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_reaper_coordinator_test PRIVATE Threads::Threads)
target_include_directories(astra_reaper_coordinator_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_reaper_coordinator_test PRIVATE cxx_std_20)
add_test(NAME astra_reaper_coordinator_test COMMAND astra_reaper_coordinator_test)

add_executable(astra_global_worker_runtime_test
  test_global_worker_runtime.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_global_worker_runtime_test PRIVATE Threads::Threads)
target_include_directories(astra_global_worker_runtime_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_global_worker_runtime_test PRIVATE cxx_std_20)
add_test(NAME astra_global_worker_runtime_test COMMAND astra_global_worker_runtime_test)

add_executable(astra_move_only_submit_test
  test_move_only_submit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_move_only_submit_test PRIVATE Threads::Threads)
target_include_directories(astra_move_only_submit_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_move_only_submit_test PRIVATE cxx_std_20)
add_test(NAME astra_move_only_submit_test COMMAND astra_move_only_submit_test)

add_executable(astra_admission_backpressure_test
  test_admission_backpressure.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_admission_backpressure_test PRIVATE Threads::Threads)
target_include_directories(astra_admission_backpressure_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_admission_backpressure_test PRIVATE cxx_std_20)
add_test(NAME astra_admission_backpressure_test COMMAND astra_admission_backpressure_test)

add_executable(astra_task_outcome_state_test
  test_task_outcome_state.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_task_outcome_state_test PRIVATE Threads::Threads)
target_include_directories(astra_task_outcome_state_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_task_outcome_state_test PRIVATE cxx_std_20)
add_test(NAME astra_task_outcome_state_test COMMAND astra_task_outcome_state_test)

add_executable(astra_helping_wait_test
  test_helping_wait.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_helping_wait_test PRIVATE Threads::Threads)
target_include_directories(astra_helping_wait_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_helping_wait_test PRIVATE cxx_std_20)
add_test(NAME astra_helping_wait_test COMMAND astra_helping_wait_test)

add_executable(astra_task_cancellation_test
  test_task_cancellation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_task_cancellation_test PRIVATE Threads::Threads)
target_include_directories(astra_task_cancellation_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_task_cancellation_test PRIVATE cxx_std_20)
add_test(NAME astra_task_cancellation_test COMMAND astra_task_cancellation_test)

add_executable(astra_graceful_drain_test
  test_graceful_drain.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_graceful_drain_test PRIVATE Threads::Threads)
target_include_directories(astra_graceful_drain_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_graceful_drain_test PRIVATE cxx_std_20)
add_test(NAME astra_graceful_drain_test COMMAND astra_graceful_drain_test)

add_executable(astra_shutdown_guards_test
  test_shutdown_guards.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_shutdown_guards_test PRIVATE Threads::Threads)
target_include_directories(astra_shutdown_guards_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_shutdown_guards_test PRIVATE cxx_std_20)
add_test(NAME astra_shutdown_guards_test COMMAND astra_shutdown_guards_test)

add_executable(astra_immediate_escalation_test
  test_immediate_escalation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_immediate_escalation_test PRIVATE Threads::Threads)
target_include_directories(astra_immediate_escalation_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_immediate_escalation_test PRIVATE cxx_std_20)
add_test(NAME astra_immediate_escalation_test COMMAND astra_immediate_escalation_test)

add_executable(astra_last_handle_raii_test
  test_last_handle_raii.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_last_handle_raii_test PRIVATE Threads::Threads)
target_include_directories(astra_last_handle_raii_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_last_handle_raii_test PRIVATE cxx_std_20)
add_test(NAME astra_last_handle_raii_test COMMAND astra_last_handle_raii_test)

add_executable(astra_finalization_control_api_test
  test_finalization_control_api.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_finalization_control_api_test PRIVATE Threads::Threads)
target_include_directories(astra_finalization_control_api_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_finalization_control_api_test PRIVATE cxx_std_20)
add_test(NAME astra_finalization_control_api_test COMMAND astra_finalization_control_api_test)

add_executable(astra_finalization_begin_test
  test_finalization_begin.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_finalization_begin_test PRIVATE Threads::Threads)
target_include_directories(astra_finalization_begin_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_finalization_begin_test PRIVATE cxx_std_20)
add_test(NAME astra_finalization_begin_test COMMAND astra_finalization_begin_test)

add_executable(astra_finalization_wait_test
  test_finalization_wait.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_finalization_wait_test PRIVATE Threads::Threads)
target_include_directories(astra_finalization_wait_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_finalization_wait_test PRIVATE cxx_std_20)
add_test(NAME astra_finalization_wait_test COMMAND astra_finalization_wait_test)

add_executable(astra_finalization_escalation_test
  test_finalization_escalation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_finalization_escalation_test PRIVATE Threads::Threads)
target_include_directories(astra_finalization_escalation_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_finalization_escalation_test PRIVATE cxx_std_20)
add_test(NAME astra_finalization_escalation_test COMMAND astra_finalization_escalation_test)

add_executable(astra_locked_local_routing_test
  test_locked_local_routing.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_locked_local_routing_test PRIVATE Threads::Threads)
target_include_directories(astra_locked_local_routing_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_locked_local_routing_test PRIVATE cxx_std_20)
add_test(NAME astra_locked_local_routing_test COMMAND astra_locked_local_routing_test)

add_executable(astra_steal_round_test
  test_steal_round.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_steal_round_test PRIVATE Threads::Threads)
target_include_directories(astra_steal_round_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_steal_round_test PRIVATE cxx_std_20)
add_test(NAME astra_steal_round_test COMMAND astra_steal_round_test)

add_executable(astra_park_handshake_test
  test_park_handshake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_park_handshake_test PRIVATE Threads::Threads)
target_include_directories(astra_park_handshake_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_park_handshake_test PRIVATE cxx_std_20)
add_test(NAME astra_park_handshake_test COMMAND astra_park_handshake_test)

add_executable(astra_chase_lev_ordering_test
  test_chase_lev_ordering.cpp
)
target_link_libraries(astra_chase_lev_ordering_test PRIVATE Threads::Threads)
target_include_directories(astra_chase_lev_ordering_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_chase_lev_ordering_test PRIVATE cxx_std_20)
add_test(NAME astra_chase_lev_ordering_test COMMAND astra_chase_lev_ordering_test)

add_executable(astra_chase_lev_growth_test
  test_chase_lev_growth.cpp
)
target_link_libraries(astra_chase_lev_growth_test PRIVATE Threads::Threads)
target_include_directories(astra_chase_lev_growth_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_chase_lev_growth_test PRIVATE cxx_std_20)
add_test(NAME astra_chase_lev_growth_test COMMAND astra_chase_lev_growth_test)

add_executable(astra_chase_lev_indices_test
  test_chase_lev_indices.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_chase_lev_indices_test PRIVATE Threads::Threads)
target_include_directories(astra_chase_lev_indices_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_chase_lev_indices_test PRIVATE cxx_std_20)
add_test(NAME astra_chase_lev_indices_test COMMAND astra_chase_lev_indices_test)

add_executable(astra_task_graph_freeze_test
  test_task_graph_freeze.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_task_graph_freeze_test PRIVATE Threads::Threads)
target_include_directories(astra_task_graph_freeze_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_task_graph_freeze_test PRIVATE cxx_std_20)
add_test(NAME astra_task_graph_freeze_test COMMAND astra_task_graph_freeze_test)

add_executable(astra_graph_admission_test
  test_graph_admission.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_graph_admission_test PRIVATE Threads::Threads)
target_include_directories(astra_graph_admission_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_graph_admission_test PRIVATE cxx_std_20)
add_test(NAME astra_graph_admission_test COMMAND astra_graph_admission_test)

add_executable(astra_graph_edge_policies_test
  test_graph_edge_policies.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_graph_edge_policies_test PRIVATE Threads::Threads)
target_include_directories(astra_graph_edge_policies_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_graph_edge_policies_test PRIVATE cxx_std_20)
add_test(NAME astra_graph_edge_policies_test COMMAND astra_graph_edge_policies_test)

add_executable(astra_graph_run_control_test
  test_graph_run_control.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_graph_run_control_test PRIVATE Threads::Threads)
target_include_directories(astra_graph_run_control_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_graph_run_control_test PRIVATE cxx_std_20)
add_test(NAME astra_graph_run_control_test COMMAND astra_graph_run_control_test)

add_executable(astra_coroutine_spawn_test
  test_coroutine_spawn.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_coroutine_spawn_test PRIVATE Threads::Threads)
target_include_directories(astra_coroutine_spawn_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_coroutine_spawn_test PRIVATE cxx_std_20)
add_test(NAME astra_coroutine_spawn_test COMMAND astra_coroutine_spawn_test)

add_executable(astra_coroutine_resume_handshake_test
  test_coroutine_resume_handshake.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_coroutine_resume_handshake_test PRIVATE Threads::Threads)
target_include_directories(astra_coroutine_resume_handshake_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_coroutine_resume_handshake_test PRIVATE cxx_std_20)
add_test(NAME astra_coroutine_resume_handshake_test COMMAND astra_coroutine_resume_handshake_test)

add_executable(astra_suspended_cancellation_test
  test_suspended_cancellation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_suspended_cancellation_test PRIVATE Threads::Threads)
target_include_directories(astra_suspended_cancellation_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_suspended_cancellation_test PRIVATE cxx_std_20)
add_test(NAME astra_suspended_cancellation_test COMMAND astra_suspended_cancellation_test)

add_executable(astra_source_runtime_await_test
  test_source_runtime_await.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/reaper_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/finalization.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/version.cpp
)
target_link_libraries(astra_source_runtime_await_test PRIVATE Threads::Threads)
target_include_directories(astra_source_runtime_await_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_BINARY_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(astra_source_runtime_await_test PRIVATE cxx_std_20)
add_test(NAME astra_source_runtime_await_test COMMAND astra_source_runtime_await_test)





















