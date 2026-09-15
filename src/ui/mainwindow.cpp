#include "mainwindow.h"

#include "ui_mainwindow.h"  // uic 根据 mainwindow.ui 生成（AUTOUIC 负责，不用手工写）

#include "editorwidget.h"       // .ui 里的 tabManager 会用它的页面；界面要连它的 cursorMoved
#include "codehighlighter.h"    // 5.7：代码高亮（"插入代码块"的语言列表就是它提供的）
#include "editorworkbench.h"    // .ui 中央区那台"工作台"（分屏 + 预览 + 双向同步）
#include "exportdialog.h"       // 5.6：导出对话框（只收集设置，干活的是 Exporter）
#include "filetreeview.h"       // 5.4.1：左边的文件树侧边栏（.ui 里已经有一块 FileTreeView）
#include "logger.h"
#include "previewrenderer.h"    // updateContent() 要用完整类型（渲染管线归工作台持有，这里只是借来用）
#include "recentfiles.h"        // 5.4.2：最近打开的文件列表
#include "searchpanel.h"        // 5.5：全文搜索面板（.ui 里就是一个 SearchPanel）
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
// 代码高亮（5.7）："插入代码块"的语言列表和显示名都来自它
using markdown_editor::core::document::CodeHighlighter;
// 主题配色（5.7）：ThemeManager 是全局命名空间的类，但它返回的配色表在 core::document 里
using markdown_editor::core::document::ThemePalette;
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
}

