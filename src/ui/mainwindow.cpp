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

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
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

    // ---- 文件树侧边栏（5.4.1）----
    // 侧边栏是 .ui 里 workbench 的**第一块**子控件，所以它已经在分屏里了；
    // 工作台不认识它（工作台只管"编辑器侧 / 预览侧"两块），也不该认识 ——
    // 侧边栏是"另一条独立的东西"，三种显示模式切来切去都不影响它。
    ui->fileTree->setRootPath(QDir::currentPath());  // 没选过目录时从工作目录开始

    // 初始比例只能由这里给：工作台不知道 splitter 里一共有几块（见 EditorWorkbench::setSplitSizes）
    ui->workbench->setSplitSizes({220, 490, 490});

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

    // ---- 文件树侧边栏开关（5.4.1）----
    viewMenu->addSeparator();
    m_showFileTreeAction = viewMenu->addAction(QStringLiteral("显示文件树(&F)"));
    m_showFileTreeAction->setCheckable(true);
    m_showFileTreeAction->setChecked(true);  // .ui 里默认就是可见的，勾选状态要和它一致
    connect(m_showFileTreeAction, &QAction::toggled, this, [this](bool visible) {
        // 只是隐藏，不销毁：QFileSystemModel 的目录监听和展开状态都留着，
        // 再打开时还是原来的样子。
        ui->fileTree->setVisible(visible);
    });

    // ---- 全文搜索面板（5.5）----
    // Ctrl+Shift+F 是"在文件里搜"的通用手势；面板做成停靠窗口，开关就是它的可见性。
    m_searchAction = viewMenu->addAction(QStringLiteral("全文搜索(&S)"));
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
}

void MainWindow::initToolBar()
{
    QToolBar *toolBar = addToolBar(QStringLiteral("主工具栏"));
    toolBar->setMovable(false);
    if (m_newAction) {
        toolBar->addAction(m_newAction);
    }
    if (m_openAction) {
        toolBar->addAction(m_openAction);
    }
    if (m_saveAction) {
        toolBar->addAction(m_saveAction);
    }
    if (m_saveAsAction) {
        toolBar->addAction(m_saveAsAction);
    }
}

void MainWindow::initStatusBar()
{
    statusBar()->showMessage(QStringLiteral("就绪"));

    // 光标位置：由 EditorWidget::cursorMoved 推过来（行列都从 1 起算）
    m_cursorLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_cursorLabel);

    // 右侧常驻的缓存状态：这是"第二次打开同一个文件走了缓存"最直观的可见证据。
    // 用 addPermanentWidget（不会被临时消息顶掉），鼠标悬停能看到完整统计。
    m_cacheLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_cacheLabel);
    updateCacheStatus();
}

// ============================ 会话（一个标签 = 一个文档）============================

EditorWidget *MainWindow::createSession()
{
    // FileManager 的父对象是主窗口：即使某个标签被关掉忘了回收，也不会泄漏到进程结束
    auto *files = new FileManager(this);
    EditorWidget *editor = ui->tabManager->addEditorTab();

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

    if (request.format == ExportDialog::Format::Html) {
        Exporter::HtmlResult result;
        QString error;
        if (!m_exporter.exportHtml(files->text(), baseDir, request.targetPath, request.html, &result, &error)) {
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
    m_exporter.exportPdf(files->text(), baseDir, request.targetPath, request.pdf, request.title);
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

void MainWindow::onCursorMoved(EditorWidget *editor, int line, int column)
{
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
    event->accept();
}
