#include "mainwindow.h"

#include "ui_mainwindow.h"  // uic 根据 mainwindow.ui 生成（AUTOUIC 负责，不用手工写）

#include "editorwidget.h"  // .ui 里把 editor 提升成了它（界面要用它的 cursorMoved 信号）
#include "logger.h"
#include "syncbridge.h"

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
using markdown_editor::core::storage::FileManager;
using markdown_editor::core::storage::VersionControl;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    // ★ 一行顶掉原来 initUi 里"建 splitter / 建 editor / 建 preview / 加进布局"那 20 行：
    //   控件和布局现在由 mainwindow.ui 描述，uic 生成代码，这里只负责"装上去"。
    //   装好之后就能用 ui->editor / ui->preview / ui->splitter；菜单栏和状态栏仍然用
    //   QMainWindow 的 menuBar() / statusBar() 取（initMenuBar / initStatusBar 里）。
    ui->setupUi(this);

    initUi();
    initMenuBar();
    initToolBar();
    initStatusBar();
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

    // 注意：制表位宽度和语法高亮现在都不在这里设置了（5.1 起归 EditorWidget 自己管）：
    // 高亮器挂在它的文档上、缩进宽度决定制表位宽度，主窗口不用再照顾这些细节。

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

    // ---- 信号槽接线 ----
    connect(m_bridge, &SyncBridge::previewClicked, this, &MainWindow::onPreviewClicked);
    connect(ui->editor, &QPlainTextEdit::textChanged, this, &MainWindow::onEditorTextChanged);
    connect(ui->editor->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::onEditorScrolled);

    // 内容刚被推给页面 → 页面里的块是新的，滚动位置得重新对齐一次
    // （以前这行写在 pushContentToPreview() 末尾，现在渲染器不再认识编辑器，改用信号通知）
    connect(&m_renderer, &PreviewRenderer::contentRendered, this, &MainWindow::onEditorScrolled);

    // 光标位置 → 状态栏。行列都从 1 起算，EditorWidget 已经换算好了，
    // 所以这里不需要再做 0/1 转换（全项目的 1 起算约定由控件内部兜住）。
    connect(ui->editor, &EditorWidget::cursorMoved, this, &MainWindow::onCursorMoved);

    // ---- 文件管理器的信号：文件层面的变化 → 界面提示 ----
    // 主窗口不认识"编码/只读/脏标志"这些细节，只负责把管理器的结论显示出来：
    connect(&m_files, &FileManager::fileOpened, this, &MainWindow::onFileOpened);
    connect(&m_files, &FileManager::fileSaved, this, &MainWindow::onFileSaved);
    connect(&m_files, &FileManager::modificationChanged, this, &MainWindow::onModificationChanged);
    connect(&m_files, &FileManager::readOnlyDetected, this, &MainWindow::onReadOnlyDetected);

    // 先把预览外壳页面装起来（内容等页面加载完、渲染器自己补推）
    m_renderer.loadTemplate(QString());
}

// 菜单/工具栏/动作：这些用 .ui 表达不了 ——
// 快捷键（QKeySequence::Open）、动作对象、triggered 连接都是 C++ 的事，
// 所以这一整块和"中央布局用不用 .ui"无关，永远是代码。
void MainWindow::initMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    m_newAction = fileMenu->addAction(QStringLiteral("新建(&N)"));
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

// ============================ 文件与文档 ============================

bool MainWindow::openFile(const QString &path)
{
    // 读盘、判编码、解码、记住路径和只读状态，全在 FileManager 里完成。
    QString error;
    if (!m_files.openFile(path, &error)) {
        // 失败原因由管理器给出（打不开 / 是文件夹 / 没有权限…），主窗口只负责显示
        QMessageBox::warning(this, QStringLiteral("打开失败"), QStringLiteral("%1\n\n%2").arg(error, path));
        return false;
    }

    // 编辑器显示文档内容。setPlainText 会触发 textChanged → onEditorTextChanged
    // → m_files.setText(同样的内容) → 内容没变，所以不会置脏 ✓
    ui->editor->setPlainText(m_files.text());

    // 预览那边两件事，顺序不能反：
    //   ① 告诉渲染器"要显示的是这段内容"（它会记住，等页面就绪后再推）
    //   ② 换 baseUrl 到文档所在目录 —— 文档里的相对图片靠它才找得到
    m_renderer.updateContent(m_files.text());
    m_renderer.loadTemplate(QFileInfo(path).absolutePath());

    // 标题栏的更新交给 fileOpened 信号（onFileOpened）
    return true;
}

void MainWindow::onNewFile()
{
    if (!maybeSave()) {
        return;  // 用户按了取消：什么都别做，不能把没保存的内容丢掉
    }

    m_files.newFile();    // 清空内容、丢掉路径、清掉脏标志、编码回到 UTF-8
    ui->editor->clear();  // 编辑器跟着清空

    // 预览也回到"没有文档"的状态：baseUrl 变回 about:blank，内容清空
    m_renderer.updateContent(m_files.text());
    m_renderer.loadTemplate(QString());
}

