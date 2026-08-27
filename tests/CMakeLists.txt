# AST-002 测试入口：链接库自身并验证 BUILD_INTERFACE public include 可用。

add_executable(astra_selftest selftest.cpp)
target_link_libraries(astra_selftest PRIVATE AstraScheduler)
add_test(NAME astra_selftest COMMAND astra_selftest)
