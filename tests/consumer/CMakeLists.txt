cmake_minimum_required(VERSION 3.21)

# 独立 Linux consumer 工程（R-110 独立 find_package/link/run smoke）。
# 本模板由 tools/check_cmake_package.py 复制到仓库外目录构建，
# 验证安装目录可被外部最小工程消费。

project(astra_consumer LANGUAGES CXX)

find_package(AstraScheduler 0.1 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE AstraScheduler::AstraScheduler)
