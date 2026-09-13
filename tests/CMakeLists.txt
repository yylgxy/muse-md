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

# Windows 上测试程序要加载 Qt 的 DLL，而 ctest 是在裸环境里启动程序的，
# 不会自动带上 Qt 的 bin 目录 —— 不设置这里会直接以 0xC0000135（找不到 DLL）失败。
# path_list_prepend 会按平台分隔符把目录加到 PATH 前面；
# 不要手写 "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}"，那样分号会被当成列表分隔符。
set_tests_properties(fileutils markdownparser database markdowndocument markdownhighlighter syncbridge previewrenderer filemanager versioncontrol cachemanager PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)
