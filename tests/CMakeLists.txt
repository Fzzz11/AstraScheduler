# 测试入口：链接库自身并验证 BUILD_INTERFACE public include 可用
#（AST-002）；selftest 同时断言 in-tree 版本契约（AST-003 / R-093）。

add_executable(astra_selftest selftest.cpp)
target_link_libraries(astra_selftest PRIVATE AstraScheduler)
add_test(NAME astra_selftest COMMAND astra_selftest)

add_executable(astra_scheduler_contract_test test_scheduler_contract.cpp)
target_link_libraries(astra_scheduler_contract_test PRIVATE AstraScheduler)
add_test(NAME astra_scheduler_contract_test COMMAND astra_scheduler_contract_test)