// 菜单/工具栏/动作：这些用 .ui 表达不了 ——
// 快捷键（QKeySequence::Open）、动作对象、triggered 连接都是 C++ 的事，
// 所以这一整块和"中央布局用不用 .ui"无关，永远是代码。
void MainWindow::initMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    m_newAction = fileMenu->addAction(QStringLiteral("新建标签(&N)"));
    m_newAction->setShortcut(QKeySequence::New);
    connect(m_newAction, &QAction::triggered, this, &MainWindow::onNewFile);

    m_openAction = fileMenu->addAction(QStringLiteral("打开(&O)…"));
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::onOpenFile);

    // ---- 文件树侧边栏（5.4.1）：选一个目录作为侧边栏的根 ----
    m_openFolderAction = fileMenu->addAction(QStringLiteral("打开文件夹(&K)…"));
    connect(m_openFolderAction, &QAction::triggered, this, &MainWindow::onOpenFolder);

    // ---- 最近打开（5.4.2）----
    // 只建"壳"：条目由 rebuildRecentMenu() 按 m_recent 的内容重建。
    m_recentMenu = fileMenu->addMenu(QStringLiteral("最近打开(&R)"));

    m_saveAction = fileMenu->addAction(QStringLiteral("保存(&S)"));
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::onSaveFile);

    m_saveAsAction = fileMenu->addAction(QStringLiteral("另存为(&A)…"));
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
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

    // ---- 主题（5.7）----
    // 视图 → 主题 → 亮色 / 暗色。切换只调 ThemeManager：它负责 QSS + 调色板 + 落盘 + 发信号，
    // 本窗口和编辑器、预览区都只是"响应者"，不需要互相知道对方也要换色。
    QMenu *themeMenu = viewMenu->addMenu(QStringLiteral("主题(&T)"));
    m_themeGroup = new QActionGroup(this);
    m_themeGroup->setExclusive(true);

    m_themeLightAction = themeMenu->addAction(QStringLiteral("亮色(&L)"));
    m_themeLightAction->setCheckable(true);
    m_themeGroup->addAction(m_themeLightAction);
    connect(m_themeLightAction, &QAction::triggered, this, [this] {
        Q_UNUSED(this);
        ThemeManager::instance().setTheme(ThemeManager::Theme::Light);
    });

    m_themeDarkAction = themeMenu->addAction(QStringLiteral("暗色(&D)"));
    m_themeDarkAction->setCheckable(true);
    m_themeGroup->addAction(m_themeDarkAction);
    connect(m_themeDarkAction, &QAction::triggered, this, [this] {
        Q_UNUSED(this);
        ThemeManager::instance().setTheme(ThemeManager::Theme::Dark);
    });

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

    // ---- 帮助菜单 ----
    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    m_aboutAction = helpMenu->addAction(QStringLiteral("关于(&A)…"));
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
    m_redoAction = addEditorAction(QStringLiteral("重做(&R)"), QKeySequence::Redo, &QPlainTextEdit::redo);
    // 重做在 Windows 上是 Ctrl+Y，在 macOS/Linux 上是 Ctrl+Shift+Z。这里两个都收：
    // 用户从别的编辑器过来时手会是习惯的那个。
    m_redoAction->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)});

    editMenu->addSeparator();
    m_cutAction = addEditorAction(QStringLiteral("剪切(&T)"), QKeySequence::Cut, &QPlainTextEdit::cut);
    m_copyAction = addEditorAction(QStringLiteral("复制(&C)"), QKeySequence::Copy, &QPlainTextEdit::copy);
    m_pasteAction = addEditorAction(QStringLiteral("粘贴(&P)"), QKeySequence::Paste, &QPlainTextEdit::paste);

    editMenu->addSeparator();
    m_selectAllAction = addEditorAction(QStringLiteral("全选(&A)"), QKeySequence::SelectAll, &QPlainTextEdit::selectAll);

    // ---- 查找 / 替换 ----
    // 注意 Ctrl+F 在 Qt 里是 QKeySequence::Find；编辑器本身不处理它，所以不会冲突。
    editMenu->addSeparator();
    m_findAction = editMenu->addAction(QStringLiteral("查找(&F)…"));
    m_findAction->setShortcut(QKeySequence::Find);
    connect(m_findAction, &QAction::triggered, this, &MainWindow::onFind);

    m_replaceAction = editMenu->addAction(QStringLiteral("替换(&H)…"));
    m_replaceAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
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
    m_darkThemeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));  // 6.2：切换主题
    m_darkThemeAction->setToolTip(QStringLiteral("在亮色 / 暗色主题之间切换（Ctrl+Shift+T）"));
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
        const int chars = editor->toPlainText().size();
        const int lines = editor->document()->blockCount();
        m_charCountLabel->setText(QStringLiteral("字符 %1 · 行 %2").arg(chars).arg(lines));
    }

    if (m_modifiedLabel != nullptr) {
        const bool modified = files->isModified();
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
    editor->setThemePalette(ThemeManager::editorPalette(ThemeManager::instance().theme()));

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
    info.modified = files->isModified();
    ui->tabManager->updateTab(ui->tabManager->indexOf(editor), info);
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

    // 保证管理器里是最新内容（正常打字时 setText 已经同步过，这里是保险）
    files->setText(editor->toPlainText());

    if (!files->hasFilePath()) {
        return saveSessionAs(files, editor);  // 新文档还没有路径 → 走另存为
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

    // 内容真的变了才继续往下走：setText() 在"内容没变"时返回 false
    // （例如打开文件时 setPlainText 带来的那一次 textChanged）
    if (!files->setText(editor->toPlainText())) {
        return;
    }

    updateTabLabel(files);  // 标签上的 * 立刻出现

    // 只有当前标签的改动才推到预览：别的标签改内容（比如程序自己填充）不该抢走预览
    if (editor == currentEditor()) {
        // 防抖（300ms）在渲染管线里：敲字时它会把这次更新一直往后推，
        // 停下来之后才真正渲染一次。渲染管线归工作台所有，这里只是借来用。
        ui->workbench->renderer()->updateContent(files->text());
    }

    updateWindowTitle();
    updateDocumentStatus();  // 字符数 / 行数 / 修改状态都是随打字变的
}

// ============================ 标签切换 ============================

void MainWindow::onCurrentTabChanged(EditorWidget *editor)
{
    if (editor == nullptr) {
        // 所有标签都被关掉了：立刻补一个干净的新标签。
        // 这样"界面上永远有一个编辑器"这条不变式一直成立，后面所有代码都能少写判空。
        createSession();
        return;
    }

    FileManager *files = filesFor(editor);
    showSession(files, false);  // 同目录时只推内容，不重载页面（不闪白）

    // 告诉工作台"现在编辑的是这个编辑器"：它会接上这个编辑器的滚动条（并断开上一个），
    // 顺便把预览滚到它的当前顶行 —— 这两件事原来散在主窗口里。
    ui->workbench->setCurrentEditor(editor);

    // 状态栏的行列要换成这个标签的光标位置
    const QTextCursor cursor = editor->textCursor();
    onCursorMoved(editor, cursor.blockNumber() + 1, cursor.positionInBlock() + 1);

    // 状态栏那一组"当前文档"信息（字符数/路径/修改状态）也要换成这个标签的
    updateDocumentStatus();

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
    }
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

// 有未保存的修改时先问一句。返回 false = 用户取消，调用方必须中止当前操作。
bool MainWindow::maybeSave(FileManager *files)
{
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

    const QString diffText = history->diff(repoDir, previous.hash, newest.hash, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("与上一版对比"), error);
        return;
    }

    showTextDialog(
        QStringLiteral("与上一版对比 — %1").arg(files->fileName()),
        QStringLiteral("%1（%2） → %3（%4）\n- 开头是上一版的内容，+ 开头是这一版新增的内容")
            .arg(previous.shortHash,
                 previous.time.toString(QStringLiteral("MM-dd HH:mm:ss")),
                 newest.shortHash,
                 newest.time.toString(QStringLiteral("MM-dd HH:mm:ss"))),
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
// 面板只发"用户点了这个文件的这一行"，剩下的都是主窗口的事：
// 打开文件（已经开着就直接切过去）、把光标移到那一行、在状态栏说一句。

void MainWindow::onSearchResultActivated(const QString &filePath, int line)
{
    // openFile() 对"已经打开过的文件"会直接切过去，所以这里不用先判断有没有开过。
    // 打不开时（二进制文件、没权限……）它自己会弹窗说明原因，这里直接收工。
    if (!openFile(filePath)) {
        return;
    }

    EditorWidget *editor = currentEditor();
    if (editor == nullptr) {
        return;
    }

    // 跳行只有一份实现（EditorWidget::goToLine）：夹范围、居中、拿焦点都在里面。
    // 这里传的是搜索结果里的行号，1 起算，和编辑器/状态栏的约定一致。
    editor->goToLine(line);
    statusBar()->showMessage(QStringLiteral("已跳到 %1：第 %2 行").arg(QFileInfo(filePath).fileName()).arg(line), 5000);
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
    files->setText(editor->toPlainText());

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

    const ThemePalette palette = ThemeManager::editorPalette(theme);
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

    setWindowTitle(QStringLiteral("%1%2 - Markdown 编辑器")
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
        QStringLiteral("关于 Markdown 编辑器"),
        QStringLiteral("<h3>Markdown 编辑器 %1</h3>"
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

void MainWindow::dropEvent(QDropEvent *event)
{
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
