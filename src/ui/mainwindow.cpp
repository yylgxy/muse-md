#include "mainwindow.h"

#include "ui_mainwindow.h"  // uic 根据 mainwindow.ui 生成（AUTOUIC 负责，不用手工写）

#include "editorwidget.h"       // .ui 里的 tabManager 会用它的页面；界面要连它的 cursorMoved
#include "codehighlighter.h"    // 5.7：代码高亮（"插入代码块"的语言列表就是它提供的）
#include "editorworkbench.h"    // .ui 中央区那台"工作台"（分屏 + 预览 + 双向同步）
#include "exportdialog.h"       // 5.6：导出对话框（只收集设置，干活的是 Exporter）
#include "filetreeview.h"       // 5.4.1：左边的文件树侧边栏（.ui 里已经有一块 FileTreeView）
#include "logger.h"
#include "previewrenderer.h"    // updateContent() 要用完整类型（渲染管线归工作台持有，这里只是借来用）
#include "recentfiles.h"
#include "syncbridge.h"      // 帧统计：预览网页报回来的数据走它        // 5.4.2：最近打开的文件列表
#include "searchpanel.h"        // 5.5：全文搜索面板（.ui 里就是一个 SearchPanel）
#include "outlinepanel.h"       // C4：大纲面板（.ui 里就是一个 OutlinePanel）
#include "textstats.h"          // C5：写作统计（纯函数，进 core/document）
#include "tabmanager.h"
#include "thememanager.h"      // 5.7：亮暗主题（单例）

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>  // 文件树/搜索面板是停靠窗口：要调 setFeatures 得用它
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>  // 启动第二阶段：QTimer::singleShot(0, ...)
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

using markdown_editor::core::storage::CacheManager;
using markdown_editor::core::storage::VersionControl;
// 渲染管线的类型要写全名：它和 FileManager 不一样，以前只用到它的成员函数、不用提名字，
// 现在要连它的信号（contentSkipped / rendererRestarted），所以需要这个 using。
using markdown_editor::core::document::PreviewRenderer;
using markdown_editor::core::document::SyncBridge;  // 帧统计：预览网页报回来的那份数据走它
// 代码高亮（5.7）："插入代码块"的语言列表和显示名都来自它
using markdown_editor::core::document::CodeHighlighter;
// 主题配色（5.7）：ThemeManager 是全局命名空间的类，但它返回的配色表在 core::document 里
using markdown_editor::core::document::ThemePalette;
// 自研行级 diff（B1）："与上一版对比"改用它（本地算法 + 结构化结果）
using markdown_editor::core::document::LineDiff;
using markdown_editor::core::document::TextStats;  // C5：写作统计（纯函数）
// 注意：FileManager 不用在这里 using —— MainWindow 内部有一份同名别名（见 mainwindow.h），
// 成员函数体里直接用短名字就行，不会和全局作用域冲突。
// 预览渲染管线与同步桥也不在这里了：它们归 EditorWorkbench 所有。

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    // ★ 一行顶掉原来"建 splitter / 建 editor / 建 preview / 加进布局"那一堆：
    //   控件和布局现在由 mainwindow.ui 描述，uic 生成代码，这里只负责"装上去"。
    //   装好之后就能用 ui->tabManager / ui->preview / ui->splitter。
    ui->setupUi(this);

    initUi();
    initMenuBar();
    initToolBar();
    initStatusBar();

    // ---- 主题（5.7）----
    // 先接信号再接"应用保存的主题"：这样启动时那一次应用也会走到 onThemeChanged，
    // 编辑器和预览区就能用同一个入口同步好（不需要在构造函数里再手动同步一遍）。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &MainWindow::onThemeChanged);
    ThemeManager::instance().applySavedTheme();

    // 最近文件（5.4.2）：先把上次退出时的列表读回来，再据此建出菜单。
    // 菜单也要能自更新 —— changed() 是唯一的"列表变了"通知（加一条、删一条、清空都会发）。
    m_recent.load();
    connect(&m_recent, &RecentFiles::changed, this, &MainWindow::rebuildRecentMenu);
    rebuildRecentMenu();

    // 导出（5.6）：PDF 是异步的（printToPdf 走 Chromium 的打印管线），结果从这里回来。
    // 导出器是窗口的成员，所以"导出还没结束窗口就关了"这种情况不会发生。
    connect(&m_exporter, &Exporter::pdfExported, this, [this](const QString &path, bool ok, const QString &error) {
        if (!ok) {
            QMessageBox::warning(this,
                                 QStringLiteral("导出失败"),
                                 error.isEmpty() ? QStringLiteral("没能导出 PDF") : error);
            statusBar()->showMessage(QStringLiteral("导出 PDF 失败"), 8000);
            return;
        }
        statusBar()->showMessage(QStringLiteral("已导出：%1").arg(QDir::toNativeSeparators(path)), 8000);
        offerToOpenExportedFile(path, true);
    });

    // 先开一个空标签，保证界面上永远有一个可编辑的地方（后面所有代码就能少写一堆判空）
    createSession();
    updateWindowTitle();

    // ---- 性能：编辑器内容的延迟同步（P0-1）----
    // 打字时 onEditorTextChanged 只调用 markDirty()（O(1)），真正的
    // "toPlainText() + setText" 在这里、而且只在停手 150ms 之后做一次。
    m_syncScheduler.setSyncHandler([this](EditorWidget *editor) { syncEditorIntoFiles(editor); });
    m_syncScheduler.setDelay(EditorSyncScheduler::kDefaultDelayMs);

    // ---- 拖拽打开（6.2）----
    // 主窗口接受拖放：把 .md 从资源管理器拖进来就能打开（实现在 dragEnterEvent/dropEvent）
    setAcceptDrops(true);

    // ---- 启动：先让窗口显示，再把非核心的活排到下一轮（7.2 启动优化）----
    // 恢复会话要读盘（可能好几个文件）、文件树要开始监听一个目录，
    // 这些都不该挡在"窗口出现"前面。用 0 毫秒的单次定时器把它们排到事件循环的下一轮：
    // 用户先看到界面，然后再看到文件一个个出现。耗时会在日志里报出来（便于对比优化前后）。
    QTimer::singleShot(0, this, &MainWindow::finishStartup);
}

// 启动的第二阶段：窗口已经显示出来之后的"非核心"工作。
void MainWindow::finishStartup()
{
    QElapsedTimer timer;
    timer.start();

    // 文件树从这里开始监听目录。放在这里而不是构造函数里：大目录的首次列目录
    // 是有真实开销的（QFileSystemModel 要起监听线程并读一遍目录），
    // 它不该拖慢"窗口出现"。
    ui->fileTree->setRootPath(QDir::currentPath());
    syncSearchDirectoryToSidebar();

    // 恢复上次的会话（打开那些文件是这里最费时的一步）
    restoreSession();

    LOG_INFO("启动：非核心部分完成（文件树 + 会话恢复），耗时 %1 ms", timer.elapsed());
}

MainWindow::~MainWindow()
{
    delete ui;  // ui 是 new 出来的，析构里要还回去
}

// ============================ 界面 ============================

void MainWindow::initUi()
{
    // 这里是"控件建好之后的接线和配置"，不是布局 —— 所以仍然在代码里。

    // ---- 把中央区的左右两块交给工作台 ----
    // 工作台负责：左右分屏（QSplitter）、三种显示模式、编辑器与预览的双向同步。
    // .ui 只描述"左边 tabManager、右边 preview"这个结构，行为都在 EditorWorkbench 里。
    // 它内部会顺手做掉：给预览换页面（转发 JS 日志）、建 WebChannel、把同步桥注册给页面、
    // 加载预览模板 —— 这些原来都挤在主窗口里。
    if (!ui->workbench->setup(ui->tabManager, ui->preview)) {
        LOG_ERROR("工作台初始化失败：编辑器侧或预览侧缺了一块");
    }

    // 预览被点击 → 工作台已经替我们把当前标签的光标跳过去了，这里只负责"界面表达"
    connect(ui->workbench, &EditorWorkbench::editorLineClicked, this, &MainWindow::onEditorLineClicked);
    // 显示模式变化 → 同步菜单勾选（也可能是代码里改的，所以以信号为准）
    connect(ui->workbench, &EditorWorkbench::viewModeChanged, this, &MainWindow::onViewModeChanged);
    onViewModeChanged(ui->workbench->viewMode());

    // 预览侧的两种"异常情况"必须让用户看得见，否则预览一片空白时人根本不知道发生了什么：
    //   * contentSkipped：内容太大被跳过（或者渲染进程反复崩溃后放弃了自动恢复）
    //   * rendererRestarted：渲染进程崩了，正在自动恢复（几秒后就自己好了）
    connect(ui->workbench->renderer(), &PreviewRenderer::contentSkipped, this, [this](const QString &reason) {
        LOG_WARN("预览:%1", reason);
        statusBar()->showMessage(reason, 10000);
    });
    connect(ui->workbench->renderer(), &PreviewRenderer::rendererRestarted, this, [this](int attempt) {
        statusBar()->showMessage(QStringLiteral("预览的渲染进程重启了，正在自动恢复（第 %1 次）…").arg(attempt), 5000);
    });

    // ---- 标签页 ----
    // 关标签前"要不要保存"的对话框由主窗口提供：TabManager 只负责"问一声、按答案决定关不关"。
    // 注意这里同时完成了会话回收 —— 函数返回到 TabManager 之后它就把标签页（和编辑器）删掉了，
    // 我们必须在那之前把会话表里的记录清干净，否则会留下悬空的键。
    ui->tabManager->setCloseConfirmHandler([this](EditorWidget *editor) {
        FileManager *files = filesFor(editor);
        if (!maybeSave(files)) {
            return false;  // 用户在保存提示里点了取消
        }
        removeSession(editor);
        return true;
    });

    // 切标签 → 换预览、换标题、换状态栏
    connect(ui->tabManager, &TabManager::currentEditorChanged, this, &MainWindow::onCurrentTabChanged);

    // ---- 文件树侧边栏（5.4.1，主窗口布局那一节改成停靠面板）----
    // 它现在是左侧的 QDockWidget（见 mainwindow.ui）：能拖到别的停靠区、也能关掉，
    // 视图菜单里有开关。工作台只管"编辑器侧 / 预览侧"两块，侧边栏不进中央分屏 ——
    // 三种显示模式切来切去都不影响它。
    ui->fileTreeDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable
                                  | QDockWidget::DockWidgetFloatable);
    // 注意：文件树的根目录**不在这里设**。设根目录会让 QFileSystemModel 立刻开始
    // 监听那个目录（大目录首次列目录是有开销的），所以它被排到了 finishStartup() ——
    // 那是"窗口已经显示出来之后"的第二阶段（7.2 启动优化）。

    // 中央分屏现在是"编辑器 + 预览"两块，初始各占一半
    ui->workbench->setSplitSizes({600, 600});

    // 帧统计（性能排查）：预览网页在滚动停手后报回它自己的帧数/最长帧间隔。
    // 这一条和"编辑区"那条配对：谁的数字差，问题就在谁那边。
    connect(ui->workbench->bridge(), &SyncBridge::previewFramesReported, this, [](int frames, int worstMs) {
        LOG_INFO("帧统计（预览网页）: %1 帧，最长帧间隔 %2 ms", frames, worstMs);
    });

    // 双击（或回车）一个文件 → 开成新标签。
    // 侧边栏本身不认识标签页，它只发"用户点了这个文件"，开到哪里是主窗口的事。
    connect(ui->fileTree, &FileTreeView::fileActivated, this, [this](const QString &path) { openFile(path); });

    // 右键菜单的新建/删除/重命名：文件操作侧边栏自己做完了（它不弹窗的那几个函数是公开的），
    // 这里只负责把结果表达出来 —— 状态栏说一句、日志留一条，必要时把侧边栏切到新文件那儿。
    connect(ui->fileTree, &FileTreeView::fileCreated, this, [this](const QString &path) {
        LOG_INFO("侧边栏新建文件：%1", path);
        statusBar()->showMessage(QStringLiteral("已新建：%1").arg(path));
        openFile(path);  // 新建出来通常就是马上要写东西，直接开成标签
    });
    connect(ui->fileTree, &FileTreeView::fileRemoved, this, [this](const QString &path) {
        LOG_INFO("侧边栏删除：%1", path);
        statusBar()->showMessage(QStringLiteral("已删除：%1").arg(path));
    });
    connect(ui->fileTree, &FileTreeView::fileRenamed, this, [this](const QString &oldPath, const QString &newPath) {
        LOG_INFO("侧边栏重命名：%1 → %2", oldPath, newPath);
        // 注意：如果被改名的文件正开在标签里，那个会话记的还是旧路径
        //（FileManager 不提供"改路径"操作，硬改会让脏标志/历史仓库错位）。
        // 所以这里只提示一句，让用户自己决定重新打开。
        statusBar()->showMessage(QStringLiteral("已重命名为：%1").arg(newPath));
    });
    connect(ui->fileTree, &FileTreeView::errorOccurred, this, [this](const QString &message) {
        // 侧边栏自己已经弹过窗了，这里只在状态栏留个痕
        statusBar()->showMessage(message, 5000);
    });

    // ---- 全文搜索面板（5.5）----
    // 面板自己不打开文件：它只发"用户点了这个文件的这一行"，打开和跳行是主窗口的事。
    // 这样面板既能脱离主窗口单独测，将来也能被别的地方复用（比如"在工作区里搜"）。
    connect(ui->searchPanel, &SearchPanel::resultActivated, this, &MainWindow::onSearchResultActivated);
    connect(ui->searchPanel, &SearchPanel::statusMessage, this, [this](const QString &text) {
        statusBar()->showMessage(text, 8000);
    });

    // 默认不占地方：菜单「视图 → 全文搜索」或 Ctrl+Shift+F 打开（和主流编辑器一致）
    ui->searchDock->hide();
    syncSearchDirectoryToSidebar();

    // ---- 大纲面板（C4）----
    // 和文件树**叠在同一个停靠区**（左侧），底部多一个页签切换"文件 / 大纲"。
    // 这样三栏布局（侧栏 + 编辑器 + 预览）一点没动 —— 多开一列就破了当时的设计稿约束。
    //
    // ⚠️ 三行的顺序不能换：tabifyDockWidget → raise() → hide()。
    //    raise() 决定的是"这一叠里默认显示哪一页"，hide() 决定"整叠默认收不收起来"。
    //    先 hide 再 raise 会得到"动作勾上了、面板却看不见"（raise 只切页签、不改可见性）。
    tabifyDockWidget(ui->fileTreeDock, ui->outlineDock);
    ui->fileTreeDock->raise();  // 默认停在"文件"那一页
    ui->outlineDock->hide();    // 和搜索面板一致：默认不占地方

    // 面板只发"用户点了这一行"；跳到哪、怎么跳是主窗口的事（和 SearchPanel 同一分工）。
    connect(ui->outlinePanel, &OutlinePanel::lineActivated, this, &MainWindow::onOutlineLineActivated);
    connect(ui->outlinePanel, &OutlinePanel::statusMessage, this, [this](const QString &text) {
        statusBar()->showMessage(text, 8000);
    });

    // 大纲刷新的第三条路径：打字。这里是**唯一的连接点**（不要挪到 connectSession 里 ——
    // 那会在每个标签上各连一份，标签一多就重复触发）。
    m_outlineTimer.setSingleShot(true);
    m_outlineTimer.setInterval(300);  // 打字停手 300ms 才扫一次全文
    connect(&m_outlineTimer, &QTimer::timeout, this, [this] {
        // ★ 现读 currentEditor()，**不要**在别处捕获具体 editor：
        //   定时器活得比一次切换久，捕获了就会把大纲刷成上一个文档的内容。
        refreshOutline(currentEditor());
    });
}

