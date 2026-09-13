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

# ============================================================================
# 兜底依赖：改头文件一定要触发重编
# ============================================================================
#
# 为什么需要这个：这台机器上 CMake 抓不到编译器的 /showIncludes 输出，
# 于是每个目标的依赖表里**只有 .cpp 自己**（compiler_depend.make 里一个头文件都没有，
# 各目标的 *.obj.d 全是 0 字节）。后果不是"编得慢"，而是**编错了**：
# 改了头文件（比如给类加/删成员）之后，别的 .cpp 不重编，新旧 ABI 混在一起，
# 就会出现"代码明明没错、程序一启动就 abort()"这种极难查的崩溃 —— 本机已经踩过四次。
# 实测过：`cmake -E cmake_cl_compile_depends --filter-prefix=...` 产出的 .d 是空的，
# 连空前缀都只能抓到一行源文件名，所以这不是"中文前缀对不上"的问题，是那条链路压根没生效。
#
# 办法：用 OBJECT_DEPENDS 把项目里所有头文件挂到每个源文件上。
# CMake 官方文档（prop_sf/OBJECT_DEPENDS）明确写了：Makefile 生成器和 Ninja 生成器上
# "named files 比目标文件新就重编"。Visual Studio 生成器做不到这一点 ——
# 如果哪天换成 VS 生成器，请换回它自带的依赖跟踪（msbuild 那套是语言无关的，不受这个 bug 影响）。
#
# 代价：改任何头文件都会重编整个项目（本工程全量也就一两分钟），换来的是"不会再编出半新半旧的程序"。
# 等哪天依赖抓取正常了（或者换了生成器），把各子目录末尾的 md_editor_force_header_deps_here() 删掉即可。
#
# 注：这里用 GLOB 是**为了收集依赖**，不是收集源文件 —— 目标里的源文件仍然必须显式列出
#（见上面的说明）。CONFIGURE_DEPENDS 让以后新增的头文件也能被自动收录。
file(GLOB_RECURSE MD_EDITOR_ALL_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/third_party/md4c/src/*.h")

# 在某个目录的 CMakeLists.txt **末尾**调用（此时该目录里的目标都已定义）：
# 把本目录所有目标的所有源文件都挂上"依赖全部头文件"。
# 之所以要按目录调用：源文件属性的作用域默认只对**同一个 CMakeLists.txt 里定义的目标**可见
#（见 CMake 文档 set_source_files_properties），在顶层设一次是没用的。
function(md_editor_force_header_deps_here)
    if(NOT MD_EDITOR_ALL_HEADERS)
        return()
    endif()

    get_property(dir_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS dir_targets)
        get_target_property(sources ${target} SOURCES)
        if(NOT sources)
            continue()          # 没有源文件的目标（比如纯接口库）跳过
        endif()
        set_property(SOURCE ${sources} APPEND PROPERTY OBJECT_DEPENDS ${MD_EDITOR_ALL_HEADERS})
    endforeach()
endfunction()

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