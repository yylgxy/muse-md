# core/storage：持久化层（文件读写；将来数据库相关的代码也放这里）。
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
add_library(core_storage STATIC
    filemanager.h
    filemanager.cpp
    versioncontrol.h
    versioncontrol.cpp
    cachemanager.h
    cachemanager.cpp
    textencoding.h
)

# 让链接 core_storage 的模块能直接 #include "filemanager.h"
target_include_directories(core_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ★ 同一个 target 的 target_link_libraries 必须全部用 keyword 形式。
#   这一行原来是旧式写法（target_link_libraries(core_storage a b c)），
#   加了 keyword 就必须整行都改，否则 CMake 直接报 "all-keyword or all-plain" 错误。
#
# core_document 是 PUBLIC：filemanager.h 里 include 了 markdowndocument.h（它是值成员），
#           这就属于接口的一部分 —— 链接本模块的人也必须能拿到那个头文件目录。
# infrastructure 是 PRIVATE：FileUtils / Logger 只在 filemanager.cpp 里用，不暴露出去。
# Qt6::Sql 目前没人用（DataBase 在 infrastructure 里），先留着给将来的数据库代码。
target_link_libraries(core_storage
    PUBLIC
        core_document
        Qt6::Core
    PRIVATE
        infrastructure
        Qt6::Sql
)