// 菜单/工具栏/动作：这些用 .ui 表达不了 ——
// 快捷键（QKeySequence::Open）、动作对象、triggered 连接都是 C++ 的事，
// 所以这一整块和"中央布局用不用 .ui"无关，永远是代码。
//
// 悬停提示（7.3）在这两处分别是：
//   * setStatusTip：鼠标停在**菜单项**上时，状态栏会显示这句话（QMainWindow 自带的行为）；
//   * setToolTip：鼠标停在**工具栏按钮**上时弹出的小黄条。
// 两者不是重复劳动：菜单项给出的是"这个命令做什么"，工具栏按钮还要顺带提示快捷键。
void MainWindow::initMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    m_newAction = fileMenu->addAction(QStringLiteral("新建标签(&N)"));
    m_newAction->setShortcut(QKeySequence::New);
    m_newAction->setStatusTip(QStringLiteral("新建一个标签（Ctrl+N）"));
    m_newAction->setToolTip(QStringLiteral("新建标签（Ctrl+N）"));
    connect(m_newAction, &QAction::triggered, this, &MainWindow::onNewFile);

    m_openAction = fileMenu->addAction(QStringLiteral("打开(&O)…"));
    m_openAction->setShortcut(QKeySequence::Open);
    m_openAction->setStatusTip(QStringLiteral("打开一个 Markdown 文件（Ctrl+O）"));
    m_openAction->setToolTip(QStringLiteral("打开文件（Ctrl+O）"));
    connect(m_openAction, &QAction::triggered, this, &MainWindow::onOpenFile);

    // ---- 文件树侧边栏（5.4.1）：选一个目录作为侧边栏的根 ----
    m_openFolderAction = fileMenu->addAction(QStringLiteral("打开文件夹(&K)…"));
    m_openFolderAction->setStatusTip(QStringLiteral("把左侧文件树切到某个文件夹"));
    connect(m_openFolderAction, &QAction::triggered, this, &MainWindow::onOpenFolder);

    // ---- 最近打开（5.4.2）----
    // 只建"壳"：条目由 rebuildRecentMenu() 按 m_recent 的内容重建。
    m_recentMenu = fileMenu->addMenu(QStringLiteral("最近打开(&R)"));

    m_saveAction = fileMenu->addAction(QStringLiteral("保存(&S)"));
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setStatusTip(QStringLiteral("保存当前标签（Ctrl+S）"));
    m_saveAction->setToolTip(QStringLiteral("保存（Ctrl+S）"));
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::onSaveFile);

    m_saveAsAction = fileMenu->addAction(QStringLiteral("另存为(&A)…"));
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    m_saveAsAction->setStatusTip(QStringLiteral("把当前文档存到别的位置（Ctrl+Shift+S）"));
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::onSaveFileAs);

    // ---- 导出（5.6）----
    // 做成子菜单：以后要加"导出为纯文本/Markdown"时不用再动文件菜单的结构。
    // 导出的是**编辑器里现在的内容**（不必先存盘），这一点见 exportCurrentDocument()。
    QMenu *exportMenu = fileMenu->addMenu(QStringLiteral("导出(&E)"));

    m_exportHtmlAction = exportMenu->addAction(QStringLiteral("导出为 HTML(&H)…"));
    connect(m_exportHtmlAction, &QAction::triggered, this, &MainWindow::onExportHtml);

    m_exportPdfAction = exportMenu->addAction(QStringLiteral("导出为 PDF(&P)…"));
    connect(m_exportPdfAction, &QAction::triggered, this, &MainWindow::onExportPdf);

    // ---- 版本历史（4.2.2 的轻量快照）----
    // 快照是保存时自动打的，这里只负责"看"：查历史列表、和上一版比差异
    fileMenu->addSeparator();

    m_historyAction = fileMenu->addAction(QStringLiteral("版本历史(&H)…"));
    connect(m_historyAction, &QAction::triggered, this, &MainWindow::onShowHistory);

    m_diffAction = fileMenu->addAction(QStringLiteral("与上一版对比(&D)…"));
    connect(m_diffAction, &QAction::triggered, this, &MainWindow::onDiffWithPrevious);

    m_rollbackAction = fileMenu->addAction(QStringLiteral("回滚到历史版本(&R)…"));
    connect(m_rollbackAction, &QAction::triggered, this, &MainWindow::onRollbackToVersion);

    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(QStringLiteral("退出(&Q)"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // 「编辑」菜单紧跟在「文件」后面（菜单顺序：文件 → 编辑 → 标签 → 视图 → 工具 → 帮助）。
    // 注意它是**单独一个函数**：撤销/重做…这些动作作用于"当前标签"，逻辑自成一块。
    initEditActions();

    // ---- 标签菜单（5.2）----
    QMenu *tabMenu = menuBar()->addMenu(QStringLiteral("标签(&B)"));

    m_closeTabAction = tabMenu->addAction(QStringLiteral("关闭当前标签(&W)"));
    m_closeTabAction->setShortcut(QKeySequence::Close);
    connect(m_closeTabAction, &QAction::triggered, this, &MainWindow::onCloseTab);

    // ---- 重开刚关掉的标签（C3）----
    //
    // Ctrl+Shift+T 是被"抢"过来的：它原来挂在工具栏的「暗色主题」上，主题挪到 Ctrl+Shift+D。
    // 理由：同一个 QKeySequence 装在两个 QAction 上，Qt 只会发 activatedAmbiguously，
    // **两个动作都不稳定**（有时这个生效、有时那个，还可能都不生效）；
    // 而"重开刚关掉的标签"是全平台（浏览器、编辑器）都这么用的手势，用户的手指是有肌肉记忆的，
    // 主题切换没有这种约定 —— 所以按"通用约定优先于历史选择"来取舍。
    // 这处取舍要同步到 README 与 docs/UI_DESIGN_SPEC.md 的快捷键表，否则文档就在骗人。
    m_reopenTabAction = tabMenu->addAction(QStringLiteral("重新打开关闭的标签(&R)"));
    m_reopenTabAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    m_reopenTabAction->setStatusTip(QStringLiteral("重开最近关掉的那个标签（Ctrl+Shift+T）"));
    m_reopenTabAction->setEnabled(false);  // 还没关过东西，没什么可重开的
    connect(m_reopenTabAction, &QAction::triggered, this, &MainWindow::onReopenClosedTab);
    // 可用性由栈的状态推着走，不在这里猜（见 TabManager::closedTabAvailabilityChanged）
    connect(ui->tabManager, &TabManager::closedTabAvailabilityChanged, m_reopenTabAction, &QAction::setEnabled);

    tabMenu->addSeparator();
    // Ctrl+Tab / Ctrl+Shift+Tab 是编辑器的通用习惯，QTabWidget 本身不带，这里补上
    m_nextTabAction = tabMenu->addAction(QStringLiteral("下一个标签(&N)"));
    m_nextTabAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab));
    connect(m_nextTabAction, &QAction::triggered, this, &MainWindow::onNextTab);

    m_previousTabAction = tabMenu->addAction(QStringLiteral("上一个标签(&P)"));
    m_previousTabAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab));
    connect(m_previousTabAction, &QAction::triggered, this, &MainWindow::onPreviousTab);

    // ---- 视图菜单（5.3 的三种显示模式）----
    // 用 QActionGroup 做成互斥：三个里同时只有一个被勾上，这就是"当前模式"的界面表达。
    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    m_viewModeGroup = new QActionGroup(this);
    m_viewModeGroup->setExclusive(true);

    auto addViewAction = [&](const QString &text, const QKeySequence &shortcut, EditorWorkbench::ViewMode mode) {
        QAction *action = viewMenu->addAction(text);
        action->setCheckable(true);
        action->setShortcut(shortcut);
        m_viewModeGroup->addAction(action);
        // 直接让工作台改模式；界面勾选状态由 viewModeChanged 信号统一刷新（见 onViewModeChanged）
        connect(action, &QAction::triggered, this, [this, mode] {
            ui->workbench->setViewMode(mode);
        });
        return action;
    };

    // Ctrl+1 / 2 / 3：和"分屏 / 仅编辑 / 仅预览"的顺序对应，好记
    m_viewSplitAction =
        addViewAction(QStringLiteral("左右分屏(&1)"), QKeySequence(Qt::CTRL | Qt::Key_1), EditorWorkbench::ViewMode::Split);
    m_viewEditorOnlyAction = addViewAction(QStringLiteral("仅编辑(&2)"),
                                           QKeySequence(Qt::CTRL | Qt::Key_2),
                                           EditorWorkbench::ViewMode::EditorOnly);
    m_viewPreviewOnlyAction = addViewAction(QStringLiteral("仅预览(&3)"),
                                            QKeySequence(Qt::CTRL | Qt::Key_3),
                                            EditorWorkbench::ViewMode::PreviewOnly);

    // ---- 文件树侧边栏开关（5.4.1 → 主窗口布局那一节改成停靠面板）----
    viewMenu->addSeparator();
    m_showFileTreeAction = viewMenu->addAction(QStringLiteral("文件树(&F)"));
    m_showFileTreeAction->setStatusTip(QStringLiteral("显示/隐藏左侧文件树面板"));
    m_showFileTreeAction->setCheckable(true);
    m_showFileTreeAction->setChecked(true);  // .ui 里默认就是可见的，勾选状态要和它一致
    connect(m_showFileTreeAction, &QAction::toggled, this, [this](bool visible) {
        // 只是隐藏，不销毁：QFileSystemModel 的目录监听和展开状态都留着，
        // 再打开时还是原来的样子。
        ui->fileTreeDock->setVisible(visible);
    });
    // 用户把停靠面板拖走/关掉时，菜单上的勾也要跟着变（两边状态不能不一致）。
    // 不会来回递归：setChecked 只在状态真的变了时才发 toggled。
    connect(ui->fileTreeDock, &QDockWidget::visibilityChanged, m_showFileTreeAction, &QAction::setChecked);

    // ---- 全文搜索面板（5.5）----
    // Ctrl+Shift+F 是"在文件里搜"的通用手势；面板做成停靠窗口，开关就是它的可见性。
    m_searchAction = viewMenu->addAction(QStringLiteral("全文搜索面板(&S)"));
    m_searchAction->setCheckable(true);
    m_searchAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    connect(m_searchAction, &QAction::toggled, this, [this](bool visible) {
        ui->searchDock->setVisible(visible);
        if (visible) {
            ui->searchDock->raise();
        }
    });
    // 用户点停靠窗口的 × 关掉时，菜单上的勾也要跟着取消（否则两边状态会不一致）。
    // 不会来回递归：setChecked 只有在状态真的变了时才发 toggled。
    connect(ui->searchDock, &QDockWidget::visibilityChanged, m_searchAction, &QAction::setChecked);

    // ---- 大纲面板（C4）----
    // 和文件树叠在同一个停靠区，所以这里不需要"关掉文件树"之类的联动 ——
    // 两个面板的可见性是同一个 QDockWidget 叠层里的两页，各自独立记着。
    m_outlineAction = viewMenu->addAction(QStringLiteral("大纲面板(&O)"));
    m_outlineAction->setCheckable(true);
    m_outlineAction->setStatusTip(QStringLiteral("显示/隐藏文档大纲面板"));
    connect(m_outlineAction, &QAction::toggled, this, [this](bool visible) {
        ui->outlineDock->setVisible(visible);
        if (visible) {
            ui->outlineDock->raise();  // 勾选 = 想看它，所以顺便把它这一页翻到前面来
        }
    });
    // 用户直接点页签切到大纲（或点 × 收起）时，菜单上的勾也要跟上，两边不能不一致。
    connect(ui->outlineDock, &QDockWidget::visibilityChanged, m_outlineAction, &QAction::setChecked);

    // ---- 焦点模式（C5）----
    // 只留当前段落、把别的行淡化。这是"写东西时不想被别的内容分散注意力"的诉求，
    // 所以它是个开关，不是模式切换 —— 勾上/取消随时可以。
    //
    // 状态由主窗口持有（m_focusMode），因为**焦点模式是每个编辑器各自的属性**：
    // 切标签时要把它补到新标签上（见 onCurrentTabChanged），否则会出现
    // "在 A 标签勾了，切到 B 标签发现没生效、切回来又还在"这种不一致。
    m_focusModeAction = viewMenu->addAction(QStringLiteral("焦点模式(&F)"));
    m_focusModeAction->setCheckable(true);
    m_focusModeAction->setStatusTip(QStringLiteral("只保留当前段落，其余行淡化"));
    connect(m_focusModeAction, &QAction::toggled, this, &MainWindow::onFocusModeToggled);

    // ---- 主题（5.7）----
    // 视图 → 主题 → 亮色 / 暗色。切换只调 ThemeManager：它负责 QSS + 调色板 + 落盘 + 发信号，
    // 本窗口和编辑器、预览区都只是"响应者"，不需要互相知道对方也要换色。
    QMenu *themeMenu = viewMenu->addMenu(QStringLiteral("主题(&T)"));
    m_themeGroup = new QActionGroup(this);
    m_themeGroup->setExclusive(true);

    m_themeLightAction = themeMenu->addAction(QStringLiteral("亮色(&L)"));
    m_themeLightAction->setStatusTip(QStringLiteral("切到亮色主题"));
    m_themeLightAction->setCheckable(true);
    m_themeGroup->addAction(m_themeLightAction);
    connect(m_themeLightAction, &QAction::triggered, this, [this] {
        Q_UNUSED(this);
        // 选内置主题 = 退出自定义主题（否则自定义配色会盖住亮色的编辑器配色）
        ThemeManager::instance().clearCustomTheme();
        ThemeManager::instance().setTheme(ThemeManager::Theme::Light);
    });

    m_themeDarkAction = themeMenu->addAction(QStringLiteral("暗色(&D)"));
    m_themeDarkAction->setStatusTip(QStringLiteral("切到暗色主题（Ctrl+Shift+D）"));
    m_themeDarkAction->setCheckable(true);
    m_themeGroup->addAction(m_themeDarkAction);
    connect(m_themeDarkAction, &QAction::triggered, this, [this] {
        Q_UNUSED(this);
        ThemeManager::instance().clearCustomTheme();
        ThemeManager::instance().setTheme(ThemeManager::Theme::Dark);
    });

    // ---- 主题导入导出（C7）----
    // 动态列出已导入的自定义主题 + 分隔线 + 导入/导出。
    // 选一个自定义主题 = 控件外观跟暗色、编辑器配色换成自定义那份（见 applyCustomTheme 的取舍说明）。
    themeMenu->addSeparator();
    const auto customThemes = ThemeManager::instance().customThemes();
    for (const auto &pair : customThemes) {
        QAction *custom = themeMenu->addAction(pair.first);
        const QString path = pair.second;
        connect(custom, &QAction::triggered, this, [this, path] {
            QString error;
            const auto palette = ThemeManager::paletteFromFile(path, &error);
            if (!error.isEmpty()) {
                statusBar()->showMessage(QStringLiteral("读主题失败：%1").arg(error), 5000);
                return;
            }
            ThemeManager::instance().applyCustomTheme(palette, ThemeManager::Theme::Dark);
        });
    }
    m_importThemeAction = themeMenu->addAction(QStringLiteral("导入主题…(&I)"));
    m_importThemeAction->setStatusTip(QStringLiteral("从 JSON 文件导入一套编辑器配色"));
    connect(m_importThemeAction, &QAction::triggered, this, &MainWindow::onImportTheme);
    m_exportThemeAction = themeMenu->addAction(QStringLiteral("导出当前主题…(&E)"));
    m_exportThemeAction->setStatusTip(QStringLiteral("把当前编辑器配色导出成 JSON 文件"));
    connect(m_exportThemeAction, &QAction::triggered, this, &MainWindow::onExportTheme);

    // ---- 工具菜单 ----
    QMenu *toolsMenu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    m_clearCacheAction = toolsMenu->addAction(QStringLiteral("清空内存缓存(&C)"));
    connect(m_clearCacheAction, &QAction::triggered, this, &MainWindow::onClearCache);

    // ---- 插入（5.7）----
    // 代码块的语言**由你选**：这里列出的就是高亮器认识的全部语言（30 多种）。
    // 也可以直接在文档里手写 ```python —— 两种方式等价，因为语言名是同一个词。
    toolsMenu->addSeparator();
    QAction *insertCodeAction = toolsMenu->addAction(QStringLiteral("插入代码块(&K)…"));
    connect(insertCodeAction, &QAction::triggered, this, &MainWindow::onInsertCodeBlock);

    // ---- 写作统计（C5）----
    // ⚠️ 刻意做成"点一下才算"：统计要遍历全文，而它每次按键都会变。放进状态栏实时刷
    //   就等于给打字加一条 O(全文) 的尾巴（还得再加一个定时器去节流）。
    //   状态栏继续用它已有的 O(1) 字符数；想看详细数字时点这一项。
    //   这样统计功能对打字延迟的影响是零，代价只是数字要手点一下才刷新。
    QAction *statsAction = toolsMenu->addAction(QStringLiteral("写作统计(&S)…"));
    statsAction->setStatusTip(QStringLiteral("字符 / 词 / 段 / 句 / 阅读时长（点击时才计算）"));
    connect(statsAction, &QAction::triggered, this, &MainWindow::onShowTextStats);

    // ---- 帮助菜单 ----
    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    m_aboutAction = helpMenu->addAction(QStringLiteral("关于(&A)…"));
    m_aboutAction->setStatusTip(QStringLiteral("关于这个程序"));
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::onAbout);

    m_aboutQtAction = helpMenu->addAction(QStringLiteral("关于 Qt(&Q)…"));
    connect(m_aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
}