// 有未保存的修改时先问一句。返回 false = 用户取消，调用方必须中止当前操作。
bool MainWindow::maybeSave()
{
    if (!m_files.isModified()) {
        return true;
    }

    const QMessageBox::StandardButton answer =
        QMessageBox::warning(this,
                             QStringLiteral("有未保存的修改"),
                             QStringLiteral("「%1」有未保存的修改，要先保存吗？").arg(m_files.fileName()),
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                             QMessageBox::Save);

    if (answer == QMessageBox::Cancel) {
        return false;
    }
    if (answer == QMessageBox::Discard) {
        return true;
    }

    onSaveFile();                  // 新文档没有路径时，它自己会转去「另存为」
    return !m_files.isModified();  // 保存失败、或用户取消了另存为 → 别继续往下丢内容
}

void MainWindow::onOpenFile()
{
    if (!maybeSave()) {
        return;
    }

    const QString startDir = m_files.filePath().isEmpty()
                                 ? QDir::homePath()
                                 : QFileInfo(m_files.filePath()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("打开 Markdown 文件"),
        startDir,
        QStringLiteral("Markdown (*.md *.markdown *.txt);;所有文件 (*)"));
    if (!path.isEmpty()) {
        openFile(path);
    }
}

void MainWindow::onSaveFile()
{
    // 保证管理器里是最新内容（正常打字时 setText 已经同步过，这里是保险）
    m_files.setText(ui->editor->toPlainText());

    if (!m_files.hasFilePath()) {
        onSaveFileAs();  // 新文档还没有路径 → 走另存为
        return;
    }

    QString error;
    if (!m_files.saveFile(&error)) {
        // 失败原因由管理器给出（只读 / 没有写权限 / 文件被占用…）。
        // 注意：失败时脏标志仍然是 true，界面上的 * 不会被去掉。
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
    }
    // 成功的话，标题栏和状态栏由 fileSaved 信号更新
}

