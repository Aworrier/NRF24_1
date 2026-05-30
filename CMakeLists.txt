# ============================================================================
# NRF24 项目顶层 CMake 构建配置
#
# 本文件是 ESP-IDF 项目的入口 CMakeLists.txt。
# ESP-IDF 构建系统要求前五行必须保持以下顺序，否则 CMake 无法正确工作。
#
# 构建流程简述:
#   1. cmake_minimum_required: 指定 CMake 最低版本。
#   2. include(project.cmake): 加载 ESP-IDF 的项目引导脚本，
#      该脚本会设置工具链、组件管理器、烧录目标等。
#   3. idf_build_set_property(MINIMAL_BUILD ON):
#      启用最小化构建——仅编译 main 组件及其直接依赖的 ESP-IDF 组件，
#      不编译完整的 ESP-IDF 组件树，大幅加快构建速度。
#   4. project(NRF24): 声明项目名称。
#   5. set(EXTRA_COMPONENT_DIRS components):
#      添加额外的组件搜索路径（本项目使用 "components" 目录）。
# ============================================================================

# 指定 CMake 最低版本要求（ESP-IDF v5.x 要求 >= 3.16，此处使用 3.22 更宽松的兼容性）
cmake_minimum_required(VERSION 3.22)

# 加载 ESP-IDF 项目引导脚本（由 IDF_PATH 环境变量定位）
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 启用最小化构建：只编译 main 和它依赖的组件，加快增量编译速度
idf_build_set_property(MINIMAL_BUILD ON)

# 项目名称（会作为固件文件名的一部分，如 NRF24.bin）
project(NRF24)

# 添加额外组件搜索路径（如第三方库或自定义组件）
set(EXTRA_COMPONENT_DIRS components)