// 编辑菜单：撤销/重做/剪切/复制/粘贴/全选 + 查找/替换。
//
// 这些动作都作用于**当前标签的编辑器**（不是"某个编辑器"），所以：
//   * triggered 里现取 currentEditor()，切标签不用重连；
//   * 可用状态跟着当前编辑器走（见 onCurrentTabChanged 与 connectSession 里的
//     undoAvailable / redoAvailable / copyAvailable）。
// 编辑器自己本来就处理 Ctrl+Z 这些快捷键；这里给动作设同样的键，是为了让
// 「菜单里显示的那套快捷键」和「按下去真的能用」是同一件事。
void MainWindow::initEditActions()
{
    QMenu *editMenu = menuBar()->addMenu(QStringLiteral("编辑(&E)"));

    const auto addEditorAction = [this, editMenu](const QString &text,
                                                  const QKeySequence &shortcut,
                                                  void (QPlainTextEdit::*slot)()) {
        QAction *action = editMenu->addAction(text);
        if (!shortcut.isEmpty()) {
            action->setShortcut(shortcut);
        }
        connect(action, &QAction::triggered, this, [this, slot] {
            if (EditorWidget *editor = currentEditor()) {
                (editor->*slot)();
            }
        });
        return action;
    };

    m_undoAction = addEditorAction(QStringLiteral("撤销(&U)"), QKeySequence::Undo, &QPlainTextEdit::undo);
    m_undoAction->setStatusTip(QStringLiteral("撤销上一步（Ctrl+Z）"));
    m_undoAction->setToolTip(QStringLiteral("撤销（Ctrl+Z）"));
    m_redoAction = addEditorAction(QStringLiteral("重做(&R)"), QKeySequence::Redo, &QPlainTextEdit::redo);
    // 重做在 Windows 上是 Ctrl+Y，在 macOS/Linux 上是 Ctrl+Shift+Z。这里两个都收：
    // 用户从别的编辑器过来时手会是习惯的那个。
    m_redoAction->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)});
    m_redoAction->setStatusTip(QStringLiteral("重做刚撤销的那一步（Ctrl+Y）"));
    m_redoAction->setToolTip(QStringLiteral("重做（Ctrl+Y）"));

    editMenu->addSeparator();
    m_cutAction = addEditorAction(QStringLiteral("剪切(&T)"), QKeySequence::Cut, &QPlainTextEdit::cut);
    m_cutAction->setStatusTip(QStringLiteral("剪切选中的内容（Ctrl+X）"));
    m_copyAction = addEditorAction(QStringLiteral("复制(&C)"), QKeySequence::Copy, &QPlainTextEdit::copy);
    m_copyAction->setStatusTip(QStringLiteral("复制选中的内容（Ctrl+C）"));
    m_pasteAction = addEditorAction(QStringLiteral("粘贴(&P)"), QKeySequence::Paste, &QPlainTextEdit::paste);
    m_pasteAction->setStatusTip(QStringLiteral("粘贴（Ctrl+V）"));

    editMenu->addSeparator();
    m_selectAllAction = addEditorAction(QStringLiteral("全选(&A)"), QKeySequence::SelectAll, &QPlainTextEdit::selectAll);
    m_selectAllAction->setStatusTip(QStringLiteral("选中当前文档的全部内容（Ctrl+A）"));

    // ---- 查找 / 替换 ----
    // 注意 Ctrl+F 在 Qt 里是 QKeySequence::Find；编辑器本身不处理它，所以不会冲突。
    editMenu->addSeparator();
    m_findAction = editMenu->addAction(QStringLiteral("查找(&F)…"));
    m_findAction->setShortcut(QKeySequence::Find);
    m_findAction->setStatusTip(QStringLiteral("在当前文档里查找（Ctrl+F）"));
    connect(m_findAction, &QAction::triggered, this, &MainWindow::onFind);

    m_replaceAction = editMenu->addAction(QStringLiteral("替换(&H)…"));
    m_replaceAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    m_replaceAction->setStatusTip(QStringLiteral("在当前文档里查找并替换（Ctrl+H）"));
    connect(m_replaceAction, &QAction::triggered, this, &MainWindow::onReplace);
}

void MainWindow::initToolBar()
{
    QToolBar *toolBar = addToolBar(QStringLiteral("主工具栏"));
    toolBar->setMovable(false);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);  // 没有图标资源，用文字当按钮

    // 常用文件操作
    toolBar->addAction(m_newAction);
    toolBar->addAction(m_openAction);
    toolBar->addAction(m_saveAction);
    toolBar->addSeparator();

    // 常用编辑操作（和编辑菜单共用同一批 QAction：状态、快捷键都是同一份）
    toolBar->addAction(m_undoAction);
    toolBar->addAction(m_redoAction);
    toolBar->addSeparator();

    // 两个"开关型"的视图按钮：一眼能看出当前状态（勾着 = 开着）
    m_togglePreviewAction = toolBar->addAction(QStringLiteral("预览"));
    m_togglePreviewAction->setCheckable(true);
    m_togglePreviewAction->setChecked(true);
    m_togglePreviewAction->setShortcut(QKeySequence(Qt::Key_F11));  // 6.2：切换预览 = F11
    m_togglePreviewAction->setToolTip(QStringLiteral("显示/隐藏预览区（F11，等价于 视图 → 仅编辑）"));
    connect(m_togglePreviewAction, &QAction::toggled, this, [this](bool shown) {
        // 勾着 = 左右分屏；取消 = 仅编辑。其它模式（仅预览）从视图菜单进，
        // 这里只做"预览这一块的开关"，语义保持简单。
        ui->workbench->setViewMode(shown ? EditorWorkbench::ViewMode::Split
                                        : EditorWorkbench::ViewMode::EditorOnly);
    });

    m_darkThemeAction = toolBar->addAction(QStringLiteral("暗色主题"));
    m_darkThemeAction->setCheckable(true);
    // 6.2 起这里是 Ctrl+Shift+T，C3 让给了「重开关闭的标签」—— 理由见 initMenuBar 里那段注释。
    m_darkThemeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
    m_darkThemeAction->setToolTip(QStringLiteral("在亮色 / 暗色主题之间切换（Ctrl+Shift+D）"));
    connect(m_darkThemeAction, &QAction::toggled, this, [this](bool dark) {
        ThemeManager::instance().setTheme(dark ? ThemeManager::Theme::Dark : ThemeManager::Theme::Light);
    });
}

void MainWindow::initStatusBar()
{
    statusBar()->showMessage(QStringLiteral("就绪"));

    // 状态栏的布局：左边是"临时消息区"（statusBar()->showMessage 用的那块），
    // 右边一排是常驻信息（addPermanentWidget，不会被临时消息顶掉）。
    // 顺序和"看的时候的眼睛路线"一致：光标 → 字符数 → 修改状态 → 路径 → 缓存。

    // 光标位置：由 EditorWidget::cursorMoved 推过来（行列都从 1 起算）
    m_cursorLabel = new QLabel(this);
    m_cursorLabel->setMinimumWidth(120);
    statusBar()->addPermanentWidget(m_cursorLabel);

    // 字符数：整篇文档的字符数（含空白），打字时实时变
    m_charCountLabel = new QLabel(this);
    m_charCountLabel->setMinimumWidth(110);
    statusBar()->addPermanentWidget(m_charCountLabel);

    // 修改状态：已修改 / 已保存。用 ● 和 ○ 一眼区分（文字也在，颜色之外还有形状）
    m_modifiedLabel = new QLabel(this);
    m_modifiedLabel->setMinimumWidth(90);
    statusBar()->addPermanentWidget(m_modifiedLabel);

    // 文件路径：长路径用"中间省略"显示（开头和结尾都看得见），完整路径进 tooltip，
    // 而且可以选中复制 —— 需要把路径贴到别处时很方便。
    m_pathLabel = new QLabel(this);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_pathLabel->setMinimumWidth(180);
    statusBar()->addPermanentWidget(m_pathLabel);

    // 最右侧常驻的缓存状态：这是"第二次打开同一个文件走了缓存"最直观的可见证据。
    m_cacheLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_cacheLabel);

    updateCacheStatus();
    updateDocumentStatus();
}

// 状态栏上"当前文档"那一组信息的统一刷新入口。
// 为什么集中在一起：切标签、打字、保存、光标移动都会影响其中一两项，
// 分散着更新迟早会漏一处（表现是"状态栏显示的还是上一个文件的信息"）。
void MainWindow::updateDocumentStatus()
{
    FileManager *files = currentFiles();
    EditorWidget *editor = currentEditor();

    if (files == nullptr || editor == nullptr) {
        if (m_charCountLabel != nullptr) {
            m_charCountLabel->setText(QString());
        }
        if (m_modifiedLabel != nullptr) {
            m_modifiedLabel->setText(QString());
        }
        if (m_pathLabel != nullptr) {
            m_pathLabel->setText(QString());
            m_pathLabel->setToolTip(QString());
        }
        return;
    }

    if (m_charCountLabel != nullptr) {
        // O(1)：别用 toPlainText().size()（那会把整篇文档拷一遍，而这里跟着每次按键跑）
        const int chars = editor->characterCount();
        const int lines = editor->document()->blockCount();
        m_charCountLabel->setText(QStringLiteral("字符 %1 · 行 %2").arg(chars).arg(lines));
    }

    if (m_modifiedLabel != nullptr) {
        const bool modified = isSessionModified(files, editor);
        m_modifiedLabel->setText(modified ? QStringLiteral("● 未保存") : QStringLiteral("○ 已保存"));
        m_modifiedLabel->setStyleSheet(modified ? QStringLiteral("color: #b8860b;") : QString());
        m_modifiedLabel->setToolTip(modified ? QStringLiteral("有未保存的修改（Ctrl+S 保存）")
                                             : QStringLiteral("内容已经保存到磁盘"));
    }

    if (m_pathLabel != nullptr) {
        const QString path = files->hasFilePath() ? QDir::toNativeSeparators(files->filePath())
                                                 : QStringLiteral("未命名（还没保存过）");
        // 中间省略：路径的开头（盘符/项目名）和结尾（文件名）都是有用信息
        m_pathLabel->setText(m_pathLabel->fontMetrics().elidedText(path, Qt::ElideMiddle, 360));
        m_pathLabel->setToolTip(path);
    }
}

// ============================ 会话（一个标签 = 一个文档）============================

