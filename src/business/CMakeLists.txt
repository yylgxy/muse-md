# business 层：业务逻辑与可复用的界面组件。
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
add_library(business STATIC
    editorwidget.h
    editorwidget.cpp
    tabmanager.h
    tabmanager.cpp
    editorworkbench.h
    editorworkbench.cpp
)

# 让链接 business 的模块能直接 #include "editorwidget.h"。
# 这条是**必须**的：mainwindow.ui 里把 editor 提升成了 EditorWidget，
# uic 生成的 ui_mainwindow.h 会写 #include "editorwidget.h"，
# 所以 ui 目标必须能通过 business 的头文件目录找到它。
target_include_directories(business PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ★ 同一个 target 的 target_link_libraries 必须全部用 keyword 形式
#   （原来是旧式写法，加 keyword 就得整行都改）。
#
# Qt6::Widgets 是 PUBLIC：EditorWidget 继承 QPlainTextEdit，qplaintextedit.h 出现在
#           它自己的头文件里，属于接口的一部分。
# core_document / core_storage 是 PUBLIC：EditorWidget 里绑定了 MarkdownHighlighter
#           （属于 core/document）；将来业务层还会用到 FileManager（core/storage）。
target_link_libraries(business
    PUBLIC
        core_document
        core_storage
        Qt6::Widgets
)

# 兜底依赖：改任何头文件都重编本目录的目标。
# 原因见根 CMakeLists.txt 里 md_editor_force_header_deps_here() 上面的长注释
#（本机 CMake 抓不到 /showIncludes，依赖表是空的，会出现新旧 ABI 混用的诡异崩溃）。
md_editor_force_header_deps_here()
