# ui 层：主窗口、编辑器控件、预览视图等。
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
add_library(ui STATIC
    mainwindow.ui        # ← .ui 也列进来：AUTOUIC 会用 uic 把它编成 ui_mainwindow.h
    mainwindow.h
    mainwindow.cpp
)

# 让链接 ui 的目标（src/app）能直接 #include "mainwindow.h"
target_include_directories(ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ui business Qt6::Widgets Qt6::WebEngineWidgets)