// 主窗口布局的契约测试。
//
// 为什么要有这么一个"读文件的测试"：
//   验收标准是「界面布局完整，所有功能都有菜单/按钮入口，停靠面板可拖拽」。
//   而主窗口**没法在自动化测试里实例化** —— 它里面有一个 QWebEngineView，
//   构造它就要拉起 Chromium（受控环境里起不来）。所以"点开看看"那套在这里走不通。
//
//   退一步：布局和入口本身是"结构"，可以对着源码验证 ——
//     * mainwindow.ui：中央控件是不是 EditorWorkbench、两个停靠面板在不在、
//       文件树有没有真的从中央分屏里挪出去；
//     * mainwindow.cpp：四个菜单在不在、每个菜单里该有的条目在不在、
//       工具栏有没有把那些动作放上去、状态栏有没有那几样信息。
//
//   这是**结构性检查**，不是渲染检查：它保证"入口存在、接线写下来了"，
//   但"点下去好不好看"仍然要靠人跑一次 GUI（README 的验收步骤里写了）。
//   代价是改了这些名字/文案就要顺手改测试 —— 这正是它有用的原因：
//   它把"界面入口"变成了改代码时会立刻发现的东西，而不是等人肉发现少了个菜单。
//
// 需要 QCoreApplication（读文件 + 调 MainWindow 的纯静态函数；窗口本身不构造）。
//
// 注意：为了能调 MainWindow::droppedFiles()（拖拽的规则），这个测试链接了 ui 层。
// 链接 ui 只是因为那个函数住在 mainwindow.cpp 里 —— 测试**不会**构造 MainWindow，
// 所以不会拉起 Chromium。
//
// 跑法：ctest -C Debug --output-on-failure

#include "mainwindow.h"  // 只为调它的纯静态函数 droppedFiles()

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-62s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

QString readFile(const QString &path, bool *ok = nullptr)
{
    QFile file(path);
    const bool opened = file.open(QIODevice::ReadOnly);
    if (ok != nullptr) {
        *ok = opened;
    }
    return opened ? QString::fromUtf8(file.readAll()) : QString();
}

