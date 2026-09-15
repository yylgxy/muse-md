# ui 层：主窗口、编辑器控件、预览视图等。
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
add_library(ui STATIC
    mainwindow.ui        # ← .ui 也列进来：AUTOUIC 会用 uic 把它编成 ui_mainwindow.h
    mainwindow.h
    mainwindow.cpp
    exportdialog.h       # 5.6：导出对话框（只收集设置，干活的是 business 里的 Exporter）
    exportdialog.cpp
)

# 让链接 ui 的目标（src/app）能直接 #include "mainwindow.h"
target_include_directories(ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ★ 同一个 target 的 target_link_libraries 必须全部用 keyword 形式（原来是旧式写法，
#   加了 keyword 就得整行都改）。这里把 ui 真正用到的几层都明确写出来，不靠传递：
#     core_storage  → FileManager（文件管理，4.2.1）
#     core_document → PreviewRenderer / SyncBridge（渲染管线 + 双向同步）
target_link_libraries(ui
    PUBLIC
        business
        core_storage
        core_document
        Qt6::Widgets
        Qt6::WebEngineWidgets
)
# 兜底依赖：改任何头文件都重编本目录的目标。
# 原因见根 CMakeLists.txt 里 md_editor_force_header_deps_here() 上面的长注释
#（本机 CMake 抓不到 /showIncludes，依赖表是空的，会出现新旧 ABI 混用的诡异崩溃）。
md_editor_force_header_deps_here()
