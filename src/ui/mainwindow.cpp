#include "mainwindow.h"

#include "ui_mainwindow.h"  // uic 根据 mainwindow.ui 生成（AUTOUIC 负责，不用手工写）

#include "editorwidget.h"  // .ui 里的 tabManager 会用它的页面；界面要连它的 cursorMoved
#include "logger.h"
#include "syncbridge.h"
#include "tabmanager.h"

#include <QAction>
#include <QCloseEvent>
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
#include <QScrollBar>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEnginePage>  // attach() 返回页面，交给 QWebChannel 当父对象（要完整类型才能转 QObject*）
#include <QWebEngineView>

using markdown_editor::core::document::PreviewRenderer;
using markdown_editor::core::document::SyncBridge;
using markdown_editor::core::storage::CacheManager;
using markdown_editor::core::storage::VersionControl;
// 注意：FileManager 不用在这里 using —— MainWindow 内部有一份同名别名（见 mainwindow.h），
// 成员函数体里直接用短名字就行，不会和全局作用域冲突。

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

    // 把预览视图交给渲染管线：它会给 view 换一个"能转发 console 日志"的页面，
    // 并持有页面、负责模板加载和内容推送（4.1.4）
    QWebEnginePage *previewPage = m_renderer.attach(ui->preview);

    // 左右各占一半（splitter 的初始比例是运行期设置，.ui 里表达不了）
    ui->splitter->setStretchFactor(0, 1);
    ui->splitter->setStretchFactor(1, 1);
    ui->splitter->setSizes({600, 600});

    // ---- WebChannel：把 C++ 的同步桥暴露给页面里的 JS ----
    // 名字 "syncBridge" 必须和模板里 channel.objects.syncBridge 完全一致
    m_bridge = new SyncBridge(this);
    auto *channel = new QWebChannel(previewPage);
    channel->registerObject(QStringLiteral("syncBridge"), m_bridge);
    previewPage->setWebChannel(channel);

    connect(m_bridge, &SyncBridge::previewClicked, this, &MainWindow::onPreviewClicked);

    // 内容刚被推给页面 → 页面里的块是新的，滚动位置得重新对齐一次（用当前标签）
    connect(&m_renderer, &PreviewRenderer::contentRendered, this, [this] {
        onEditorScrolled(currentEditor());
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

    // 先把预览外壳页面装起来（内容等页面加载完、渲染器自己补推）
    m_renderer.loadTemplate(QString());
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

    m_saveAction = fileMenu->addAction(QStringLiteral("保存(&S)"));
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::onSaveFile);

    m_saveAsAction = fileMenu->addAction(QStringLiteral("另存为(&A)…"));
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::onSaveFileAs);

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

    // ---- 工具菜单 ----
    QMenu *toolsMenu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    m_clearCacheAction = toolsMenu->addAction(QStringLiteral("清空内存缓存(&C)"));
    connect(m_clearCacheAction, &QAction::triggered, this, &MainWindow::onClearCache);
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
    connect(editor, &QPlainTextEdit::textChanged, this, [this, editor] { onEditorTextChanged(editor); });
    connect(editor->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, editor] {
        onEditorScrolled(editor);
    });
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

    if (forceReload || dir != m_previewBaseDir) {
        // 目录变了（或调用方明确要求）：重新加载模板，baseUrl 跟着换 ——
        // 文档里的相对路径图片靠它才找得到
        m_previewBaseDir = dir;
        m_renderer.updateContent(files->text());
        m_renderer.loadTemplate(dir);
    } else {
        // 只是换了个标签：只推内容，页面不重载。
        // 重载页面会闪一下白屏、还会让 WebChannel 重连，切标签时手感很差。
        m_renderer.updateContentNow(files->text());
    }

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
        // 防抖（300ms）在 PreviewRenderer 里：敲字时它会把这次更新一直往后推，
        // 停下来之后才真正渲染一次。
        m_renderer.updateContent(files->text());
    }

    updateWindowTitle();
}

// ============================ 预览 → 编辑器 ============================

void MainWindow::onPreviewClicked(int line)
{
    EditorWidget *editor = currentEditor();
    if (editor == nullptr) {
        return;
    }

    QTextDocument *doc = editor->document();
    if (doc->blockCount() <= 0) {
        return;
    }

    // 1 起算的行号 → QPlainTextEdit 的 blockNumber()（0 起算），并夹到合法范围，
    // 防止"预览的行号比编辑器的行数还大"时越界
    const int blockNumber = qBound(0, line - 1, doc->blockCount() - 1);

    QTextCursor cursor(doc->findBlockByNumber(blockNumber));
    editor->setTextCursor(cursor);
    editor->centerCursor();  // 让目标行落在屏幕中间，而不是贴着边
    editor->setFocus();

    LOG_INFO("预览点击 → 编辑器跳到第 %1 行", line);
}

// ============================ 编辑器滚动 → 预览滚动 ============================

void MainWindow::onEditorScrolled(EditorWidget *editor)
{
    if (editor == nullptr || m_bridge == nullptr) {
        return;
    }
    // 后台标签的滚动不该带走预览
    if (editor != currentEditor()) {
        return;
    }

    // 当前最顶可见行 = 视口左上角那个位置对应的文本块。
    // 注意：QPlainTextEdit::firstVisibleBlock() 是 protected 的，外部调不到，
    // 所以走 cursorForPosition()（它接受的是视口坐标）。
    const int blockNumber = editor->cursorForPosition(QPoint(0, 0)).blockNumber();
    const int line = blockNumber + 1;  // blockNumber() 是 0 起算 → +1 变成"人类行号"

    m_bridge->reportEditorScroll(line);  // → editorScrolled 信号 → JS scrollToLine()
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

    // 状态栏的行列要换成这个标签的光标位置
    const QTextCursor cursor = editor->textCursor();
    onCursorMoved(editor, cursor.blockNumber() + 1, cursor.positionInBlock() + 1);

    // 让预览滚到这个标签的当前顶行（不然切过来还停在上一篇的位置上）
    onEditorScrolled(editor);
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
    m_renderer.updateContent(files->text());
    updateTabLabel(files);
    updateWindowTitle();
    updateCacheStatus();

    statusBar()->showMessage(QStringLiteral("已回滚到 %1（内容尚未写盘，按 Ctrl+S 保存）").arg(picked.shortHash));
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