// 从源码里找"这个文件里出现过这些片段"（片段都是可以稳定识别的写法）
QStringList missingPieces(const QString &text, const QStringList &pieces)
{
    QStringList missing;
    for (const QString &piece : pieces) {
        if (!text.contains(piece)) {
            missing << piece;
        }
    }
    return missing;
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 源码树的位置：从可执行文件往上找（build/<...>/bin 下面是 bin 目录）
    // 更简单的办法：用编译期写进来的路径。这里用相对当前目录的方式，
    // 因为 ctest 的工作目录就是构建目录，源码树是它的上级两级。
    QString root = QDir::currentPath();
    if (!QFileInfo::exists(root + QStringLiteral("/src/ui/mainwindow.ui"))) {
        // 退一步：从可执行文件位置往上找（Qt Creator 直接跑时是 build/.../bin）
        QDir dir(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 6 && !QFileInfo::exists(dir.filePath(QStringLiteral("src/ui/mainwindow.ui"))); ++i) {
            dir.cdUp();
        }
        root = dir.absolutePath();
    }

    const QString uiPath = root + QStringLiteral("/src/ui/mainwindow.ui");
    const QString cppPath = root + QStringLiteral("/src/ui/mainwindow.cpp");

    bool uiOk = false;
    bool cppOk = false;
    const QString ui = readFile(uiPath, &uiOk);
    const QString cpp = readFile(cppPath, &cppOk);

    std::printf("---- 源码树: %s ----\n", root.toUtf8().constData());
    check(uiOk, QStringLiteral("能找到 mainwindow.ui"), uiPath);
    check(cppOk, QStringLiteral("能找到 mainwindow.cpp"), cppPath);
    if (!uiOk || !cppOk) {
        std::printf("\n有失败项（找不到源码文件）\n");
        return 1;
    }

    // ============================ A. 中央区域与停靠面板（.ui）============================
    {
        std::printf("---- A. 中央区域与停靠面板 ----\n");

        check(ui.contains(QStringLiteral("<widget class=\"EditorWorkbench\" name=\"workbench\">")),
              QStringLiteral("中央控件: 是 EditorWorkbench（分屏编辑预览）"));
        check(ui.contains(QStringLiteral("<class>MainWindow</class>")),
              QStringLiteral("中央控件: 挂在 MainWindow 里（不是独立窗口）"));

        // 中央区里只应该有 tabManager 与 preview —— 文件树不再是分屏的一块
        const int lastWorkbenchEnd = ui.lastIndexOf(QStringLiteral("</widget>"));
        Q_UNUSED(lastWorkbenchEnd);
        check(ui.contains(QStringLiteral("<widget class=\"TabManager\" name=\"tabManager\">")),
              QStringLiteral("中央区: 左边是多标签管理器"));
        check(ui.contains(QStringLiteral("<widget class=\"QWebEngineView\" name=\"preview\">")),
              QStringLiteral("中央区: 右边是预览视图"));

        // 两个停靠面板
        check(ui.contains(QStringLiteral("<widget class=\"QDockWidget\" name=\"fileTreeDock\">")),
              QStringLiteral("停靠面板: 文件树是 QDockWidget（可拖拽/可关闭）"));
        check(ui.contains(QStringLiteral("<widget class=\"FileTreeView\" name=\"fileTree\"/>")),
              QStringLiteral("停靠面板: 文件树控件在面板里"));
        check(ui.contains(QStringLiteral("<widget class=\"QDockWidget\" name=\"searchDock\">")),
              QStringLiteral("停靠面板: 搜索面板是 QDockWidget"));
        check(ui.contains(QStringLiteral("<widget class=\"SearchPanel\" name=\"searchPanel\"/>")),
              QStringLiteral("停靠面板: 搜索面板控件在面板里"));

        // 停靠区：文件树在左（1 = LeftDockWidgetArea），搜索在底部（4 = BottomDockWidgetArea）
        const int fileDockAt = ui.indexOf(QStringLiteral("name=\"fileTreeDock\""));
        const int searchDockAt = ui.indexOf(QStringLiteral("name=\"searchDock\""));
        const QString fileDockBlock = ui.mid(fileDockAt, 400);
        const QString searchDockBlock = ui.mid(searchDockAt, 400);
        check(fileDockBlock.contains(QStringLiteral("<number>1</number>")),
              QStringLiteral("停靠区: 文件树默认在左侧"));
        check(searchDockBlock.contains(QStringLiteral("<number>4</number>")),
              QStringLiteral("停靠区: 搜索面板默认在底部"));

        // 文件树必须**不在** workbench 里了（否则中央分屏又变成三块）
        const int workbenchStart = ui.indexOf(QStringLiteral("name=\"workbench\""));
        const int workbenchEnd = ui.indexOf(QStringLiteral("name=\"fileTreeDock\""));
        const QString workbenchBlock = ui.mid(workbenchStart, workbenchEnd - workbenchStart);
        check(!workbenchBlock.contains(QStringLiteral("FileTreeView")),
              QStringLiteral("结构: 文件树已经不在中央分屏里（中央只剩编辑器+预览）"));
        check(workbenchBlock.contains(QStringLiteral("tabManager")) && workbenchBlock.contains(QStringLiteral("preview")),
              QStringLiteral("结构: 中央分屏里确实是编辑器 + 预览两块"));
    }

    // ============================ B. 菜单栏（mainwindow.cpp）============================
    {
        std::printf("---- B. 菜单栏 ----\n");

        const QStringList menus = {QStringLiteral("文件(&F)"), QStringLiteral("编辑(&E)"), QStringLiteral("视图(&V)"),
                                   QStringLiteral("帮助(&H)")};
        const QStringList missingMenus = missingPieces(cpp, menus);
        check(missingMenus.isEmpty(), QStringLiteral("菜单: 文件 / 编辑 / 视图 / 帮助 四个都在"),
              missingMenus.join(QStringLiteral(", ")));

        // 文件菜单：新建、打开、保存、另存为、最近文件、导出、退出
        const QStringList fileItems = {QStringLiteral("新建标签(&N)"), QStringLiteral("打开(&O)…"),
                                       QStringLiteral("保存(&S)"),      QStringLiteral("另存为(&A)…"),
                                       QStringLiteral("最近打开(&R)"),  QStringLiteral("导出(&E)"),
                                       QStringLiteral("退出(&Q)")};
        const QStringList missingFile = missingPieces(cpp, fileItems);
        check(missingFile.isEmpty(), QStringLiteral("文件菜单: 新建/打开/保存/另存为/最近文件/导出/退出 都有"),
              missingFile.join(QStringLiteral(", ")));
        check(cpp.contains(QStringLiteral("导出为 HTML(&H)…")) && cpp.contains(QStringLiteral("导出为 PDF(&P)…")),
              QStringLiteral("文件菜单: 导出子菜单里有 HTML 和 PDF 两项"));

        // 编辑菜单：撤销、重做、复制、粘贴、查找、替换
        const QStringList editItems = {QStringLiteral("撤销(&U)"), QStringLiteral("重做(&R)"),
                                       QStringLiteral("复制(&C)"), QStringLiteral("粘贴(&P)"),
                                       QStringLiteral("查找(&F)…"), QStringLiteral("替换(&H)…")};
        const QStringList missingEdit = missingPieces(cpp, editItems);
        check(missingEdit.isEmpty(), QStringLiteral("编辑菜单: 撤销/重做/复制/粘贴/查找/替换 都有"),
              missingEdit.join(QStringLiteral(", ")));
        check(cpp.contains(QStringLiteral("initEditActions")) && cpp.contains(QStringLiteral("&QPlainTextEdit::undo")),
              QStringLiteral("编辑菜单: 这些动作真的接到编辑器上了（不是空菜单）"));
        check(cpp.contains(QStringLiteral("QKeySequence::Undo")) && cpp.contains(QStringLiteral("QKeySequence::Find")),
              QStringLiteral("编辑菜单: 快捷键用的是标准键（Ctrl+Z / Ctrl+F）"));

        // 视图菜单：侧边栏开关、预览模式、主题
        check(cpp.contains(QStringLiteral("文件树(&F)")) && cpp.contains(QStringLiteral("全文搜索面板(&S)")),
              QStringLiteral("视图菜单: 有侧边栏（文件树/搜索面板）的显示隐藏开关"));
        check(cpp.contains(QStringLiteral("左右分屏(&1)")) && cpp.contains(QStringLiteral("仅编辑(&2)"))
                  && cpp.contains(QStringLiteral("仅预览(&3)")),
              QStringLiteral("视图菜单: 三种预览模式都在"));
        check(cpp.contains(QStringLiteral("主题(&T)")) && cpp.contains(QStringLiteral("亮色(&L)"))
                  && cpp.contains(QStringLiteral("暗色(&D)")),
              QStringLiteral("视图菜单: 主题子菜单（亮色/暗色）"));

        // 帮助菜单：关于
        check(cpp.contains(QStringLiteral("关于(&A)…")) && cpp.contains(QStringLiteral("void MainWindow::onAbout()")),
              QStringLiteral("帮助菜单: 关于（而且真的实现了 onAbout）"));
        check(cpp.contains(QStringLiteral("QMessageBox::about(")), QStringLiteral("帮助菜单: 关于用的是 QMessageBox::about"));
    }

    // ============================ C. 工具栏与状态栏 ============================
    {
        std::printf("---- C. 工具栏与状态栏 ----\n");

        // 工具栏：新建、打开、保存、撤销、重做、切换主题、切换预览
        const QStringList toolbarItems = {QStringLiteral("toolBar->addAction(m_newAction)"),
                                          QStringLiteral("toolBar->addAction(m_openAction)"),
                                          QStringLiteral("toolBar->addAction(m_saveAction)"),
                                          QStringLiteral("toolBar->addAction(m_undoAction)"),
                                          QStringLiteral("toolBar->addAction(m_redoAction)"),
                                          QStringLiteral("m_togglePreviewAction"),
                                          QStringLiteral("m_darkThemeAction")};
        const QStringList missingToolbar = missingPieces(cpp, toolbarItems);
        check(missingToolbar.isEmpty(),
              QStringLiteral("工具栏: 新建/打开/保存/撤销/重做/切换预览/切换主题 都放上去了"),
              missingToolbar.join(QStringLiteral(", ")));
        check(cpp.contains(QStringLiteral("m_togglePreviewAction->setCheckable(true)"))
                  && cpp.contains(QStringLiteral("m_darkThemeAction->setCheckable(true)")),
              QStringLiteral("工具栏: 两个开关按钮是 checkable（一眼看出当前状态）"));

        // 状态栏：行号列号、字符数、路径、修改状态
        const QStringList statusLabels = {QStringLiteral("m_cursorLabel = new QLabel"),
                                          QStringLiteral("m_charCountLabel = new QLabel"),
                                          QStringLiteral("m_pathLabel = new QLabel"),
                                          QStringLiteral("m_modifiedLabel = new QLabel")};
        const QStringList missingLabels = missingPieces(cpp, statusLabels);
        check(missingLabels.isEmpty(),
              QStringLiteral("状态栏: 光标/字符数/路径/修改状态 四个标签都在"),
              missingLabels.join(QStringLiteral(", ")));
        check(cpp.contains(QStringLiteral("行 %1，列 %2")), QStringLiteral("状态栏: 显示了行号列号"));
        check(cpp.contains(QStringLiteral("字符 %1 · 行 %2")), QStringLiteral("状态栏: 显示了字符数与行数"));
        check(cpp.contains(QStringLiteral("● 未保存")) && cpp.contains(QStringLiteral("○ 已保存")),
              QStringLiteral("状态栏: 修改状态不只靠颜色（同时有 ● / ○ 形状）"));
        check(cpp.contains(QStringLiteral("addPermanentWidget")),
              QStringLiteral("状态栏: 常驻信息用 addPermanentWidget（不会被临时消息顶掉）"));
        check(cpp.contains(QStringLiteral("void MainWindow::updateDocumentStatus()")),
              QStringLiteral("状态栏: 有一个统一刷新入口 updateDocumentStatus()"));
    }

    // ============================ D. 入口都能点到实处 ============================
    {
        std::printf("---- D. 接线是否落到实体上 ----\n");

        // 每个菜单项/按钮背后都得有真的实现（不能只是加了个 action）
        const QStringList implementations = {
            QStringLiteral("void MainWindow::onNewFile()"),      QStringLiteral("void MainWindow::onOpenFile()"),
            QStringLiteral("void MainWindow::onSaveFile()"),     QStringLiteral("void MainWindow::onSaveFileAs()"),
            QStringLiteral("void MainWindow::onExportHtml()"),   QStringLiteral("void MainWindow::onExportPdf()"),
            QStringLiteral("void MainWindow::onFind()"),         QStringLiteral("void MainWindow::onReplace()"),
            QStringLiteral("void MainWindow::onAbout()"),        QStringLiteral("void MainWindow::onThemeChanged("),
        };
        const QStringList missingImpl = missingPieces(cpp, implementations);
        check(missingImpl.isEmpty(), QStringLiteral("接线: 菜单项对应的实现函数都存在"),
              missingImpl.join(QStringLiteral(", ")));

        // 查找/替换必须真的接到编辑器上（不是弹个空框）
        check(cpp.contains(QStringLiteral("m_findDialog.setEditor(")),
              QStringLiteral("接线: 查找对话框绑定到了当前编辑器"));
        const QString findDialogPath = root + QStringLiteral("/src/business/findreplacedialog.cpp");
        const QString findDialog = readFile(findDialogPath);
        check(!findDialog.isEmpty(), QStringLiteral("接线: 查找对话框的实现文件在"), findDialogPath);
        check(findDialog.contains(QStringLiteral("m_editor->findNext("))
                  && findDialog.contains(QStringLiteral("m_editor->replaceAll(")),
              QStringLiteral("接线: 对话框把查找/替换真的转发给了编辑器"));

        // 停靠面板的显示隐藏由视图菜单控制
        check(cpp.contains(QStringLiteral("ui->fileTreeDock->setVisible(")),
              QStringLiteral("接线: 文件树面板的开关控制的是停靠面板"));
        check(cpp.contains(QStringLiteral("ui->searchDock->setVisible(")),
              QStringLiteral("接线: 搜索面板的开关控制的是停靠面板"));
        check(cpp.contains(QStringLiteral("visibilityChanged, m_showFileTreeAction"))
                  && cpp.contains(QStringLiteral("visibilityChanged, m_searchAction")),
              QStringLiteral("接线: 面板被拖走/关掉时菜单勾选会自动同步"));
    }

    // ============================ E. 快捷键（6.2）============================
    {
        std::printf("---- E. 快捷键 ----\n");

        // 快捷键是"看得见才记得住"的东西，所以一边设一边在结构上钉住。
        // 前三组用 Qt 的标准键（QKeySequence::New 在 Windows 上就是 Ctrl+N）。
        const QStringList standardKeys = {QStringLiteral("QKeySequence::New"), QStringLiteral("QKeySequence::Open"),
                                          QStringLiteral("QKeySequence::Save"), QStringLiteral("QKeySequence::Undo"),
                                          QStringLiteral("QKeySequence::Redo"), QStringLiteral("QKeySequence::Find")};
        const QStringList missingKeys = missingPieces(cpp, standardKeys);
        check(missingKeys.isEmpty(),
              QStringLiteral("快捷键: 新建/打开/保存/撤销/重做/查找 都绑了标准键（Ctrl+N/O/S/Z/Y/F）"),
              missingKeys.join(QStringLiteral(", ")));

        // 重做额外认 Ctrl+Y（有些习惯是从别的编辑器带过来的）
        check(cpp.contains(QStringLiteral("QKeySequence(Qt::CTRL | Qt::Key_Y)")),
              QStringLiteral("快捷键: 重做额外绑了 Ctrl+Y"));

        // 切换预览 = F11、切换主题 = Ctrl+Shift+T
        check(cpp.contains(QStringLiteral("QKeySequence(Qt::Key_F11)")),
              QStringLiteral("快捷键: 切换预览绑了 F11"));
        check(cpp.contains(QStringLiteral("QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T)")),
              QStringLiteral("快捷键: 切换主题绑了 Ctrl+Shift+T"));

        // 替换是 Ctrl+H（QKeySequence 没有标准的 Replace）
        check(cpp.contains(QStringLiteral("Qt::CTRL | Qt::Key_H")), QStringLiteral("快捷键: 替换绑了 Ctrl+H"));
    }

    // ============================ F. 拖拽打开（6.2）============================
    {
        std::printf("---- F. 拖拽打开 ----\n");

        check(cpp.contains(QStringLiteral("setAcceptDrops(true)")),
              QStringLiteral("拖拽: 窗口开了接受拖放"));
        check(cpp.contains(QStringLiteral("void MainWindow::dragEnterEvent("))
                  && cpp.contains(QStringLiteral("void MainWindow::dropEvent(")),
              QStringLiteral("拖拽: dragEnterEvent 与 dropEvent 都实现了"));
        check(cpp.contains(QStringLiteral("event->acceptProposedAction()")),
              QStringLiteral("拖拽: 接受时会 acceptProposedAction（鼠标不再显示禁止图标）"));

        // ---- 真正的逻辑：从 MIME 数据里挑文件（纯函数，直接调）----
        // 这个函数不碰窗口，所以能在这里真跑一遍 —— 它是"拖什么能打开"的全部规则。
        const QString tempDir = QDir::tempPath() + QStringLiteral("/md-editor-drop-test");
        QDir(tempDir).removeRecursively();
        QDir().mkpath(tempDir);

        const QString mdFile = tempDir + QStringLiteral("/note.md");
        const QString markdownFile = tempDir + QStringLiteral("/readme.markdown");
        const QString txtFile = tempDir + QStringLiteral("/plain.txt");
        const QString exeFile = tempDir + QStringLiteral("/app.exe");
        for (const QString &path : {mdFile, markdownFile, txtFile, exeFile}) {
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write("x");
        }

        QMimeData data;
        QList<QUrl> urls;
        urls << QUrl::fromLocalFile(mdFile) << QUrl::fromLocalFile(markdownFile) << QUrl::fromLocalFile(txtFile)
             << QUrl::fromLocalFile(exeFile) << QUrl(QStringLiteral("https://example.com/remote.md"));
        data.setUrls(urls);

        const QStringList dropped = MainWindow::droppedFiles(&data);
        check(dropped.size() == 3, QStringLiteral("拖拽: 只收 md / markdown / txt（exe 和网址被挡掉）"),
              QStringLiteral("%1 个：%2").arg(dropped.size()).arg(dropped.join(QStringLiteral(", "))));
        check(dropped.contains(mdFile) && dropped.contains(markdownFile) && dropped.contains(txtFile),
              QStringLiteral("拖拽: 三个文本文件都在"));
        check(!dropped.contains(exeFile), QStringLiteral("拖拽: .exe 不会被打开（避免把二进制灌进编辑器）"));
        check(MainWindow::droppedFiles(nullptr).isEmpty(), QStringLiteral("拖拽: 空数据 -> 空（不崩）"));

        QMimeData textOnly;
        textOnly.setText(QStringLiteral("拖的是文字，不是文件"));
        check(MainWindow::droppedFiles(&textOnly).isEmpty(), QStringLiteral("拖拽: 拖来一段文字 -> 不打开"));

        // 拖一个目录进来 = 打开里面第一层的 md（不递归）
        const QString subDir = tempDir + QStringLiteral("/notes");
        QDir().mkpath(subDir);
        QFile inner(subDir + QStringLiteral("/inner.md"));
        inner.open(QIODevice::WriteOnly);
        inner.write("x");
        QMimeData dirData;
        dirData.setUrls({QUrl::fromLocalFile(subDir)});
        const QStringList fromDir = MainWindow::droppedFiles(&dirData);
        check(fromDir.size() == 1 && fromDir.first() == subDir + QStringLiteral("/inner.md"),
              QStringLiteral("拖拽: 拖目录 -> 打开里面的 .md"), fromDir.join(QStringLiteral(", ")));

        QDir(tempDir).removeRecursively();
    }

    // ============================ G. 窗口记忆（6.2）============================
    {
        std::printf("---- G. 窗口记忆 ----\n");

        check(cpp.contains(QStringLiteral("void MainWindow::restoreSession()"))
                  && cpp.contains(QStringLiteral("void MainWindow::saveSession() const")),
              QStringLiteral("记忆: 有恢复与保存两个函数"));
        check(cpp.contains(QStringLiteral("restoreGeometry(state.geometry)")),
              QStringLiteral("记忆: 用 Qt 的 restoreGeometry（多显示器/DPI 由 Qt 处理）"));
        check(cpp.contains(QStringLiteral("saveGeometry()")), QStringLiteral("记忆: 用 saveGeometry 存几何"));
        check(cpp.contains(QStringLiteral("SessionState::load()")) && cpp.contains(QStringLiteral("SessionState::save(")),
              QStringLiteral("记忆: 读写都走 SessionState（那一层能自动测）"));
        check(cpp.contains(QStringLiteral("saveSession();")) && cpp.contains(QStringLiteral("event->accept();")),
              QStringLiteral("记忆: 关窗口时保存（而且是在用户确认关闭之后）"));
        check(cpp.contains(QStringLiteral("QFileInfo::exists(path)")),
              QStringLiteral("记忆: 上次的文件在磁盘上没了就跳过（不弹窗打扰）"));
    }

    // ============================ H. 构建清单一致性（.cpp/.h 有没有登记进 CMakeLists）============================
    //
    // 为什么加这一段：这个项目的"验证构建"是手写 cl/link 的（沙箱里 cmake 配不动），
    // **完全绕过 CMake**。于是"新加了一个 .cpp 却忘了写进模块的 CMakeLists"这种错误
    // 在我的验证里永远看不见，只有在 Qt Creator 里构建才会炸出来（典型表现是
    // LNK2019 找不到某个类的成员函数 —— 那个文件根本没被编译）。
    // 真实踩过一次（SessionState），所以这里用测试把它钉住：源码树里每个 .cpp/.h
    // 都必须出现在它所属模块的 CMakeLists.txt 里。
    {
        std::printf("---- H. 构建清单一致性 ----\n");

        struct Module
        {
            const char *dir;   // 源码目录
            const char *cml;   // 它对应的 CMakeLists.txt
        };
        const QList<Module> modules = {
            {"src/infrastructure", "src/infrastructure/CMakeLists.txt"},
            {"src/core/document", "src/core/document/CMakeLists.txt"},
            {"src/core/storage", "src/core/storage/CMakeLists.txt"},
            {"src/business", "src/business/CMakeLists.txt"},
            {"src/ui", "src/ui/CMakeLists.txt"},
            {"src/app", "src/app/CMakeLists.txt"},
        };

        int checked = 0;
        QStringList unregistered;
        for (const Module &module : modules) {
            QDir dir(root + QLatin1Char('/') + QLatin1String(module.dir));
            const QString manifestPath = root + QLatin1Char('/') + QLatin1String(module.cml);
            const QString manifest = readFile(manifestPath);
            if (manifest.isEmpty()) {
                unregistered << QStringLiteral("(读不到 %1)").arg(manifestPath);
                continue;
            }
            const QFileInfoList files = dir.entryInfoList(QStringList{QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files);
            for (const QFileInfo &file : files) {
                ++checked;
                if (!manifest.contains(file.fileName())) {
                    unregistered << QStringLiteral("%1/%2").arg(QLatin1String(module.dir), file.fileName());
                }
            }
        }
        check(!unregistered.isEmpty() ? false : true,
              QStringLiteral("清单: 源码树里每个 .cpp/.h 都写进了所属模块的 CMakeLists（共 %1 个）").arg(checked),
              unregistered.join(QStringLiteral(", ")));
        check(checked > 40, QStringLiteral("清单: 这次真的检查了足够多的文件（不是空跑）"),
              QStringLiteral("%1 个").arg(checked));

        // 测试文件也要登记，否则 ctest 里会少一个而没人发现
        const QString testsManifest = readFile(root + QStringLiteral("/tests/CMakeLists.txt"));
        QStringList missingTests;
        const QFileInfoList testFiles =
            QDir(root + QStringLiteral("/tests")).entryInfoList(QStringList{QStringLiteral("*.cpp")}, QDir::Files);
        for (const QFileInfo &file : testFiles) {
            if (!testsManifest.contains(file.fileName())) {
                missingTests << file.fileName();
            }
        }
        check(missingTests.isEmpty(),
              QStringLiteral("清单: 每个测试文件都登记在 tests/CMakeLists.txt（共 %1 个）").arg(testFiles.size()),
              missingTests.join(QStringLiteral(", ")));

        // 资源文件也要登记在 .qrc 里，否则运行期读不到（样式表/模板都是这么进 exe 的）
        const QString qrc = readFile(root + QStringLiteral("/resources/resources.qrc"));
        QStringList missingResources;
        QDirIterator it(root + QStringLiteral("/resources"), QStringList{QStringLiteral("*.html"), QStringLiteral("*.qss")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString relative = QDir(root + QStringLiteral("/resources")).relativeFilePath(path);
            if (!qrc.contains(relative)) {
                missingResources << relative;
            }
        }
        check(missingResources.isEmpty(), QStringLiteral("清单: 每个资源文件都登记在 resources.qrc"),
              missingResources.join(QStringLiteral(", ")));
    }

    // ============================ I. 「写了但没接线」检查 ============================
    //
    // 7.1 全链路测试时抓到的真 bug：initEditActions() 写了、却**从来没被调用** ——
    // 于是运行起来「编辑」菜单根本不会出现，而结构测试却放过了它（它只查"文件里有没有
    // 这些字符串"，不查"有没有被调用"）。
    //
    // 规则：mainwindow.cpp 里每个 `MainWindow::xxx` 的定义，它的名字在文件里必须出现
    // **不止一次**（定义之外至少还有一次调用或 connect）。槽函数是通过
    // &MainWindow::onXxx 连的，也算出现 ✓。只出现一次 = 死代码或者忘了接。
    //
    // 例外：Qt 通过虚函数表调用的那几个（closeEvent / dragEnterEvent / dropEvent …）
    // 本来就不会被显式调用，所以**在头文件里带 override 的一律跳过** ——
    // 这条例外是写这道检查时被自己的假阳性打出来的（它当时报了 closeEvent）。
    {
        std::printf("---- I. 写了但没接线 ----\n");

        const QString header = readFile(root + QStringLiteral("/src/ui/mainwindow.h"));

        static const QRegularExpression definitionRe(QStringLiteral("MainWindow::([A-Za-z_][A-Za-z0-9_]*)\\s*\\("));
        QRegularExpressionMatchIterator it = definitionRe.globalMatch(cpp);

        QStringList defined;
        while (it.hasNext()) {
            const QString name = it.next().captured(1);
            if (!defined.contains(name)) {
                defined << name;
            }
        }
        check(defined.size() > 30, QStringLiteral("接线: 从 mainwindow.cpp 里解析出了足够多的成员函数"),
              QStringLiteral("%1 个").arg(defined.size()));

        int skippedOverrides = 0;
        QStringList neverCalled;
        for (const QString &name : defined) {
            // 头文件里声明成 override 的（Qt 自己会调）：跳过
            const QRegularExpression overrideForThisName(
                QStringLiteral("\\b%1\\s*\\([^;]*\\)\\s*override").arg(QRegularExpression::escape(name)));
            if (overrideForThisName.match(header).hasMatch()) {
                ++skippedOverrides;
                continue;
            }

            int occurrences = 0;
            int from = 0;
            while ((from = cpp.indexOf(name, from)) >= 0) {
                ++occurrences;
                from += name.size();
            }
            if (occurrences < 2) {
                neverCalled << name;
            }
        }
        check(!neverCalled.isEmpty() ? false : true,
              QStringLiteral("接线: 每个成员函数都至少被调用/连接过一次（没有写了却不接线的）"),
              neverCalled.join(QStringLiteral(", ")));
        check(skippedOverrides >= 3,
              QStringLiteral("接线: 跳过的事件处理函数（override）至少有 3 个：关窗 + 拖拽那两个"),
              QStringLiteral("%1 个").arg(skippedOverrides));

        // 构造函数里那几件事必须真的被调过（这是上面那条规则最关心的几处）
        const QStringList mustBeCalled = {QStringLiteral("initUi();"),          QStringLiteral("initMenuBar();"),
                                          QStringLiteral("initEditActions();"),  QStringLiteral("initToolBar();"),
                                          QStringLiteral("initStatusBar();"),    QStringLiteral("restoreSession();"),
                                          QStringLiteral("createSession();")};
        const QStringList missingCalls = missingPieces(cpp, mustBeCalled);
        check(missingCalls.isEmpty(), QStringLiteral("接线: 初始化/会话恢复那几件事都在构造函数里调用了"),
              missingCalls.join(QStringLiteral(", ")));
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
