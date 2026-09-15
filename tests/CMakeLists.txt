# 测试目录。
#
# 跑法：在构建目录里执行
#     ctest -C Debug --output-on-failure
# 或者直接运行 bin/Debug/test_xxx.exe（退出码 0 = 全部通过）。
#
# 注意：新增测试文件时同样要显式列进 add_executable（不要用 file(GLOB)，
#       理由见根 CMakeLists.txt 顶部说明）。

# ---------------- FileUtils 契约测试 ----------------
add_executable(test_fileutils
    test_fileutils.cpp
)
target_link_libraries(test_fileutils
    infrastructure
    Qt6::Core
)
add_test(NAME fileutils COMMAND test_fileutils)

# ---------------- MarkdownParser 契约测试 ----------------
# 这个测试只链接 core_document，测试代码里没有一行 md4c 代码 ——
# md4c 是 core_document 的实现细节（CMake 里 PRIVATE 链接），拿不到它的头文件，
# 这正是分层想要的效果。
add_executable(test_markdownparser
    test_markdownparser.cpp
)
target_link_libraries(test_markdownparser
    core_document
    Qt6::Core
)
add_test(NAME markdownparser COMMAND test_markdownparser)

# ---------------- DataBase 契约测试 ----------------
# 这个测试必须有 QCoreApplication（Qt 靠它加载 SQL 驱动插件），
# 运行时还需要 Qt 的 plugins/sqldrivers/qsqlite.dll 能被找到。
add_executable(test_database
    test_database.cpp
)
target_link_libraries(test_database
    infrastructure
    Qt6::Core
    Qt6::Sql
)
add_test(NAME database COMMAND test_database)

# ---------------- MarkdownDocument 契约测试 ----------------
# 只链接 core_document 就够：FileUtils / Logger 是通过 core_document 的
# PUBLIC infrastructure 传递过来的。
add_executable(test_markdowndocument
    test_markdowndocument.cpp
)
target_link_libraries(test_markdowndocument
    core_document
    Qt6::Core
)
add_test(NAME markdowndocument COMMAND test_markdowndocument)

# ---------------- MarkdownHighlighter 契约测试 ----------------
# 需要 Qt6::Gui：QTextDocument / QTextLayout / QGuiApplication 都在 Gui 里。
# 注意这个测试要有 QGuiApplication（不是 QCoreApplication）——QTextDocument 依赖字体。
add_executable(test_markdownhighlighter
    test_markdownhighlighter.cpp
)
target_link_libraries(test_markdownhighlighter
    core_document
    Qt6::Core
    Qt6::Gui
)
add_test(NAME markdownhighlighter COMMAND test_markdownhighlighter)

