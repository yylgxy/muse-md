cmake_minimum_required(VERSION 3.24)
project(MarkdownEditor VERSION 1.0.0 LANGUAGES CXX C)   # 需要 C：third_party/md4c 是纯 C 库

# C++17 标准 + Qt 自动编译开关
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)    # 自动处理信号槽 MOC
set(CMAKE_AUTORCC ON)    # 自动处理资源文件
set(CMAKE_AUTOUIC ON)    # 自动处理 UI 文件

# Windows 专属「运行时不弹控制台黑框」由可执行目标上的 WIN32 关键字实现，
# 见 src/app/CMakeLists.txt。不要再用全局 CMAKE_WIN32_EXECUTABLE（它会作用于所有目标）。
#
# 禁止在任何子目录里用 file(GLOB) / file(GLOB_RECURSE) 收集源文件：
#   它会递归吞掉目录下的构建产物。曾因此把
#   src/*/build/.../CMakeFiles/<ver>/CompilerIdCXX/CMakeCXXCompilerId.cpp
#   当成项目源码编进每个 target，并在 Qt Creator 的项目树里冒出一个
#   "Source Files > build\...\CompilerIdCXX" 节点。源文件一律在各子目录里显式列出。

# 输出目录统一
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 查找 Qt6 所需模块（P0+P1 只需要这些）
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    WebEngineWidgets
    Sql
    Network
)

# 全局头文件路径
include_directories(${PROJECT_SOURCE_DIR}/src)

# 第三方库（原样引入的外部源码，见 third_party/CMakeLists.txt）
# 必须排在 src/ 之前：core_document 要链接它定义的 md4c-html 目标
add_subdirectory(third_party)

# 添加子模块
add_subdirectory(src/infrastructure)
add_subdirectory(src/core/document)
add_subdirectory(src/core/storage)
add_subdirectory(src/business)
add_subdirectory(src/ui)
add_subdirectory(src/app)

# 测试：在构建目录里用 ctest -C Debug --output-on-failure 运行
enable_testing()
add_subdirectory(tests)