void MainWindow::onSaveFileAs()
{
    const QString startPath = m_files.filePath().isEmpty() ? QDir::homePath() : m_files.filePath();
    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("另存为"),
                                                      startPath,
                                                      QStringLiteral("Markdown (*.md);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }

    m_files.setText(ui->editor->toPlainText());

    QString error;
    if (!m_files.saveFileAs(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return;
    }

    // 换目录了：baseUrl 要跟着换，否则文档里的相对图片会指错地方
    m_renderer.loadTemplate(QFileInfo(path).absolutePath());
}

// ============================ 编辑器 → 预览 ============================

void MainWindow::onEditorTextChanged()
{
    // 内容真的变了才继续往下走：setText() 在"内容没变"时返回 false
    // （例如打开文件时 setPlainText 带来的那一次 textChanged，以及 newFile 之后的清空）
    if (!m_files.setText(ui->editor->toPlainText())) {
        return;
    }

    // 交给渲染管线。防抖（300ms）在 PreviewRenderer 里，主窗口不再自己管计时器：
    // 敲字时它会把这次更新一直往后推，停下来之后才真正渲染一次。
    m_renderer.updateContent(m_files.text());

    updateWindowTitle();
}

// ============================ 预览 → 编辑器 ============================

void MainWindow::onPreviewClicked(int line)
{
    QTextDocument *doc = ui->editor->document();
    if (doc->blockCount() <= 0) {
        return;
    }

    // 1 起算的行号 → QPlainTextEdit 的 blockNumber()（0 起算），并夹到合法范围，
    // 防止"预览的行号比编辑器的行数还大"时越界
    const int blockNumber = qBound(0, line - 1, doc->blockCount() - 1);

    QTextCursor cursor(doc->findBlockByNumber(blockNumber));
    ui->editor->setTextCursor(cursor);
    ui->editor->centerCursor();  // 让目标行落在屏幕中间，而不是贴着边
    ui->editor->setFocus();

    LOG_INFO("预览点击 → 编辑器跳到第 %1 行", line);
}

// ============================ 编辑器滚动 → 预览滚动 ============================

void MainWindow::onEditorScrolled()
{
    // 当前最顶可见行 = 视口左上角那个位置对应的文本块。
    // 注意：QPlainTextEdit::firstVisibleBlock() 是 protected 的，外部调不到，
    // 所以走 cursorForPosition()（它接受的是视口坐标）。
    const int blockNumber = ui->editor->cursorForPosition(QPoint(0, 0)).blockNumber();
    const int line = blockNumber + 1;  // blockNumber() 是 0 起算 → +1 变成"人类行号"

    if (m_bridge) {
        m_bridge->reportEditorScroll(line);  // → editorScrolled 信号 → JS scrollToLine()
    }
}

// ============================ 文件管理器的结论 → 界面 ============================
//
// FileManager 不弹任何对话框（那样它就没法在无窗口的环境里跑了），
// 只把结论和原因发出来；弹窗、状态栏、标题栏这些"界面表达"全在这里。

void MainWindow::onFileOpened(const QString &path)
{
    statusBar()->showMessage(QStringLiteral("已打开：%1（%2）")
                                 .arg(path, FileManager::encodingName(m_files.encoding())));
    updateWindowTitle();
    updateCacheStatus();  // 命中/未命中次数刚刚变了
}

void MainWindow::onFileSaved(const QString &path)
{
    statusBar()->showMessage(QStringLiteral("已保存：%1（%2）")
                                 .arg(path, FileManager::encodingName(m_files.encoding())));
    updateWindowTitle();
    updateCacheStatus();  // 保存后缓存里换成了新内容
}

void MainWindow::onModificationChanged(bool modified)
{
    // 标题栏的 * 就靠这里。注意：保存成功时先发 modificationChanged(false)、
    // 再发 fileSaved(path)，所以最后停在状态栏上的是"已保存"这条。
    updateWindowTitle();
    statusBar()->showMessage(modified ? QStringLiteral("有未保存的修改") : QStringLiteral("已保存到磁盘"));
}

void MainWindow::onReadOnlyDetected(const QString &path, const QString &reason)
{
    LOG_WARN("只读文件: %1", path);
    QMessageBox::warning(this, QStringLiteral("文件是只读的"), reason);
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
    if (!m_files.hasFilePath()) {
        QMessageBox::information(this,
                                 QStringLiteral("版本历史"),
                                 QStringLiteral("这个文档还没保存过。\n保存一次（Ctrl+S）就会留下第一份快照。"));
        return;
    }

    VersionControl *history = m_files.versionControl();
    const QString repoDir = history->repositoryPathFor(m_files.filePath());

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

    showTextDialog(QStringLiteral("版本历史 — %1").arg(m_files.fileName()),
                   QStringLiteral("共 %1 个快照（最新的在最上面）\n快照仓库：%2\n"
                                  "想用命令行看：cd 进上面这个目录，然后 git log / git diff")
                       .arg(commits.size())
                       .arg(repoDir),
                   lines.join(QLatin1Char('\n')));
}

void MainWindow::onDiffWithPrevious()
{
    if (!m_files.hasFilePath()) {
        QMessageBox::information(this,
                                 QStringLiteral("与上一版对比"),
                                 QStringLiteral("这个文档还没保存过，没有可对比的版本。"));
        return;
    }

    VersionControl *history = m_files.versionControl();
    const QString repoDir = history->repositoryPathFor(m_files.filePath());

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
        QStringLiteral("与上一版对比 — %1").arg(m_files.fileName()),
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
    if (!m_files.hasFilePath()) {
        QMessageBox::information(this,
                                 QStringLiteral("回滚到历史版本"),
                                 QStringLiteral("这个文档还没保存过，没有历史版本可以回滚。"));
        return;
    }

    VersionControl *history = m_files.versionControl();
    const QString repoDir = history->repositoryPathFor(m_files.filePath());

    QString error;
    const QList<VersionControl::Commit> commits = history->history(repoDir, 50, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("回滚到历史版本"), error);
        return;
    }
    if (commits.isEmpty()) {
        QMessageBox::information(this,
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
        QStringLiteral("回滚到历史版本 — %1").arg(m_files.fileName()),
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
    if (!m_files.restoreSnapshot(picked.hash, &restoreError)) {
        QMessageBox::warning(this, QStringLiteral("回滚失败"), restoreError);
        return;
    }

    // 内容同步到编辑器和预览。
    // 注意 setPlainText 会触发 textChanged → setText(同样的内容) → 返回 false，
    // 所以预览要显式推一次（和打开文件时同样的道理）。
    ui->editor->setPlainText(m_files.text());
    m_renderer.updateContent(m_files.text());
    updateWindowTitle();
    updateCacheStatus();

    statusBar()->showMessage(QStringLiteral("已回滚到 %1（内容尚未写盘，按 Ctrl+S 保存）").arg(picked.shortHash));
}

// ============================ 缓存（4.2.3）============================

void MainWindow::onClearCache()
{
    m_files.cacheManager()->clear();
    statusBar()->showMessage(QStringLiteral("已清空内存缓存（下次打开文件会重新读盘）"));
    updateCacheStatus();
}

void MainWindow::updateCacheStatus()
{
    if (m_cacheLabel == nullptr) {
        return;
    }

    const CacheManager *cache = m_files.cacheManager();
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

void MainWindow::onCursorMoved(int line, int column)
{
    if (m_cursorLabel != nullptr) {
        m_cursorLabel->setText(QStringLiteral("行 %1，列 %2").arg(line).arg(column));
    }
}

// ============================ 其它 ============================

void MainWindow::updateWindowTitle()
{
    setWindowTitle(QStringLiteral("%1%2 - Markdown 编辑器")
                       .arg(m_files.isModified() ? QStringLiteral("*") : QString(), m_files.fileName()));
}

// 关窗口：有未保存的修改就先问一句。
// 和「新建 / 打开」共用 maybeSave()，保证三处的行为完全一致 ——
// 少这一处的话，用户点右上角关闭就会把没保存的内容丢掉（原来就是这样）。
void MainWindow::closeEvent(QCloseEvent *event)
{
    if (maybeSave()) {
        event->accept();
    } else {
        event->ignore();  // 用户选了取消：窗口不关，继续编辑
    }
}
