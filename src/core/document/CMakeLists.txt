# core/document：文档模型层。
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
add_library(core_document STATIC
    markdownparser.h
    markdownparser.cpp
    markdowndocument.h
    markdowndocument.cpp
)

# 让链接 core_document 的模块能直接 #include "markdownparser.h"
target_include_directories(core_document PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ★ 同一个 target 的 target_link_libraries 必须全部用 keyword 形式，
#   不能和旧式写法（target_link_libraries(t a b c)）混用，否则 CMake 直接报错。
#
# PUBLIC  = 对外接口的一部分，会传递给链接本模块的人：
#           infrastructure 的头文件目录就是这样一路传到 ui/app 的
#           （所以 src/app/main.cpp 能直接 #include "logger.h"）。
# PRIVATE = 实现细节，不传递：
#           md4c 只在 markdownparser.cpp 里用，ui/app 不该也不许 #include <md4c-html.h>。
#           哪天上层"偷偷"直接用上了 md4c，编译器会拦住它 —— 这正是分层想要的效果。
target_link_libraries(core_document
    PUBLIC
        infrastructure
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::WebEngineWidgets
    PRIVATE
        md4c-html
)
