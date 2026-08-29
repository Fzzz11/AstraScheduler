cmake_minimum_required(VERSION 3.21)

# 独立 Linux consumer 工程（R-110 独立 find_package/link/run smoke）。
# 本模板由 tools/check_cmake_package.py 复制到仓库外目录构建，
# 验证安装目录可被外部最小工程消费。

project(astra_consumer LANGUAGES CXX)

# R-093：package 使用 exact-version 检查（主要 mismatch 边界），consumer
# 必须钉住完整 major.minor.patch 三元组；库版本升级时该请求会失败，
# 强制 consumer 更新请求并 rebuild/relink（D-145）。
find_package(AstraScheduler 1.0.0 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE AstraScheduler::AstraScheduler)
