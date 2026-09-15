# business 层：业务逻辑与可复用的界面组件。
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
add_library(business STATIC
    editorwidget.h
    editorwidget.cpp
    tabmanager.h
    tabmanager.cpp
    editorworkbench.h
    editorworkbench.cpp
    filetreeview.h
    filetreeview.cpp
    recentfiles.h
    recentfiles.cpp
    fulltextsearch.h
    fulltextsearch.cpp
    searchpanel.h
    searchpanel.cpp
    exporter.h
    exporter.cpp
    thememanager.h
    thememanager.cpp
    findreplacedialog.h
    findreplacedialog.cpp
    sessionstate.h
    sessionstate.cpp
)

# 让链接 business 的模块能直接 #include "editorwidget.h"。
# 这条是**必须**的：mainwindow.ui 里把 editor 提升成了 EditorWidget，
# uic 生成的 ui_mainwindow.h 会写 #include "editorwidget.h"，
# 所以 ui 目标必须能通过 business 的头文件目录找到它。
target_include_directories(business PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ★ 同一个 target 的 target_link_libraries 必须全部用 keyword 形式
#   （原来是旧式写法，加 keyword 就得整行都改）。
#
# Qt6::Widgets 是 PUBLIC：这些控件都继承 Qt Widgets 的类，头文件出现在接口里。
# core_document / core_storage 是 PUBLIC：EditorWidget 绑定了 MarkdownHighlighter
#           （属于 core/document）；将来业务层还会用到 FileManager（core/storage）。
# infrastructure 是 PRIVATE：RecentFiles / FullTextSearch 只在 .cpp 里用 ConfigManager / FileUtils。
# Qt6::Sql 是 PRIVATE：全文搜索（5.5）用 SQLite 存索引，fulltextsearch.h 里有个 QSqlDatabase
#                     成员，所以"能编译"要求 Qt6::Sql 的头文件可见（Qt 的头文件都在同一个
#                     include 根下，Qt6::Core 已经带来了）；**链接**时静态库的
#                     PRIVATE 依赖会被 CMake 以 $<LINK_ONLY:Qt6::Sql> 自动传给最终目标，
#                     所以链接 business 的人不用自己再写一遍 —— 但测试要直接用
#                     QSqlDatabase 的话，自己显式链接更清楚。
# Qt6::WebEngineWidgets 是 PRIVATE：导出 PDF（5.6）要用 QWebEnginePage::printToPdf，
#                     EditorWorkbench 也用 QWebEngineView。这两个头文件里都只是前置声明的
#                     指针，不需要出现在接口上（core_document 本来就 PUBLIC 带了它，
#                     这里显式写出来是为了让"谁在用 WebEngine"一眼可见）。
target_link_libraries(business
    PUBLIC
        core_document
        core_storage
        Qt6::Widgets
    PRIVATE
        infrastructure
        Qt6::Sql
        Qt6::WebEngineWidgets
)

# 兜底依赖：改任何头文件都重编本目录的目标。
# 原因见根 CMakeLists.txt 里 md_editor_force_header_deps_here() 上面的长注释
#（本机 CMake 抓不到 /showIncludes，依赖表是空的，会出现新旧 ABI 混用的诡异崩溃）。
md_editor_force_header_deps_here()
