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