EditorWidget *MainWindow::createSession()
{
    // FileManager 的父对象是主窗口：即使某个标签被关掉忘了回收，也不会泄漏到进程结束
    auto *files = new FileManager(this);
    EditorWidget *editor = ui->tabManager->addEditorTab();

    // 新标签也要跟上当前主题：EditorWidget 自己是从亮色起步的，
    // 开机时如果用户用的是暗色主题，这里得把它按当前主题刷一遍。
    // 用 currentPalette() 而不是 editorPalette(theme)：有自定义主题时要跟着用自定义配色。
    editor->setThemePalette(ThemeManager::instance().currentPalette());

    m_sessions.insert(editor, files);
    connectSession(editor, files);
    updateTabLabel(files);
    return editor;
}

void MainWindow::connectSession(EditorWidget *editor, FileManager *files)
{
    // 每个信号都用 lambda 把"是哪个编辑器/哪个文档"一起带上。
    // 这样槽函数不用去猜 sender()，也不怕将来加东西时连错对象。
    // 注意滚动条不用在这里连：那是"编辑器 → 预览"的同步，归工作台的 setCurrentEditor() 管，
    // 而且它会随当前标签切换，连在主窗口上会变成"所有标签一起抢预览"。
    connect(editor, &QPlainTextEdit::textChanged, this, [this, editor] { onEditorTextChanged(editor); });
    connect(editor, &EditorWidget::cursorMoved, this, [this, editor](int line, int column) {
        onCursorMoved(editor, line, column);
    });

    // 编辑菜单/工具栏那几个动作的可用状态：跟着**当前编辑器**的能力走。
    // 信号源是每个编辑器，槽里先判断"你是不是当前那个"，避免后台标签把菜单状态搅乱。
    const auto refreshEditActions = [this, editor] {
        if (editor != currentEditor()) {
            return;
        }
        if (m_undoAction != nullptr) {
            m_undoAction->setEnabled(editor->document()->isUndoAvailable());
        }
        if (m_redoAction != nullptr) {
            m_redoAction->setEnabled(editor->document()->isRedoAvailable());
        }
        const bool hasSelection = editor->textCursor().hasSelection();
        if (m_cutAction != nullptr) {
            m_cutAction->setEnabled(hasSelection && !editor->isReadOnly());
        }
        if (m_copyAction != nullptr) {
            m_copyAction->setEnabled(hasSelection);
        }
    };
    connect(editor, &QPlainTextEdit::undoAvailable, this, [refreshEditActions](bool) { refreshEditActions(); });
    connect(editor, &QPlainTextEdit::redoAvailable, this, [refreshEditActions](bool) { refreshEditActions(); });
    connect(editor, &QPlainTextEdit::copyAvailable, this, [refreshEditActions](bool) { refreshEditActions(); });
    connect(editor, &QPlainTextEdit::selectionChanged, this, [refreshEditActions] { refreshEditActions(); });

    // 帧统计（性能排查）：编辑区滚动停手之后报一句话，直接写进日志。
    // 和预览那侧的那份一起看，就知道"浏览掉帧"发生在哪一侧 —— 这是没法靠猜的：
    //   编辑区平均/最长间隔大 → Qt 侧重绘慢（QSS、行号栏、高亮…）
    //   预览帧率低、最长间隔大 → Chromium 合成慢（软件渲染、显卡驱动）
    connect(editor, &EditorWidget::framesReported, this, [](const QString &summary) {
        LOG_INFO("帧统计（编辑区）: %1", summary);
    });

    // 编辑器右键菜单里的两项：对话框/语言列表都在主窗口这边，编辑器只发"用户要这个"
    connect(editor, &EditorWidget::findRequested, this, &MainWindow::onFind);
    connect(editor, &EditorWidget::insertCodeBlockRequested, this, &MainWindow::onInsertCodeBlock);

    // 大文档快速模式（7.2）：进了就明确说一句，否则用户会以为"语法高亮坏了"。
    // 只说给当前标签听 —— 后台标签变大不该刷当前的状态栏。
    connect(editor, &EditorWidget::fastModeChanged, this, [this, editor](bool fast) {
        if (editor != currentEditor()) {
            return;
        }
        statusBar()->showMessage(fast ? QStringLiteral("文档很大（超过 %1 字符）：已关闭语法高亮、暂停预览，保证编辑流畅")
                                            .arg(EditorWidget::kFastModeThresholdChars)
                                      : QStringLiteral("文档变小了：语法高亮与预览已恢复"),
                                 10000);
    });

    connect(files, &FileManager::fileOpened, this, [this, files](const QString &path) {
        onFileOpened(files, path);
    });
    connect(files, &FileManager::fileSaved, this, [this, files](const QString &path) {
        onFileSaved(files, path);
    });
    connect(files, &FileManager::modificationChanged, this, [this, files](bool modified) {
        onModificationChanged(files, modified);
    });
    connect(files, &FileManager::readOnlyDetected, this,
            [this, files](const QString &path, const QString &reason) { onReadOnlyDetected(files, reason); });
}

EditorWidget *MainWindow::currentEditor() const
{
    return ui->tabManager->currentEditor();
}

// 注意这两个函数的**返回类型**必须写全限定名：
// C++ 在解析返回类型时还没进入 MainWindow 的作用域，所以类里那个
// FileManager 别名在这里是看不见的（参数类型在限定名之后，反而能用短名字）。
markdown_editor::core::storage::FileManager *MainWindow::filesFor(const EditorWidget *editor) const
{
    if (editor == nullptr) {
        return nullptr;
    }
    return m_sessions.value(const_cast<EditorWidget *>(editor), nullptr);
}

markdown_editor::core::storage::FileManager *MainWindow::currentFiles() const
{
    return filesFor(currentEditor());
}

EditorWidget *MainWindow::editorFor(const FileManager *files) const
{
    if (files == nullptr) {
        return nullptr;
    }
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it.value() == files) {
            return it.key();
        }
    }
    return nullptr;
}

void MainWindow::updateTabLabel(FileManager *files)
{
    EditorWidget *editor = editorFor(files);
    if (editor == nullptr) {
        return;
    }

    TabManager::TabInfo info;
    info.fileName = files->fileName();  // 没有路径时它会返回"未命名"
    info.filePath = files->filePath();
    // 用合成判断：同步还没跑（打字后 150ms 内）也要能看出"有未保存的修改"
    info.modified = isSessionModified(files, editor);
    ui->tabManager->updateTab(ui->tabManager->indexOf(editor), info);
}

// ============================ C2：每个标签的光标与滚动位置 ============================
//
// 要解决的问题：切回来永远是文档开头（showSession() 只推内容）。
//
// 两个纯搬运的辅助函数放在文件作用域，是为了让"离开时记"和"退出时记全部"用**同一段代码** ——
// 两份实现迟早会漂移（比如一处改成记 positionInBlock()、另一处还记着旧字段）。

namespace {

// 把编辑器当前的位置记进表里。path 为空（没保存过的新标签）就不记。
void recordEditorViewState(QHash<QString, QString> *state, EditorWidget *editor, const QString &path)
{
    if (state == nullptr || editor == nullptr || path.isEmpty()) {
        return;
    }

    // 行列都转成 1 起算 —— 和状态栏、goToLine、全文搜索全链路一致。
    // blockNumber() / positionInBlock() 是 0 起算的，转换只在这里和 onCursorMoved 发生。
    const QTextCursor cursor = editor->textCursor();
    SessionState::setViewState(state,
                               path,
                               cursor.blockNumber() + 1,
                               cursor.positionInBlock() + 1,
                               editor->verticalScrollBar()->value());
}

}  // namespace

void MainWindow::rememberViewState(EditorWidget *editor)
{
    if (editor == nullptr) {
        return;
    }
    const FileManager *files = filesFor(editor);
    if (files == nullptr || !files->hasFilePath()) {
        return;  // 没保存过的新标签：下次也开不出来，记了没意义（和 recentFiles 的取舍一致）
    }
    recordEditorViewState(&m_viewState, editor, files->filePath());
}

void MainWindow::applyViewState(EditorWidget *editor, const QString &path) const
{
    if (editor == nullptr || path.isEmpty()) {
        return;
    }

    const auto it = m_viewState.constFind(path);
    if (it == m_viewState.constEnd()) {
        return;
    }

    int line = 0;
    int column = 1;
    int scroll = 0;
    if (!SessionState::parseViewState(it.value(), &line, &column, &scroll)) {
        return;  // 存坏了 —— 当作"没记过"，退化成文档开头（不报错、不崩）
    }

    // ★ 顺序：先用 goToLine() 把光标放好，最后再设滚动条的值。
    //
    //   goToLine() 内部会 centerCursor()，也就是"把目标行滚到屏幕中间" ——
    //   如果反过来先设滚动值、再 goToLine()，那次居中会把刚恢复的滚动位置顶掉。
    //   放在最后，才是"光标在原来的行上、视口在原来的位置"。
    //
    //   不传 selectChars：恢复到上次的光标位置不是"找到了什么"，不该选中任何文字。
    if (line > 0) {
        editor->goToLine(line, qMax(1, column));
    }

    // 文档变短时这个值会被 QScrollBar 自己夹到合法范围，不用手动判。
    editor->verticalScrollBar()->setValue(scroll);
}

void MainWindow::showSession(FileManager *files, bool forceReload)
{
    if (files == nullptr) {
        return;
    }

    const QString dir = files->hasFilePath() ? QFileInfo(files->filePath()).absolutePath() : QString();

    // 布局、预览、同步全都交给工作台；主窗口只说"现在这个文档是什么、目录在哪"。
    // 目录没变的工作台内部只推内容、不重载页面（切标签不会闪白，也不会让 WebChannel 重连）。
    ui->workbench->showContent(files->text(), dir, forceReload);

    updateWindowTitle();
    updateCacheStatus();
}

void MainWindow::removeSession(EditorWidget *editor)
{
    FileManager *files = filesFor(editor);
    if (files == nullptr) {
        return;
    }
    // 先把它从"待同步"名单里摘掉：编辑器马上要被销毁了，
    // 定时器到点时不该再去碰它（QPointer 也能兜住，但这里是更明确的表达）。
    m_syncScheduler.forget(editor);
    m_sessions.remove(editor);
    delete files;  // 立刻回收：关掉的标签不该继续占着内容缓存和历史对象
}

// ============================ 文件与文档 ============================

bool MainWindow::openFile(const QString &path)
{
    // 已经打开过的文件不重复开标签，直接切过去（这是多标签编辑器该有的行为）
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it.value()->filePath() == path) {
            ui->tabManager->setCurrentIndex(ui->tabManager->indexOf(it.key()));
            m_recent.add(path);  // 它确实是"刚打开过"的，置个顶
            syncSidebarTo(path);
            return true;
        }
    }

    EditorWidget *editor = createSession();  // 新标签会成为当前标签
    FileManager *files = filesFor(editor);

    QString error;
    if (!files->openFile(path, &error)) {
        // 失败原因由管理器给出（打不开 / 是文件夹 / 没有权限…），主窗口只负责显示
        QMessageBox::warning(this, QStringLiteral("打开失败"), QStringLiteral("%1\n\n%2").arg(error, path));
        // 开不了就把这个空标签收回去，别在界面上留一个没用的"未命名"
        const int index = ui->tabManager->indexOf(editor);
        removeSession(editor);
        ui->tabManager->closeTab(index);
        return false;
    }

    // 编辑器显示文档内容。setPlainText 会触发 textChanged → onEditorTextChanged
    // → files->setText(同样的内容) → 内容没变，所以不会置脏 ✓
    editor->setPlainText(files->text());
    updateTabLabel(files);

    // 换文档了：baseUrl 必须跟着换（forceReload = true）
    showSession(files, true);

    // 打开成功才算"打开过"：失败的那条路径不该进最近列表（否则菜单里全是打不开的东西）。
    // 去重/置顶/截断都在 RecentFiles 里，这里只管交路径。
    m_recent.add(path);
    syncSidebarTo(path);
    return true;
}

void MainWindow::onNewFile()
{
    // 多标签之后"新建"就是开一个新标签 —— 不需要再问"当前文档要不要保存"了：
    // 当前文档不会被丢掉，那个提示属于"关闭标签"的场景。
    EditorWidget *editor = createSession();
    FileManager *files = filesFor(editor);

    editor->clear();
    updateTabLabel(files);
    showSession(files, true);  // 新文档没有路径 → baseUrl 回到 about:blank
}

void MainWindow::onOpenFile()
{
    FileManager *files = currentFiles();
    const QString startDir = (files == nullptr || !files->hasFilePath())
                                 ? QDir::homePath()
                                 : QFileInfo(files->filePath()).absolutePath();

    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("打开 Markdown 文件"),
        startDir,
        QStringLiteral("Markdown (*.md *.markdown *.txt);;所有文件 (*)"));
    if (!path.isEmpty()) {
        openFile(path);
    }
}

// 选一个目录当文件树的根（5.4.1）。
// 注意它**不等于**"打开一个工作区"：这里只是让侧边栏换个地方看文件，
// 不改变编辑器的状态，也不关任何标签。
void MainWindow::onOpenFolder()
{
    const QString startDir = ui->fileTree->rootPath().isEmpty() ? QDir::homePath() : ui->fileTree->rootPath();
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择要显示在侧边栏的文件夹"), startDir);
    if (dir.isEmpty()) {
        return;  // 用户取消了
    }

    ui->fileTree->setRootPath(dir);
    statusBar()->showMessage(QStringLiteral("文件树：%1").arg(QDir::toNativeSeparators(dir)));
    syncSearchDirectoryToSidebar();  // 搜索面板跟着走：看的是哪个目录，就索引哪个目录
}

// 侧边栏跟着当前文档走：根目录已经是这个文件的上级时不动，否则切到文件所在目录。
// 为什么要"已经在里面就不动"：用户可能特意把根设成了一个大目录、正在往下翻，
// 每打开一个文件都把根抢走会让人没法用。
void MainWindow::syncSidebarTo(const QString &filePath)
{
    if (filePath.isEmpty() || ui->fileTree == nullptr) {
        return;
    }

    const QString dir = QDir::cleanPath(QFileInfo(filePath).absolutePath());
    const QString root = QDir::cleanPath(ui->fileTree->rootPath());

    if (!root.isEmpty()) {
        // Windows 上路径大小写不敏感，所以要 CaseInsensitive 地比
        const bool same = (QString::compare(dir, root, Qt::CaseInsensitive) == 0);
        const bool inside = dir.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
        if (same || inside) {
            return;  // 已经看得到这个文件了
        }
    }

    ui->fileTree->setRootPath(dir);
    syncSearchDirectoryToSidebar();
}

// ============================ 最近打开（5.4.2）============================

