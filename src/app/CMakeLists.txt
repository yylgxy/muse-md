# 应用入口：全项目唯一的可执行目标。
# WIN32 = Windows 下用 GUI 子系统启动，不弹控制台黑框（替代原来的全局 CMAKE_WIN32_EXECUTABLE）。
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
add_executable(MarkdownEditor WIN32
    main.cpp
    # 资源清单：AUTORCC 会让 rcc 把它编成 C++ 源码链进 exe，
    # 运行时用 ":/html/preview_template.html" 这样的虚拟路径读取（不依赖 exe 旁边的文件）
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

# 把 CMake 里 project(... VERSION 1.0.0) 的版本号传给代码，日志启动横幅里会打出来
target_compile_definitions(MarkdownEditor PRIVATE APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(MarkdownEditor ui infrastructure)