# ---------------- SyncBridge 契约测试 ----------------
# 两件事：① buildLineMap() 扫出的块数要和**真实渲染出的顶层元素个数**一致；
#          ② 预览模板（.qrc 资源）是否具备双向同步必需的东西。
# 因为要读 ":/html/preview_template.html"，所以这里也把 qrc 编进来
# （顺便验证 .qrc 本身是有效的、文件真的在里面）。
add_executable(test_syncbridge
    test_syncbridge.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_link_libraries(test_syncbridge
    core_document
    Qt6::Core
)
add_test(NAME syncbridge COMMAND test_syncbridge)

# ---------------- PreviewRenderer 契约测试 ----------------
# 这个测试**故意不新建任何 QWebEnginePage / QWebEngineView**：真正的页面渲染要 Chromium
# 运行时（受限环境里起不来）。它测的是三样不需要页面的东西：
#   ① buildApplyScript()：HTML + 行号表 打包成 JS 时的转义契约
#   ② baseUrlFromDir()：相对路径图片的基准 URL
#   ③ updateContent() 的防抖时序（只用 QTimer + 事件循环）
# 仍然链接 core_document，而 previewrenderer.cpp 引用了 WebEngine 的符号，
# 所以运行时会把 Qt6WebEngineWidgets.dll 拖进来 —— ctest 的 PATH 已经覆盖。
add_executable(test_previewrenderer
    test_previewrenderer.cpp
)
target_link_libraries(test_previewrenderer
    core_document
    Qt6::Core
)
add_test(NAME previewrenderer COMMAND test_previewrenderer)

# ---------------- FileManager 契约测试 ----------------
# 这个测试会**真的读写磁盘**（临时目录 md-editor-filemanager-test/）：
# 编码检测/转换、四个操作、只读文件、修改标志、以及"保存后磁盘内容真的变了"。
# 链接 core_storage（FileManager）+ infrastructure（FileUtils，测试里用原始字节造样本）；
# core_document 是通过 core_storage 的 PUBLIC 链接传递过来的（filemanager.h 里要用 MarkdownDocument）。
add_executable(test_filemanager
    test_filemanager.cpp
)
target_link_libraries(test_filemanager
    core_storage
    infrastructure
    Qt6::Core
)
add_test(NAME filemanager COMMAND test_filemanager)

# ---------------- VersionControl 契约测试 ----------------
# 这个测试会**真的调用系统 git**（QProcess）：初始化仓库、提交快照、查历史、比差异。
# 机器上没有 git 时它打印 SKIP 并返回 0 —— Git 没装不是本项目代码的问题，
# 不该让整个测试套挂掉。仓库根目录用 setHistoryRoot() 指到临时目录，不碰 AppData 里的真实历史。
add_executable(test_versioncontrol
    test_versioncontrol.cpp
)
target_link_libraries(test_versioncontrol
    core_storage
    infrastructure
    Qt6::Core
)
add_test(NAME versioncontrol COMMAND test_versioncontrol)

# ---------------- CacheManager 契约测试 ----------------
# 只测内存里的缓存逻辑（不碰 git），但需要临时文件来验证"过期判断"（磁盘状态变了要返回 Stale）。
# CacheManager 不是 QObject，没有信号，所以不需要 moc。
add_executable(test_cachemanager
    test_cachemanager.cpp
)
target_link_libraries(test_cachemanager
    core_storage
    infrastructure
    Qt6::Core
)
add_test(NAME cachemanager COMMAND test_cachemanager)

# ---------------- EditorWidget 契约测试（5.1）----------------
# 这个测试要有 **QApplication**（QWidget 依赖它，QGuiApplication 不够），
# 而且它用 QApplication::sendEvent 发**真实按键事件**来验证自动缩进、括号闭合这些行为。
# 链接 business（EditorWidget）+ core_document（MarkdownHighlighter 是它的接口的一部分）。
add_executable(test_editorwidget
    test_editorwidget.cpp
)
target_link_libraries(test_editorwidget
    business
    core_document
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)
add_test(NAME editorwidget COMMAND test_editorwidget)

# ---------------- TabManager 契约测试（5.2）----------------
# 同样需要 QApplication。重点验证"关闭确认回调说取消时标签不会被关掉"，
# 以及"关标签真的把页面释放了"（用 QPointer 观察）。
add_executable(test_tabmanager
    test_tabmanager.cpp
)
target_link_libraries(test_tabmanager
    business
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)
add_test(NAME tabmanager COMMAND test_tabmanager)

# ---------------- EditorWorkbench 契约测试（5.3）----------------
# 需要 QApplication。这个测试**不创建 QWebEngineView**：预览侧传普通 QWidget 就够，
# 分屏、三种显示模式、比例记忆、双向同步桥的接线全都能真验证 ——
# 只有"页面里真的显示出来了"那一步要人工跑 GUI。
# 链接 core_document 是因为要直接用 SyncBridge / PreviewRenderer 的类型和信号。
add_executable(test_editorworkbench
    test_editorworkbench.cpp
)
target_link_libraries(test_editorworkbench
    business
    core_document
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)
add_test(NAME editorworkbench COMMAND test_editorworkbench)

# ---------------- RecentFiles 契约测试（5.4.2）----------------
# 这个测试会真的读写配置文件，但**用 setFilePath() 把配置指到临时目录**，
# 不碰用户真实的 <AppData>/Dev/MarkdownEditor/config.ini。
# 它同时把 ConfigManager（配置层）一起测了：两者是"存取"和"规则"的关系，
# 拆成两个测试程序反而要各写一遍临时目录的准备工作。
# 只需要 QCoreApplication（RecentFiles 是 QObject，要发 changed()）。
add_executable(test_recentfiles
    test_recentfiles.cpp
)
target_link_libraries(test_recentfiles
    business
    infrastructure
    Qt6::Core
)
add_test(NAME recentfiles COMMAND test_recentfiles)

# ---------------- FileTreeView 契约测试（5.4.1）----------------
# 需要 QApplication（QXxxView 依赖它，QFileSystemModel 还依赖事件循环）。
# 三个文件操作（createFile/renamePath/removePath）本来是给右键菜单用的，
# 因为做成了"不弹窗的公开函数"，这里才能直接测 —— 无窗口环境下弹窗是走不通的。
# 全部在临时目录里干活。
add_executable(test_filetreeview
    test_filetreeview.cpp
)
target_link_libraries(test_filetreeview
    business
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)
add_test(NAME filetreeview COMMAND test_filetreeview)

# ---------------- FullTextSearch + SearchPanel 契约测试（5.5）----------------
# 需要 QApplication（SearchPanel 是控件），也需要 Qt6::Sql（索引是真的 SQLite 库）。
# 索引库指向临时目录里的文件，不碰用户真实的 search-index.sqlite。
# 被测的东西分三层：纯函数（查询怎么转义、哪些文件该索引）、真的建索引+搜索
# （中文 2 字/3 字两条路径、增量重建、重启后还在）、面板（搜索→结果→点击跳转的信号）。
add_executable(test_fulltextsearch
    test_fulltextsearch.cpp
)
target_link_libraries(test_fulltextsearch
    business
    infrastructure
    Qt6::Sql
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)
add_test(NAME fulltextsearch COMMAND test_fulltextsearch)

# ---------------- Exporter 契约测试（5.6）----------------
# 需要 QApplication（ExportDialog 是窗口控件），并且要链接 ui（对话框住在那一层）。
# 这个测试**不生成真的 PDF**（那要 Chromium 运行时，而且打印是异步的）：
#   * HTML 导出是同步的，所以能完整验证（结构、样式内联、图片打包、原子写入、失败原因）；
#   * PDF 那边验证的是"选项 → QPageLayout"这条纯规则，以及参数不对时的失败路径。
# "PDF 打出来排版对不对"必须人工看一次（见 README 的验收步骤）。
add_executable(test_exporter
    test_exporter.cpp
)
target_link_libraries(test_exporter
    ui
    business
    infrastructure
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)
add_test(NAME exporter COMMAND test_exporter)

# ---------------- CodeHighlighter 契约测试（5.7）----------------
# 只需要 core_document，但要**带上 qrc**：测试会检查预览模板里的 CSS
# 真的定义了高亮器产出的那些 hljs-* 类名（那是"高亮器 ↔ 样式"之间的契约）。
# 不需要 QApplication：分词和拼 HTML 都是纯字符串处理。
add_executable(test_codehighlighter
    test_codehighlighter.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_link_libraries(test_codehighlighter
    core_document
    Qt6::Core
)
add_test(NAME codehighlighter COMMAND test_codehighlighter)

# ---------------- ThemeManager 契约测试（5.7）----------------
# 需要 QApplication（QSS/调色板是应用级的东西，而且要构造真的控件看颜色）。
# 带上 qrc：两份 QSS 就是从 resources/styles/ 里读的 —— 测试会检查它们真的能读到、
# 选择器清单一致、以及和 ThemePalette（C++ 配色表）的编辑器底色没打架。
# 注意：ConfigManager 会被指向临时文件，不碰用户真实的 config.ini。
add_executable(test_thememanager
    test_thememanager.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_link_libraries(test_thememanager
    business
    infrastructure
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)
add_test(NAME thememanager COMMAND test_thememanager)

# Windows 上测试程序要加载 Qt 的 DLL，而 ctest 是在裸环境里启动程序的，
# 不会自动带上 Qt 的 bin 目录 —— 不设置这里会直接以 0xC0000135（找不到 DLL）失败。
# path_list_prepend 会按平台分隔符把目录加到 PATH 前面；
# 不要手写 "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}"，那样分号会被当成列表分隔符。
set_tests_properties(fileutils markdownparser database markdowndocument markdownhighlighter syncbridge previewrenderer filemanager versioncontrol cachemanager editorwidget tabmanager editorworkbench recentfiles filetreeview fulltextsearch exporter codehighlighter thememanager PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)
# 兜底依赖：改任何头文件都重编本目录的目标。
# 原因见根 CMakeLists.txt 里 md_editor_force_header_deps_here() 上面的长注释
#（本机 CMake 抓不到 /showIncludes，依赖表是空的，会出现新旧 ABI 混用的诡异崩溃）。
md_editor_force_header_deps_here()