// 按当前列表重建子菜单。条目最多 10 条，整块重建比增量更新简单、也不可能出现"菜单和列表不一致"。
// 三种条目：能打开的、已经不存在的（禁用 + 标注）、以及列表为空时的一句说明。
void MainWindow::rebuildRecentMenu()
{
    if (m_recentMenu == nullptr) {
        return;
    }

    m_recentMenu->clear();

    const QStringList files = m_recent.files();
    if (files.isEmpty()) {
        QAction *emptyAction = m_recentMenu->addAction(QStringLiteral("（还没有打开过文件）"));
        emptyAction->setEnabled(false);
        return;
    }

    for (const QString &path : files) {
        // 菜单里显示原生分隔符：Windows 用户看着 \ 更顺眼
        QAction *action = m_recentMenu->addAction(QDir::toNativeSeparators(path));
        action->setToolTip(path);

        if (!RecentFiles::fileExists(path)) {
            // 文件被移走/删掉了：留着这一条（用户可能只是临时拔了 U 盘），但标出来并且点不动。
            // 想彻底去掉它：先清空列表再重新打开，或者把文件放回去。
            action->setEnabled(false);
            action->setText(action->text() + QStringLiteral("（文件已不存在）"));
            continue;
        }

        connect(action, &QAction::triggered, this, [this, path] { openRecentFile(path); });
    }

    m_recentMenu->addSeparator();
    QAction *clearAction = m_recentMenu->addAction(QStringLiteral("清除最近文件(&C)"));
    connect(clearAction, &QAction::triggered, &m_recent, &RecentFiles::clear);
}

// 打开一条最近记录。文件不存在时：说一句、并把它从列表里摘掉
//（这就是 FileTreeView 那套"操作失败要说清原因"的同一种做法）。
bool MainWindow::openRecentFile(const QString &path)
{
    if (!RecentFiles::fileExists(path)) {
        QMessageBox::warning(this,
                             QStringLiteral("文件已不存在"),
                             QStringLiteral("这个文件找不到了：\n%1\n\n已经把它从「最近打开」里去掉。").arg(path));
        m_recent.remove(path);
        return false;
    }
    return openFile(path);
}

bool MainWindow::saveSession(FileManager *files, EditorWidget *editor)
{
    if (files == nullptr || editor == nullptr) {
        return false;
    }

    // 保证管理器里是最新内容：这里**显式**同步一次（不依赖节流器到点），
    // 然后把编辑器从"待同步"名单里摘掉 —— 内容已经在管理器里了，不必再同步一次。
    files->setText(editor->toPlainText());
    editor->document()->setModified(false);
    m_syncScheduler.forget(editor);

    if (!files->hasFilePath()) {
        return saveSessionAs(files, editor);  // 新文档还没有路径 → 走另存为
    }

    // 保存之前再查一次外部改动：磁盘上那份被别人改过的话，
    // 直接写下去就把别人的改动覆盖掉了 —— 那可能是人家几个小时的工作。
    if (!confirmOverwriteIfChanged(files)) {
        return false;
    }

    QString error;
    if (!files->saveFile(&error)) {
        // 失败原因由管理器给出（只读 / 没有写权限 / 文件被占用…）。
        // 注意：失败时脏标志仍然是 true，标签上的 * 不会被去掉。
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return false;
    }
    // 成功的话，标签/标题/状态栏由 fileSaved 信号更新
    return true;
}

bool MainWindow::saveSessionAs(FileManager *files, EditorWidget *editor)
{
    if (files == nullptr || editor == nullptr) {
        return false;
    }

    const QString startPath = files->hasFilePath() ? files->filePath() : QDir::homePath();
    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("另存为"),
                                                      startPath,
                                                      QStringLiteral("Markdown (*.md);;所有文件 (*)"));
    if (path.isEmpty()) {
        return false;  // 用户取消了
    }

    files->setText(editor->toPlainText());
    editor->document()->setModified(false);
    m_syncScheduler.forget(editor);  // 已经同步过了，别再让节流器做一遍

    QString error;
    if (!files->saveFileAs(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return false;
    }

    updateTabLabel(files);
    // 换目录了：如果这正是当前标签，预览的 baseUrl 也要跟着换
    if (files == currentFiles()) {
        showSession(files, true);
    }

    // 另存为也算"用过这个文件"：下次在「最近打开」里能找到它
    m_recent.add(path);
    syncSidebarTo(path);
    return true;
}

void MainWindow::onSaveFile()
{
    saveSession(currentFiles(), currentEditor());
}

void MainWindow::onSaveFileAs()
{
    saveSessionAs(currentFiles(), currentEditor());
}

// ============================ 编辑器 → 预览 ============================

void MainWindow::onEditorTextChanged(EditorWidget *editor)
{
    FileManager *files = filesFor(editor);
    if (files == nullptr) {
        return;
    }

    // ★ 性能（P0-1）：这里**不再**做 files->setText(editor->toPlainText())。
    // toPlainText() 会把整篇文档深拷贝一遍、setText 还要跟旧内容全串比较一遍，
    // 而这段代码在**每次按键**都会跑 —— 几十万字符的文档上，打字就是被这笔钱拖慢的。
    // 现在只标记"这个编辑器脏了"（O(1)），交给节流器在停手后统一同步一次；
    // 保存/关闭/切标签/导出之前会 flush 一次，所以不会出现"保存到旧内容"。
    m_syncScheduler.markDirty(editor);

    // 标签上的 * 要**立刻**出现：用编辑器自己的 modified 标志判断（O(1)），
    // 不等那次延迟同步（见 isSessionModified 的说明）。
    updateTabLabel(files);

    // 预览的推送不在这里做 —— 它跟着"同步完成"走（见 syncEditorIntoFiles）：
    // 那时候文档管理器里的内容才是最新的，而且渲染管线自己还有 300ms 防抖。
    updateWindowTitle();
    updateDocumentStatus();  // 字符数 / 行数 / 修改状态都是随打字变的

    // 大纲（C4）：打字不能每敲一个字就扫全文，所以只把这个 300ms 的防抖定时器重启一下。
    // 放在这里而不是在 connectSession 里另连一次 textChanged：那样每个标签都会多一份连接，
    // 而且"这是不是当前标签"的判断在这里已经做过了，不用再写一遍。
    m_outlineTimer.start();
}

// 编辑器内容 → 文档管理器（由节流器在停手之后调用；保存前也会被显式调用）。
void MainWindow::syncEditorIntoFiles(EditorWidget *editor)
{
    FileManager *files = filesFor(editor);
    if (files == nullptr || editor == nullptr) {
        return;
    }

    // 内容真的变了才继续往下走：setText() 在"内容没变"时返回 false
    //（被撤销回原样、或者打开文件时 setPlainText 带来的那一次 textChanged）
    if (!files->setText(editor->toPlainText())) {
        // 内容没变：编辑器那边的"待同步"标记清掉就行
        editor->document()->setModified(false);
        return;
    }

    // 同步完了：编辑器的 modified 标记复位，之后的"是否未保存"以文档管理器为准
    editor->document()->setModified(false);

    // 只有当前标签的改动才推到预览：别的标签改内容（比如程序自己填充）不该抢走预览
    if (editor == currentEditor()) {
        // 防抖（300ms）在渲染管线里：敲字时它会把这次更新一直往后推，
        // 停下来之后才真正渲染一次。渲染管线归工作台所有，这里只是借来用。
        ui->workbench->renderer()->updateContent(files->text());
    }

    updateTabLabel(files);
    updateWindowTitle();
    updateDocumentStatus();
}

bool MainWindow::isSessionModified(FileManager *files, EditorWidget *editor) const
{
    if (files != nullptr && files->isModified()) {
        return true;
    }
    // 编辑器自己说改过 = "还没被同步进文档管理器的那部分改动"
    return editor != nullptr && editor->document() != nullptr && editor->document()->isModified();
}

// ============================ 标签切换 ============================

void MainWindow::onCurrentTabChanged(EditorWidget *editor)
{
    // ★ 切标签之前先同步：离开的这个标签之后可能被保存/关闭，
    // 那时文档管理器里必须是它的最新内容（否则会写盘一个旧版本）。
    m_syncScheduler.flushAll();

    // ★ C2：趁"离开的那个标签"还活着、还显示着，把它的光标与滚动位置记下来。
    //   必须在下面任何切换动作之前调 —— showSession() 推内容、
    //   setCurrentEditor() 接滚动条，都会把"它原来在哪"抹掉。
    rememberViewState(m_lastEditor);

    if (editor == nullptr) {
        // 所有标签都被关掉了：立刻补一个干净的新标签。
        // 这样"界面上永远有一个编辑器"这条不变式一直成立，后面所有代码都能少写判空。
        createSession();
        return;
    }

    FileManager *files = filesFor(editor);
    showSession(files, false);  // 同目录时只推内容，不重载页面（不闪白）

    // ★ C2：把"这个标签上次看到哪"摆回去。
    //   必须在 showSession() **之后** —— showSession() 会推内容进去，
    //   而推内容（setPlainText）会把光标重置到文档开头。
    //   顺序反了的话，恢复出来的位置立刻又被顶掉，表现就是"记忆没生效"。
    if (files != nullptr && files->hasFilePath()) {
        applyViewState(editor, files->filePath());
    }

    // 告诉工作台"现在编辑的是这个编辑器"：它会接上这个编辑器的滚动条（并断开上一个），
    // 顺便把预览滚到它的当前顶行 —— 这两件事原来散在主窗口里。
    ui->workbench->setCurrentEditor(editor);

    // 状态栏的行列要换成这个标签的光标位置
    const QTextCursor cursor = editor->textCursor();
    onCursorMoved(editor, cursor.blockNumber() + 1, cursor.positionInBlock() + 1);

    // 状态栏那一组"当前文档"信息（字符数/路径/修改状态）也要换成这个标签的
    updateDocumentStatus();

    // 大纲（C4）：切标签必须**立刻**换，不等那个 300ms 防抖 ——
    // 切过去还看着上一个文档的大纲，比没有大纲更糟（会点着它跳到错的文档里）。
    refreshOutline(editor);

    // 焦点模式（C5）：把"用户要的开关状态"补到这个编辑器上。
    // 不补的话，新标签会以"关"的状态显示，和菜单上的勾不一致。
    editor->setFocusMode(m_focusMode);

    // 切到这个标签时顺手查一下：这个文件有没有被别的程序改过（7.3）。
    // 放在这里是因为"用户刚把这个文档调到眼前"，此时提示最合时宜。
    checkCurrentExternalChange();

    // 查找对话框跟着当前标签走：它只认一个编辑器，切标签不重新绑就会"对着旧文件替换"
    if (m_findDialog.isVisible()) {
        m_findDialog.setEditor(editor);
    }

    // 编辑菜单里那些动作的可用状态也要按新编辑器刷新一次
    if (m_undoAction != nullptr) {
        m_undoAction->setEnabled(editor->document()->isUndoAvailable());
    }
    if (m_redoAction != nullptr) {
        m_redoAction->setEnabled(editor->document()->isRedoAvailable());
    }
    const bool hasSelection = editor->textCursor().hasSelection();
    if (m_cutAction != nullptr) {
        m_cutAction->setEnabled(hasSelection && !editor->isReadOnly());
    }
    if (m_copyAction != nullptr) {
        m_copyAction->setEnabled(hasSelection);
    }

    // ★ C2：记下"现在显示的是谁"，下次切走时才能记住它看到哪（见函数开头的 rememberViewState）。
    //   放在最后：这个函数中间可能提前 return（比如没有标签时去 createSession()），
    //   那些分支会通过嵌套的 currentChanged 自己走到这里，不该在这里抢答。
    m_lastEditor = editor;
}

// 预览里被点了一下。光标是工作台跳的（它知道当前编辑器是谁），主窗口只负责界面表达。
void MainWindow::onEditorLineClicked(int line)
{
    LOG_INFO("预览点击 → 编辑器跳到第 %1 行", line);
    statusBar()->showMessage(QStringLiteral("已跳到第 %1 行").arg(line));
}

// ============================ 显示模式（5.3）============================

void MainWindow::onViewModeChanged(EditorWorkbench::ViewMode mode)
{
    if (m_viewSplitAction != nullptr) {
        m_viewSplitAction->setChecked(mode == EditorWorkbench::ViewMode::Split);
    }
    if (m_viewEditorOnlyAction != nullptr) {
        m_viewEditorOnlyAction->setChecked(mode == EditorWorkbench::ViewMode::EditorOnly);
    }
    if (m_viewPreviewOnlyAction != nullptr) {
        m_viewPreviewOnlyAction->setChecked(mode == EditorWorkbench::ViewMode::PreviewOnly);
    }
}

void MainWindow::onCloseTab()
{
    ui->tabManager->requestCloseTab(ui->tabManager->currentIndex());
}

void MainWindow::onReopenClosedTab()
{
    const TabManager::ClosedTab closed = ui->tabManager->takeLastClosedTab();
    if (closed.filePath.isEmpty()) {
        return;  // 栈空了。正常情况下动作已经是灰的，这里只是兜底
    }

    // 时间点写进提示语：连着关了几个标签时，用户按 Ctrl+Shift+T 得知道
    // "刚回来的是哪一个"，不然只能从标签名去猜。
    const QString when = closed.closedAt.toString(QStringLiteral("HH:mm:ss"));

    if (!openFile(closed.filePath)) {
        // openFile 内部已经弹过「打开失败」并说明了原因（被删了 / 没权限 / 是个目录）。
        // 这里补一句"它是从最近关闭里来的"，免得用户以为是点错了什么菜单。
        // 记录照旧消耗掉：那条路径已经开不出来了，留着只会让下一次重开再失败一次。
        statusBar()->showMessage(QStringLiteral("%1 打不开了（它在 %2 被关闭，之后可能被移动或删除）")
                                     .arg(closed.displayName, when));
        return;
    }

    QString message = QStringLiteral("已重新打开 %1（它在 %2 被关闭）").arg(closed.displayName, when);
    if (closed.hadUnsavedChanges) {
        // 关的时候用户选了"不保存"。重开拿到的是磁盘上的那一版，
        // 说清楚这一点，别让人以为关掉的那几行还能回来。
        message += QStringLiteral("——注意：关闭时未保存的改动不会随它回来");
    }
    statusBar()->showMessage(message);
}

void MainWindow::onNextTab()
{
    const int count = ui->tabManager->count();
    if (count <= 1) {
        return;
    }
    ui->tabManager->setCurrentIndex((ui->tabManager->currentIndex() + 1) % count);
}

void MainWindow::onPreviousTab()
{
    const int count = ui->tabManager->count();
    if (count <= 1) {
        return;
    }
    ui->tabManager->setCurrentIndex((ui->tabManager->currentIndex() + count - 1) % count);
}

