# 性能基准（A2）：一个可执行程序，多个子命令区分场景。
#
# 跑法：
#     cmake --build build --config Debug --target md_bench
#     build/bin/md_bench all --save bench/baseline.json
#
# 源文件同样必须显式列出（不要 file(GLOB)，理由见根 CMakeLists.txt 顶部说明）。

add_executable(md_bench
    bench_main.cpp
    # 把 qrc 挂上：Exporter::standaloneHtml 要从 :/html/preview_template.html 取预览模板。
    # 不挂的话导出会走"极简样式兜底"那条路，B5 量到的就不是真实的导出代价了。
    # （test_syncbridge 也是这么补的，理由相同。）
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

# 链接说明：
#   business      —— SearchPanel（B2 界面级指标要量它那条路径）+ Exporter + EditorWidget
#   core_document —— SyncBridge::buildLineMap / FrameProbe（由 business PUBLIC 带来，这里写出来是为了显式）
#   Qt6::Widgets  —— QApplication / QWidget
#   Qt6::Sql      —— FullTextSearch 就是 SQLite 索引
# 注意：**不链接** Qt6::WebEngineWidgets。bench 里没有一行需要 Chromium，
# 所以基准可以在没有图形环境的机器上跑（PDF 导出因此不纳入基准）。
target_link_libraries(md_bench
    PRIVATE
        business
        Qt6::Widgets
        Qt6::Sql
)

md_editor_force_header_deps_here()
