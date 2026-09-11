# infrastructure 层：与 UI 无关的基础设施（日志、配置、路径、平台封装）。
#
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
# 新增 .cpp/.h 时直接在这个列表里加一行 —— 忘了加 .cpp 不会编译报错，
# 而是在链接阶段报 "LNK2019: 无法解析的外部符号"。
add_library(infrastructure STATIC
    logger.h
    logger.cpp
    fileutils.h
    fileutils.cpp
    stringutils.h
    stringutils.cpp
    configmanager.h
    configmanager.cpp
)

# 让链接 infrastructure 的模块能直接 #include "fileutils.h"、"logger.h"……（导出本目录所有头文件）
target_include_directories(infrastructure PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 让 Qt 自己的 qDebug/qWarning 也带上文件名和行号（Release 下 Qt 默认会丢掉）
target_compile_definitions(infrastructure PUBLIC QT_MESSAGELOGCONTEXT)

target_link_libraries(infrastructure Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql)