// ============================ 文件管理器的结论 → 界面 ============================
//
// FileManager 不弹任何对话框（那样它就没法在无窗口的环境里跑了），
// 只把结论和原因发出来；弹窗、状态栏、标题栏这些"界面表达"全在这里。

void MainWindow::onFileOpened(FileManager *files, const QString &path)
{
    if (files == currentFiles()) {
        statusBar()->showMessage(
            QStringLiteral("已打开：%1（%2）").arg(path, FileManager::encodingName(files->encoding())));
    }
    updateTabLabel(files);
    if (files == currentFiles()) {
        updateWindowTitle();
        updateCacheStatus();  // 命中/未命中次数刚刚变了
        updateDocumentStatus();  // 路径 / 字符数 / 修改状态
        refreshOutline(currentEditor());  // C4：换了文档，大纲要跟着换（不等防抖）
    }
}

void MainWindow::onFileSaved(FileManager *files, const QString &path)
{
    updateTabLabel(files);  // 标签上的 * 消失
    if (files == currentFiles()) {
        statusBar()->showMessage(
            QStringLiteral("已保存：%1（%2）").arg(path, FileManager::encodingName(files->encoding())));
        updateWindowTitle();
        updateCacheStatus();  // 保存后缓存里换成了新内容
        updateDocumentStatus();  // 修改状态回到"已保存"，路径也可能刚变（另存为）
        // C4：另存为会换路径，大纲本身没变，但"这个面板说的是哪个文档"要跟上；
        // 保存也顺手刷一次，代价只有一次 O(行数) 扫描（正文没变的话结果一模一样）。
        refreshOutline(currentEditor());
    }
}

// ============================ 大纲（C4）============================

void MainWindow::refreshOutline(EditorWidget *editor)
{
    if (editor == nullptr) {
        ui->outlinePanel->clear();
        return;
    }

    // ★ 大文档保护：超过快速模式阈值就不扫了。
    //   这不是偷懒 —— EditorWidget 在这个阈值上关语法高亮、预览停止渲染，同一个约束
    //   （"大文档不能全量处理"）在这里是第三处生效。不拦的话，用户停手 300ms 后要等
    //   好几秒才有反应，比没有大纲更难受。
    //   characterCount() 是 O(1)；真正贵的 toPlainText() 只在确认要扫时才调。
    if (editor->characterCount() > EditorWidget::kFastModeThresholdChars) {
        ui->outlinePanel->setPaused(true);
        return;
    }

    ui->outlinePanel->setSource(editor->toPlainText());
}

void MainWindow::onOutlineLineActivated(int line)
{
    EditorWidget *editor = currentEditor();
    if (editor == nullptr) {
        return;  // 没有标签时不该有信号，防御一下
    }

    // 跳行只有一份实现（EditorWidget::goToLine：夹范围、居中、拿焦点都在里面）。
    // 预览点击（SyncBridge）、全文搜索（onSearchResultActivated）、大纲 —— 三个调用方
    // 共用同一个入口，所以"跳到第几行"的行为永远一致。
    editor->goToLine(line);
}

// ============================ 焦点模式与写作统计（C5）============================

void MainWindow::onFocusModeToggled(bool on)
{
    m_focusMode = on;  // 记住"用户要的是这样"，切标签时补到新标签上

    if (EditorWidget *editor = currentEditor()) {
        editor->setFocusMode(on);
    }
    statusBar()->showMessage(on ? QStringLiteral("焦点模式：只保留当前段落")
                                : QStringLiteral("焦点模式已关闭"),
                             5000);
}

void MainWindow::onShowTextStats()
{
    EditorWidget *editor = currentEditor();
    if (editor == nullptr) {
        statusBar()->showMessage(QStringLiteral("没有打开的文档"), 5000);
        return;
    }

    // 只在用户点了这一下的时候遍历全文（见 initMenuBar 里那段注释：
    // 放进状态栏实时算就等于给每次按键加一条 O(全文) 的尾巴）。
    const TextStats stats = TextStats::compute(editor->toPlainText());

    QMessageBox::information(this, QStringLiteral("写作统计"), TextStats::detail(stats));
    statusBar()->showMessage(TextStats::format(stats), 8000);
}

// ============================ 主题导入导出（C7）============================

void MainWindow::onImportTheme()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入主题"), QString(),
        QStringLiteral("主题 JSON (*.json);;所有文件 (*)"));

    if (path.isEmpty()) {
        return;  // 用户取消了
    }

    QString error;
    if (!ThemeManager::instance().importTheme(path, QString(), &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }

    // 导入成功 → 立刻应用（否则用户会困惑"导入了但没变化"）。
    const auto palette = ThemeManager::paletteFromFile(ThemeManager::instance().importedThemePath());
    ThemeManager::instance().applyCustomTheme(palette, ThemeManager::Theme::Dark);

    QMessageBox::information(
        this, QStringLiteral("导入成功"),
        QStringLiteral("主题已导入并应用。\n\n编辑器的配色换成了新主题，"
                       "菜单/工具栏等控件外观仍跟随「暗色」。\n"
                       "想改回内置主题，在「主题」菜单里选亮色或暗色即可。"));
}

void MainWindow::onExportTheme()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出当前主题"), QStringLiteral("muse-theme.json"),
        QStringLiteral("主题 JSON (*.json)"));

    if (path.isEmpty()) {
        return;
    }

    QString error;
    if (!ThemeManager::instance().exportTheme(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }

    statusBar()->showMessage(QStringLiteral("主题已导出到 %1").arg(path), 8000);
}

void MainWindow::onModificationChanged(FileManager *files, bool modified)
{
    // 标签上的 * 和标题栏的 * 都靠这里。
    // 注意：保存成功时先发 modificationChanged(false)、再发 fileSaved(path)，
    // 所以最后停在状态栏上的是"已保存"这条。
    updateTabLabel(files);
    if (files == currentFiles()) {
        updateWindowTitle();
        statusBar()->showMessage(modified ? QStringLiteral("有未保存的修改")
                                          : QStringLiteral("已保存到磁盘"));
        updateDocumentStatus();  // 状态栏那个 ● 未保存 / ○ 已保存
    }
}

void MainWindow::onReadOnlyDetected(FileManager *files, const QString &reason)
{
    Q_UNUSED(files);
    QMessageBox::warning(this, QStringLiteral("文件是只读的"), reason);
}

// ============================ 外部修改提示（7.3）============================
//
// 场景：编辑器里开着 a.md，你用别的程序改了 a.md（或 git 切换了分支）。
// 如果什么都不管，用户接着按 Ctrl+S 就会把别人的改动覆盖掉 —— 那是数据丢失。
// 所以这里做两件事：
//   1. 切标签、以及窗口重新获得焦点时，检查当前文档有没有被外部改过 → 问"重载 / 保留我的"；
//   2. 保存之前再查一次 → 问"要不要覆盖磁盘上那份"。
//
// 机制在 FileManager 里（hasExternalChange / externalChangeReason / reloadFromDisk /
// acceptCurrentDiskState），"问不问、怎么问"在界面层 —— 和这个项目其他地方的分工一致。

void MainWindow::promptExternalChange(FileManager *files)
{
    if (files == nullptr || !files->hasExternalChange()) {
        return;
    }

    const QString reason = files->externalChangeReason();
    const bool dirty = files->isModified();

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("文件已被外部修改"));
    box.setIcon(QMessageBox::Warning);
    box.setText(reason);
    box.setInformativeText(dirty ? QStringLiteral("你这个标签里还有未保存的修改。\n"
                                                  "重载会用磁盘上的版本替换它们（不可撤销）。")
                                 : QStringLiteral("要用磁盘上的版本重新载入吗？"));

    QPushButton *reloadButton = box.addButton(QStringLiteral("重载"), QMessageBox::AcceptRole);
    QPushButton *keepButton = box.addButton(dirty ? QStringLiteral("保留我的修改") : QStringLiteral("先不管"),
                                           QMessageBox::RejectRole);
    box.setDefaultButton(dirty ? keepButton : reloadButton);
    box.exec();

    if (box.clickedButton() == reloadButton) {
        QString error;
        if (!files->reloadFromDisk(&error)) {
            QMessageBox::warning(this, QStringLiteral("重载失败"), error);
            return;
        }
        if (EditorWidget *editor = editorFor(files)) {
            // 先把内容灌回编辑器（setPlainText 会触发 textChanged → FileManager::setText，
            // 所以要在重载之后再同步一次脏标志）
            const QSignalBlocker blocker(editor);
            editor->setPlainText(files->text());
        }
        files->setModified(false);
        updateTabLabel(files);
        if (files == currentFiles()) {
            showSession(files, true);
            updateDocumentStatus();
            updateWindowTitle();
        }
        statusBar()->showMessage(QStringLiteral("已从磁盘重载：%1").arg(files->fileName()), 5000);
        LOG_INFO("用户选择重载外部改动: %1", files->filePath());
        return;
    }

    // 用户选择"保留我的"：把当前磁盘状态记成基准，别在每次切标签/切窗口时再问一遍
    files->acceptCurrentDiskState();
    statusBar()->showMessage(QStringLiteral("保留了编辑器里的内容（磁盘上的改动先不管）"), 5000);
}

void MainWindow::checkCurrentExternalChange()
{
    // 只查当前标签：一启动就把所有标签问一遍太吵，而且用户此刻也看不到别的标签
    promptExternalChange(currentFiles());
}

bool MainWindow::confirmOverwriteIfChanged(FileManager *files)
{
    if (files == nullptr || !files->hasExternalChange()) {
        return true;  // 没变化：不用问
    }

    const QMessageBox::StandardButton answer =
        QMessageBox::warning(this,
                             QStringLiteral("磁盘上的文件已被改动"),
                             files->externalChangeReason() + QStringLiteral("\n\n仍要保存并覆盖它吗？"),
                             QMessageBox::Save | QMessageBox::Cancel,
                             QMessageBox::Cancel);
    if (answer != QMessageBox::Save) {
        statusBar()->showMessage(QStringLiteral("已取消保存（磁盘上的文件被别的程序改过了）"), 8000);
        return false;
    }
    return true;
}

// 有未保存的修改时先问一句。返回 false = 用户取消，调用方必须中止当前操作。
bool MainWindow::maybeSave(FileManager *files)
{
    // ★ 先把"欠着的"编辑器内容同步进文档管理器，再问"有没有未保存的修改"。
    // 少了这一步，刚打完最后一个字就点关闭时，文档管理器还停留在 150ms 前的版本 ——
    // 用户会看到"明明改了却不提示保存"，那是丢数据的 bug。
    m_syncScheduler.flushAll();

    if (files == nullptr || !files->isModified()) {
        return true;
    }

    const QMessageBox::StandardButton answer =
        QMessageBox::warning(this,
                             QStringLiteral("有未保存的修改"),
                             QStringLiteral("「%1」有未保存的修改，要先保存吗？").arg(files->fileName()),
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                             QMessageBox::Save);

    if (answer == QMessageBox::Cancel) {
        return false;
    }
    if (answer == QMessageBox::Discard) {
        return true;
    }

    saveSession(files, editorFor(files));  // 新文档没有路径时，它自己会转去「另存为」
    return !files->isModified();           // 保存失败、或用户取消了另存为 → 别继续往下丢内容
}

// ============================ 版本历史（4.2.2）============================
//
// 快照在保存时由 FileManager 自动打（见 FileManager::snapshotAfterSave），
// 这里只做两件"看"的事：列历史、比差异。服务本身不弹窗，弹什么、怎么排版是界面的事。

// 一个只读文本窗口：历史列表和差异都用它显示。
// 为什么不直接用 QMessageBox：差异可能几百行，需要等宽字体、不自动折行、可选可复制、好滚动。
void MainWindow::showTextDialog(const QString &title, const QString &header, const QString &body)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(900, 600);

    auto *layout = new QVBoxLayout(&dialog);

    auto *headerLabel = new QLabel(header, &dialog);
    headerLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);  // 仓库路径要能复制出来
    headerLabel->setWordWrap(true);
    layout->addWidget(headerLabel);

    auto *view = new QPlainTextEdit(&dialog);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setPlainText(body);
    layout->addWidget(view, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.exec();
}

void MainWindow::onShowHistory()
{
    FileManager *files = currentFiles();
    if (files == nullptr || !files->hasFilePath()) {
        QMessageBox::information(this,
                                 QStringLiteral("版本历史"),
                                 QStringLiteral("这个文档还没保存过。\n保存一次（Ctrl+S）就会留下第一份快照。"));
        return;
    }

    VersionControl *history = files->versionControl();
    const QString repoDir = history->repositoryPathFor(files->filePath());

    QString error;
    const QList<VersionControl::Commit> commits = history->history(repoDir, 50, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("版本历史"), error);
        return;
    }

    if (commits.isEmpty()) {
        showTextDialog(
            QStringLiteral("版本历史"),
            QStringLiteral("还没有快照。按 Ctrl+S 保存一次就会有第一份。\n快照仓库：%1").arg(repoDir),
            QStringLiteral("（每次保存都会留下一条，最新的显示在最上面）"));
        return;
    }

    QStringList lines;
    for (const VersionControl::Commit &commit : commits) {
        lines << QStringLiteral("%1  %2  %3")
                     .arg(commit.shortHash,
                          commit.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                          commit.message);
    }

    showTextDialog(QStringLiteral("版本历史 — %1").arg(files->fileName()),
                   QStringLiteral("共 %1 个快照（最新的在最上面）\n快照仓库：%2\n"
                                  "想用命令行看：cd 进上面这个目录，然后 git log / git diff")
                       .arg(commits.size())
                       .arg(repoDir),
                   lines.join(QLatin1Char('\n')));
}

void MainWindow::onDiffWithPrevious()
{
    FileManager *files = currentFiles();
    if (files == nullptr || !files->hasFilePath()) {
        QMessageBox::information(this,
                                 QStringLiteral("与上一版对比"),
                                 QStringLiteral("这个文档还没保存过，没有可对比的版本。"));
        return;
    }

    VersionControl *history = files->versionControl();
    const QString repoDir = history->repositoryPathFor(files->filePath());

    QString error;
    const QList<VersionControl::Commit> commits = history->history(repoDir, 2, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("与上一版对比"), error);
        return;
    }
    if (commits.size() < 2) {
        QMessageBox::information(
            this,
            QStringLiteral("与上一版对比"),
            QStringLiteral("目前只有 %1 个快照，还没有可对比的上一版。\n改点内容再保存一次就有了。")
                .arg(commits.size()));
        return;
    }

    const VersionControl::Commit newest = commits.at(0);
    const VersionControl::Commit previous = commits.at(1);

    // ★ B1：改走**自研行级 diff**（本地算法 + 结构化结果），不再调 `git diff`。
    //
    // 为什么换：我们自己的 LineDiff 给出的 hunks 是结构化的（每块的行号、增删各几行），
    // 未来要做"带高亮的历史对比视图 / 点一行跳过去"时，直接就能用；
    // 而 `git diff` 只给一段文本，想拿结构还得把它解析回来。
    // 另外它不再依赖 git 的输出格式（git 版本 / 语言环境变化都不影响）。
    //
    // 语义等价：commits 是"最新在前"的线性历史，所以第 2 条正好是新版的父提交，
    // diffWithParentLocal(newest) 得到的区间和原来 diff(previous, newest) 完全一致。
    //
    // `diffWithParent()`（git 那条路）**保留不动** —— tests/test_versioncontrol.cpp
    // 还在用它对拍，两条路互相印证比只留一条更可靠。
    const LineDiff::Result localDiff = history->diffWithParentLocal(repoDir, newest.hash, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("与上一版对比"), error);
        return;
    }

    // 界面暂时还是"显示一段文本"，所以在这里把结构化结果转回 unified 文本。
    // 有了结构之后，换成"按块渲染 + 高亮"只是这一步的事，算法那边不用再动。
    const QStringList oldLines = LineDiff::splitLines(history->contentOf(repoDir, previous.hash));
    const QStringList newLines = LineDiff::splitLines(history->contentOf(repoDir, newest.hash));
    const QString diffText = LineDiff::toUnifiedText(localDiff, oldLines, newLines, 3);

    showTextDialog(
        QStringLiteral("与上一版对比 — %1").arg(files->fileName()),
        QStringLiteral("%1（%2） → %3（%4）\n"
                       "- 开头是上一版的内容，+ 开头是这一版新增的内容\n"
                       "（共 %5 行新增、%6 行删除，由自研行级 diff 计算）")
            .arg(previous.shortHash,
                 previous.time.toString(QStringLiteral("MM-dd HH:mm:ss")),
                 newest.shortHash,
                 newest.time.toString(QStringLiteral("MM-dd HH:mm:ss")))
            .arg(localDiff.insertedLines)
            .arg(localDiff.deletedLines),
        diffText.isEmpty() ? QStringLiteral("（两个版本的内容完全相同）") : diffText);
}

// 回滚：把某个历史版本的内容载入编辑器。
// 刻意"只载入、不写盘"—— 用户看过内容确认没问题再按 Ctrl+S；
// 不满意直接不保存就行，所以回滚永远是可撤销的。
void MainWindow::onRollbackToVersion()
{
    FileManager *files = currentFiles();
    EditorWidget *editor = currentEditor();
    if (files == nullptr || editor == nullptr) {
        return;
    }
    if (!files->hasFilePath()) {
        QMessageBox::information(this,
                                 QStringLiteral("回滚到历史版本"),
                                 QStringLiteral("这个文档还没保存过，没有历史版本可以回滚。"));
        return;
    }

    VersionControl *history = files->versionControl();
    const QString repoDir = history->repositoryPathFor(files->filePath());

    QString error;
    const QList<VersionControl::Commit> commits = history->history(repoDir, 50, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("回滚到历史版本"), error);
        return;
    }
    if (commits.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("回滚到历史版本"),
            QStringLiteral("还没有任何快照。\n保存一次（Ctrl+S）就会留下第一份，之后就能回滚了。"));
        return;
    }

    QStringList items;
    for (const VersionControl::Commit &commit : commits) {
        items << QStringLiteral("%1  %2  %3")
                     .arg(commit.shortHash,
                          commit.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                          commit.message);
    }

    bool accepted = false;
    const QString chosen = QInputDialog::getItem(
        this,
        QStringLiteral("回滚到历史版本 — %1").arg(files->fileName()),
        QStringLiteral("选一个版本，它的内容会载入编辑器。\n"
                       "注意：**不会立刻写盘** —— 看过之后按 Ctrl+S 才保存；\n"
                       "不满意直接不做任何保存即可（当前内容不会被破坏）。"),
        items,
        0,      // 默认选中最新那条
        false,  // 不可编辑（只能从列表里选）
        &accepted);
    if (!accepted || chosen.isEmpty()) {
        return;
    }

    const int index = items.indexOf(chosen);
    if (index < 0 || index >= commits.size()) {
        return;
    }
    const VersionControl::Commit picked = commits.at(index);

    QString restoreError;
    if (!files->restoreSnapshot(picked.hash, &restoreError)) {
        QMessageBox::warning(this, QStringLiteral("回滚失败"), restoreError);
        return;
    }

    // 内容同步到编辑器和预览。
    // 注意 setPlainText 会触发 textChanged → setText(同样的内容) → 返回 false，
    // 所以预览要显式推一次（和打开文件时同样的道理）。
    editor->setPlainText(files->text());
    ui->workbench->showContent(files->text(), ui->workbench->previewBaseDir(), false);
    updateTabLabel(files);
    updateWindowTitle();
    updateCacheStatus();

    statusBar()->showMessage(QStringLiteral("已回滚到 %1（内容尚未写盘，按 Ctrl+S 保存）").arg(picked.shortHash));
}

// ============================ 全文搜索（5.5）============================
//
// 面板只发"用户点了这条命中"，剩下的都是主窗口的事：
// 打开文件（已经开着就直接切过去）、把光标移到那一行并**选中命中词**、在状态栏说一句。

void MainWindow::onSearchResultActivated(const SearchHit &hit)
{
    // openFile() 对"已经打开过的文件"会直接切过去，所以这里不用先判断有没有开过。
    // 打不开时（二进制文件、没权限……）它自己会弹窗说明原因，这里直接收工。
    if (!openFile(hit.filePath)) {
        return;
    }

    EditorWidget *editor = currentEditor();
    if (editor == nullptr) {
        return;
    }

    // 跳行只有一份实现（EditorWidget::goToLine）：夹范围、居中、拿焦点都在里面。
    // 这里传的是搜索结果里的行号，1 起算，和编辑器/状态栏的约定一致。
    //
    // ★ C1：把列号和命中长度也传下去 —— 跳过去之后命中词的这几个字会被选中。
    //   原来的写法只传行号，用户跳过去还停在行首，得自己在这一行里再找一遍。
    //   hit.matchStart 是 0 起算的下标，而 goToLine 要的是 1 起算的列号，所以 +1；
    //   没定位到命中时（matchStart == -1）退回"只跳到行首"，不传选区。
    if (hit.matchStart >= 0 && hit.matchLength > 0) {
        editor->goToLine(hit.line, hit.matchStart + 1, hit.matchLength);
    } else {
        editor->goToLine(hit.line);
    }

    statusBar()->showMessage(QStringLiteral("已跳到 %1：第 %2 行")
                                 .arg(QFileInfo(hit.filePath).fileName())
                                 .arg(hit.line),
                             5000);
}

// 搜索面板的目录跟着侧边栏的根目录走：用户在侧边栏里看到哪个目录，
// 搜索就索引哪个目录 —— 不需要在两个地方各选一遍。
void MainWindow::syncSearchDirectoryToSidebar()
{
    const QString root = ui->fileTree->rootPath();
    if (root.isEmpty()) {
        return;  // 侧边栏还没定根目录：保持面板上的原样，别把它清空
    }
    ui->searchPanel->setDirectory(root);
}

// ============================ 导出（5.6）============================
//
// 分工：ExportDialog 只负责"问用户要什么"（格式、路径、几项参数），
// Exporter 只负责"把 Markdown 变成 HTML/PDF 文件"（不弹窗、能单独测），
// 主窗口在这里把两件事接起来 + 把结果说给用户听。

void MainWindow::onExportHtml()
{
    exportCurrentDocument(false);
}

void MainWindow::onExportPdf()
{
    exportCurrentDocument(true);
}

void MainWindow::exportCurrentDocument(bool asPdf)
{
    FileManager *files = currentFiles();
    EditorWidget *editor = currentEditor();
    if (files == nullptr || editor == nullptr) {
        return;
    }

    // 导出的是"编辑器里现在的内容"，不要求先存盘 —— 但内容要先同步进管理器，
    // 否则会出现"我刚写的这一段没进导出文件"这种最让人意外的结果。
    // 走节流器的 flush（而不是直接 setText）：既能保证内容最新，
    // 又不会在这里重复实现一遍"同步"的逻辑（脏标志复位、标签刷新都在那一处）。
    m_syncScheduler.flushAll();

    if (asPdf && m_exporter.isPdfRunning()) {
        QMessageBox::information(this,
                                 QStringLiteral("还在导出"),
                                 QStringLiteral("已经有一个 PDF 正在生成。等它结束再来一次。"));
        return;
    }

    const ExportDialog::Format format = asPdf ? ExportDialog::Format::Pdf : ExportDialog::Format::Html;
    QString suggested = ExportDialog::suggestedPathFor(files->filePath(), format);
    if (!files->hasFilePath()) {
        // 还没存过盘的新文档：默认放到用户主目录（别往程序目录里写）
        suggested = QDir(QDir::homePath()).filePath(suggested);
    }

    // 对话框自己会把标题从"建议路径"推出来（a.md → a.html → 标题 a），
    // 所以这里不用另外传标题：用户在对话框里改目标文件名也不会改掉文档标题。
    ExportDialog dialog(format, suggested, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;  // 用户取消了
    }

    const ExportDialog::Request request = dialog.request();
    const QString baseDir = files->hasFilePath() ? QFileInfo(files->filePath()).absolutePath() : QString();

    // 导出跟随当前主题（5.7）：暗色主题下导出的 HTML 在浏览器里也是暗的。
    // PDF 例外 —— 打印样式会强制浅色（见 Exporter::exportOverrideStyleSheet）。
    const QString themeId = ThemeManager::themeId(ThemeManager::instance().theme());
    Exporter::HtmlOptions htmlOptions = request.html;
    htmlOptions.themeId = themeId;
    Exporter::PdfOptions pdfOptions = request.pdf;
    pdfOptions.themeId = themeId;

    if (request.format == ExportDialog::Format::Html) {
        Exporter::HtmlResult result;
        QString error;
        if (!m_exporter.exportHtml(files->text(), baseDir, request.targetPath, htmlOptions, &result, &error)) {
            QMessageBox::warning(this, QStringLiteral("导出失败"), error);
            return;
        }

        QString detail = QStringLiteral("%1 KB").arg(double(result.bytes) / 1024.0, 0, 'f', 1);
        if (result.imagesInlined > 0) {
            detail += QStringLiteral("，内联了 %1 张图片").arg(result.imagesInlined);
        }
        if (result.imagesSkipped > 0) {
            // 说清楚"哪几张没打包进去"，否则用户换了电脑才发现图片丢了
            detail += QStringLiteral("，%1 张图片没能内联（太大或找不到，仍是相对路径）").arg(result.imagesSkipped);
        }
        statusBar()->showMessage(QStringLiteral("已导出 HTML：%1（%2）").arg(QDir::toNativeSeparators(request.targetPath), detail), 8000);
        offerToOpenExportedFile(request.targetPath, false);
        return;
    }

    // PDF：异步。先给一句"正在生成"，结果由 pdfExported 信号带回来（见构造函数里的连接）。
    statusBar()->showMessage(QStringLiteral("正在生成 PDF：%1 …").arg(QDir::toNativeSeparators(request.targetPath)));
    m_exporter.exportPdf(files->text(), baseDir, request.targetPath, pdfOptions, request.title);
}

// 导出完成后问一句"要不要现在打开看看"。
// 这不是花架子：验收标准就是"导出的 HTML 浏览器打开正常、PDF 格式正确"，
// 一步能打开就省得用户自己去文件夹里翻。
void MainWindow::offerToOpenExportedFile(const QString &path, bool asPdf)
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("导出完成"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral("已导出：\n%1").arg(QDir::toNativeSeparators(path)));
    box.setInformativeText(asPdf ? QStringLiteral("要现在用系统默认的 PDF 阅读器打开看看吗？")
                                 : QStringLiteral("要现在用系统默认的浏览器打开看看吗？"));

    QPushButton *openFileButton = box.addButton(QStringLiteral("打开文件"), QMessageBox::AcceptRole);
    QPushButton *openDirButton = box.addButton(QStringLiteral("打开所在文件夹"), QMessageBox::ActionRole);
    box.addButton(QStringLiteral("完成"), QMessageBox::RejectRole);
    box.exec();

    if (box.clickedButton() == openFileButton) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else if (box.clickedButton() == openDirButton) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    }
}

// ============================ 插入代码块（5.7）============================

// "我要代码高亮，支持各种主流语言，需要自己去选择" —— 选择就发生在这里：
// 下拉框里列的是高亮器真正认识的语言（CodeHighlighter::supportedLanguages()），
// 选完插进去的 ```语言 会被预览、HTML 导出、PDF 导出同一套规则着色。
void MainWindow::onInsertCodeBlock()
{
    EditorWidget *editor = currentEditor();
    if (editor == nullptr) {
        return;
    }

    const QStringList languages = CodeHighlighter::supportedLanguages();
    QStringList shown;
    shown.reserve(languages.size());
    for (const QString &id : languages) {
        // "C++ (cpp)"：前面是给人看的名字，括号里是真正写进文档的语言名
        shown << QStringLiteral("%1 (%2)").arg(CodeHighlighter::displayNameFor(id), id);
    }

    bool accepted = false;
    const QString chosen = QInputDialog::getItem(this,
                                                 QStringLiteral("插入代码块"),
                                                 QStringLiteral("选一种语言（它决定预览里怎么着色）："),
                                                 shown,
                                                 0,
                                                 false,
                                                 &accepted);
    if (!accepted || chosen.isEmpty()) {
        return;
    }

    const int index = shown.indexOf(chosen);
    if (index < 0 || index >= languages.size()) {
        return;
    }
    const QString language = languages.at(index);

    editor->insertCodeBlock(language);
    statusBar()->showMessage(QStringLiteral("已插入 %1 代码块：在中间那行写代码，预览会按这种语言着色")
                                 .arg(CodeHighlighter::displayNameFor(language)),
                             8000);
}

// ============================ 主题（5.7）============================

// 主题一变，本窗口负责三件事：
//   1. 菜单上的勾跟着走（主题也可能是别处改的，比如启动时读配置）
//   2. 每个编辑器的语法配色 + 行号栏换色（QSS 管不到这些，它们是画出来的）
//   3. 预览区换主题（改 CSS 变量，**不重载页面** —— 所以不闪白、不丢滚动位置）
// 菜单栏/工具栏/标签页/状态栏/文件树/搜索面板这些由 QSS 自动跟，这里一行都不用写。
void MainWindow::onThemeChanged(ThemeManager::Theme theme)
{
    if (m_themeLightAction != nullptr) {
        m_themeLightAction->setChecked(theme == ThemeManager::Theme::Light);
    }
    if (m_themeDarkAction != nullptr) {
        m_themeDarkAction->setChecked(theme == ThemeManager::Theme::Dark);
    }

    const ThemePalette palette = ThemeManager::instance().currentPalette();
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it.key() != nullptr) {
            it.key()->setThemePalette(palette);
        }
    }

    if (ui->workbench != nullptr && ui->workbench->renderer() != nullptr) {
        ui->workbench->renderer()->applyTheme(ThemeManager::themeId(theme));
    }

    statusBar()->showMessage(QStringLiteral("已切换到%1主题")
                                 .arg(theme == ThemeManager::Theme::Dark ? QStringLiteral("暗色")
                                                                         : QStringLiteral("亮色")),
                             3000);
}

// ============================ 缓存（4.2.3）============================

void MainWindow::onClearCache()
{
    FileManager *files = currentFiles();
    if (files == nullptr) {
        return;
    }
    files->cacheManager()->clear();
    statusBar()->showMessage(QStringLiteral("已清空当前文档的内存缓存（下次打开会重新读盘）"));
    updateCacheStatus();
}

void MainWindow::updateCacheStatus()
{
    if (m_cacheLabel == nullptr) {
        return;
    }

    FileManager *files = currentFiles();
    if (files == nullptr) {
        m_cacheLabel->setText(QString());
        return;
    }

    const CacheManager *cache = files->cacheManager();
    const qint64 lookups = cache->hits() + cache->misses() + cache->staleCount();
    const double hitRate = lookups > 0 ? (100.0 * double(cache->hits()) / double(lookups)) : 0.0;

    m_cacheLabel->setText(QStringLiteral("缓存 %1/%2 条 · 命中 %3/%4（%5%）")
                              .arg(cache->size())
                              .arg(cache->maxEntries())
                              .arg(cache->hits())
                              .arg(lookups)
                              .arg(hitRate, 0, 'f', 0));
    // 鼠标悬停看完整统计（CacheManager 已经提供了一行可读文本）
    m_cacheLabel->setToolTip(cache->statisticsText());
}

void MainWindow::onCursorMoved(EditorWidget *editor, int line, int column){
    // 后台标签的光标变化不该刷新状态栏
    if (m_cursorLabel == nullptr || editor != currentEditor()) {
        return;
    }
    m_cursorLabel->setText(QStringLiteral("行 %1，列 %2").arg(line).arg(column));
}

// ============================ 其它 ============================

void MainWindow::updateWindowTitle()
{
    FileManager *files = currentFiles();
    const QString name = (files == nullptr) ? QStringLiteral("未命名") : files->fileName();
    const bool modified = (files != nullptr) && files->isModified();

    // 标题里的产品名统一成 muse-md（D3）：和仓库名、README、安装包一致。
    // 只改显示，不动 QApplication::applicationName（那是 AppData 路径的一部分，见 main.cpp）。
    setWindowTitle(QStringLiteral("%1%2 - muse-md")
                       .arg(modified ? QStringLiteral("*") : QString(), name));
}

// ============================ 查找 / 替换（编辑菜单）============================
//
// 对话框自己是"薄壳"：查找/替换的逻辑都在 EditorWidget 里（能脱离界面单独测）。
// 主窗口只负责两件事：把对话框指向**当前标签**的编辑器、并在切标签时重新指一次。

void MainWindow::onFind()
{
    if (currentEditor() == nullptr) {
        return;
    }
    m_findDialog.setEditor(currentEditor());
    m_findDialog.show();
    m_findDialog.raise();
    m_findDialog.activateWindow();
    m_findDialog.focusSearchField();  // 打开就能直接打字
}

void MainWindow::onReplace()
{
    onFind();  // 同一个对话框：查找和替换是一体的（替换也要先有查找词）
}

// ============================ 帮助 ============================

void MainWindow::onAbout()
{
    const QString version = QCoreApplication::applicationVersion().isEmpty()
                                ? QStringLiteral("1.0.0")
                                : QCoreApplication::applicationVersion();

    QMessageBox::about(
        this,
        QStringLiteral("关于 muse-md"),
        QStringLiteral("<h3>muse-md %1</h3>"
                       "<p>一个用 Qt 6 + C++17 写的 Markdown 编辑器，带实时双向预览、"
                       "本地版本历史、全文搜索、代码高亮与亮暗主题。</p>"
                       "<p><b>关于：</b>%2<br/>"
                       "<b>Qt：</b>%3（运行时 %4）<br/>"
                       "<b>Markdown 渲染：</b>md4c 0.5.3</p>"
                       "<p>配置与索引都在 <code>%%APPDATA%%/Dev/MarkdownEditor/</code> 下，"
                       "删掉它们不会丢笔记。</p>")
            .arg(version,
                 QCoreApplication::applicationFilePath(),
                 QLatin1String(qVersion()),
                 QLatin1String(qVersion())));
}

// ============================ 拖拽打开（6.2）============================
//
// 从资源管理器把 .md 拖进窗口就能打开。三步里只有两步需要写：
//   dragEnterEvent：决定"接不接受"。不接受的话鼠标会显示禁止图标，用户立刻知道不行。
//   dragMoveEvent：位置变化时的回调。这里**故意不重写** —— 默认实现沿用了
//                  dragEnterEvent 的答案（accept/ignore 状态会保持），我们也没有
//                  "拖到不同区域做不同事"的需求。
//   dropEvent：真正打开文件。

// 纯逻辑：从拖进来的 MIME 数据里挑出可打开的文件。
// 规则写得保守一点：只收本地文件（不是 http/ftp 那种），只收 Markdown/纯文本后缀，
// 拖进来的目录会被展开成里面第一层的 .md（"拖个文件夹过来"是很常见的操作）。
QStringList MainWindow::droppedFiles(const QMimeData *data)
{
    QStringList paths;
    if (data == nullptr || !data->hasUrls()) {
        return paths;
    }

    // 后缀白名单和"打开文件"对话框里的过滤器保持一致，免得两边对不上
    const QStringList allowedSuffixes = {QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("txt")};

    for (const QUrl &url : data->urls()) {
        if (!url.isLocalFile()) {
            continue;  // 网络地址：我们打不开（也不该去下载）
        }
        const QFileInfo info(url.toLocalFile());
        if (info.isDir()) {
            // 目录：找出里面第一层的 Markdown 文件（不递归 —— 拖一个大目录进来
            // 一下开几十个标签更可能是事故，不是本意）
            const QDir dir(info.absoluteFilePath());
            const QStringList entries = dir.entryList(QStringList{QStringLiteral("*.md"), QStringLiteral("*.markdown")},
                                                      QDir::Files,
                                                      QDir::Name);
            for (const QString &name : entries) {
                paths << dir.absoluteFilePath(name);
            }
            continue;
        }
        if (info.isFile() && allowedSuffixes.contains(info.suffix().toLower())) {
            paths << info.absoluteFilePath();
        }
    }
    return paths;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    // 只看"能不能从里面挑出文件"：挑不出来就不接受（鼠标会显示禁止图标）
    if (!droppedFiles(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

// 窗口重新获得焦点时检查当前标签有没有被外部改过（7.3）。
// 为什么挑这个时机：用户"从别的程序切回来"正是刚刚可能改过这个文件的时刻。
// 注意只在**焦点回来**时查，不在失去焦点时查 —— 后者会让"正要切过去改文件"的瞬间弹窗。
bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowActivate) {
        // 先交给基类处理（它是窗口激活的一部分），再检查
        const bool handled = QMainWindow::event(event);
        checkCurrentExternalChange();
        return handled;
    }
    return QMainWindow::event(event);
}

void MainWindow::dropEvent(QDropEvent *event){
    const QStringList paths = droppedFiles(event->mimeData());
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();

    // 多个文件就依次打开（openFile 对"已经开着的文件"会切过去，不会重复开）
    int opened = 0;
    for (const QString &path : paths) {
        if (openFile(path)) {
            ++opened;
        }
    }
    LOG_INFO("拖拽打开：%1 个文件（收到 %2 个）", opened, paths.size());
    statusBar()->showMessage(opened == 1 ? QStringLiteral("已打开：%1").arg(QFileInfo(paths.first()).fileName())
                                         : QStringLiteral("已打开 %1 个文件").arg(opened),
                             5000);
}

// ============================ 窗口记忆（6.2）============================

void MainWindow::restoreSession()
{
    const SessionState::Data state = SessionState::load();

    // ---- 窗口几何 ----
    // restoreGeometry() 自己会处理"存档里的显示器已经拔了/分辨率变了"这种情况：
    // 恢复不了时返回 false，这时就保持 .ui 里的默认大小，别把窗口摆到看不见的地方。
    if (!state.geometry.isEmpty() && !restoreGeometry(state.geometry)) {
        LOG_WARN("上次的窗口位置恢复不了（显示器变了？），这次用默认大小");
    }

    // ---- 停靠面板 ----
    ui->fileTreeDock->setVisible(state.fileTreeVisible);
    ui->searchDock->setVisible(state.searchPanelVisible);
    // C4：大纲和文件树叠在同一个停靠区，恢复可见性时也要保证"文件树那页在最前面"——
    // 否则会出现"整叠是显示的、但用户看到的是大纲"，和 fileTreeVisible=true 的语义不符。
    ui->outlineDock->setVisible(state.outlinePanelVisible);
    if (!state.outlinePanelVisible) {
        ui->fileTreeDock->raise();
    }

    // ---- C2：把每个标签的光标/滚动位置也装进内存 ----
    // 先装进来，后面 openFile() 触发的 onCurrentTabChanged 走到 applyViewState() 时就有得查了。
    m_viewState = state.viewState;

    // ---- 上次打开的文件 ----
    // 先收集"还存在的"：磁盘上没了的直接跳过（只记一条日志，不弹窗打扰）。
    // 一个都没有时，那个开着的空标签就留着用。
    QStringList existing;
    for (const QString &path : state.openFiles) {
        if (QFileInfo::exists(path)) {
            existing << path;
        } else {
            LOG_INFO("上次打开的文件已经不在了，跳过：%1", path);
        }
    }

    if (existing.isEmpty()) {
        return;
    }

    // 第一个文件复用那个空标签：新建的窗口本来会留一个"未命名"空标签，
    // 直接在那个标签里打开第一个文件，就不会多出一个没用的空标签。
    EditorWidget *first = currentEditor();
    if (first != nullptr && first->document()->isEmpty()) {
        FileManager *files = filesFor(first);
        QString error;
        if (files != nullptr && files->openFile(existing.first(), &error)) {
            first->setPlainText(files->text());
            updateTabLabel(files);
            existing.removeFirst();
        }
    }

    for (const QString &path : existing) {
        openFile(path);
    }

    // 回到上次正在看的那一个
    if (state.currentIndex > 0 && state.currentIndex < ui->tabManager->count()) {
        ui->tabManager->setCurrentIndex(state.currentIndex);
    }

    // ★ C2：最后再对**当前这个标签**恢复一次位置。
    //
    // 为什么不能只靠 onCurrentTabChanged()：第一个文件是**复用那个空标签**打开的
    // （见上面 `first->setPlainText(...)`），整个过程没有发生标签切换，
    // 那条路径就不会被触发，光标会停在文档开头。
    // 这里补一次，保证"启动后当前标签的位置"一定是对的（重复调用是幂等的：
    // applyViewState() 只是把光标和滚动条设成记下来的值）。
    if (EditorWidget *editor = currentEditor()) {
        if (const FileManager *files = filesFor(editor)) {
            if (files->hasFilePath()) {
                applyViewState(editor, files->filePath());
            }
        }
    }

    statusBar()->showMessage(QStringLiteral("已恢复上次的会话（%1 个文件）").arg(ui->tabManager->count()), 5000);
}

QStringList MainWindow::openFilePaths() const
{
    // 按标签顺序收集"有磁盘路径"的标签（没保存过的新文档没有路径，不记）
    QStringList paths;
    for (int i = 0; i < ui->tabManager->count(); ++i) {
        if (const FileManager *files = filesFor(ui->tabManager->editorAt(i))) {
            if (files->hasFilePath()) {
                paths << files->filePath();
            }
        }
    }
    return paths;
}

void MainWindow::saveSession() const
{
    SessionState::Data state;
    state.geometry = saveGeometry();
    state.openFiles = openFilePaths();
    state.currentIndex = ui->tabManager->currentIndex();
    state.fileTreeVisible = ui->fileTreeDock->isVisible();
    state.searchPanelVisible = ui->searchDock->isVisible();
    state.outlinePanelVisible = ui->outlineDock->isVisible();

    // ★ C2：把所有标签的光标/滚动位置都记一遍（不只是当前那个）——
    //   用户关窗口时希望"下次打开每个标签都还在原来的地方"。
    //   本函数是 const，所以先拷一份内存里的表再往拷本里写，不改窗口的可变状态；
    //   用 recordEditorViewState() 而不是重写一遍取值逻辑，保证和
    //   rememberViewState() 永远是同一套规则（行列 1 起算、滚动取 verticalScrollBar）。
    QHash<QString, QString> viewState = m_viewState;
    for (int i = 0; i < ui->tabManager->count(); ++i) {
        EditorWidget *editor = ui->tabManager->editorAt(i);
        const FileManager *files = filesFor(editor);
        if (editor == nullptr || files == nullptr || !files->hasFilePath()) {
            continue;
        }
        recordEditorViewState(&viewState, editor, files->filePath());
    }
    state.viewState = viewState;

    SessionState::save(state);
}

// 关窗口：**每个**有未保存修改的标签都问一遍。
// 和关标签共用 maybeSave()，保证两种入口的行为完全一致 ——
// 少这一处的话，用户点右上角关闭就会把没保存的内容丢掉。
void MainWindow::closeEvent(QCloseEvent *event)
{
    for (int i = 0; i < ui->tabManager->count(); ++i) {
        if (!maybeSave(filesFor(ui->tabManager->editorAt(i)))) {
            event->ignore();  // 用户选了取消：窗口不关，继续编辑
            return;
        }
    }

    // 真的要关了：把窗口几何 + 这次打开的文件记下来（下次启动恢复）。
    // 放在"用户没取消"之后：取消关闭时不该把状态写进去，否则下次启动会恢复一个
    // 用户其实没打算留下的会话。
    saveSession();
    event->accept();
}
