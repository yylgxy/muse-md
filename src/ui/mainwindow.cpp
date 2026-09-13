#include "mainwindow.h"

#include "ui_mainwindow.h"  // uic 根据 mainwindow.ui 生成（AUTOUIC 负责，不用手工写）

#include "logger.h"
#include "markdownhighlighter.h"
#include "syncbridge.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
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
#include <QWebChannel>
#include <QWebEnginePage>  // attach() 返回页面，交给 QWebChannel 当父对象（要完整类型才能转 QObject*）
#include <QWebEngineView>

using markdown_editor::core::document::PreviewRenderer;
using markdown_editor::core::document::SyncBridge;
using markdown_editor::core::storage::FileManager;

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

    // Tab 宽度按 4 个空格算（这属于运行期计算，.ui 里没法表达）
    ui->editor->setTabStopDistance(4 * ui->editor->fontMetrics().horizontalAdvance(QLatin1Char(' ')));

    // 语法高亮挂到编辑器的文档上（QSyntaxHighlighter 会自己接管重绘）
    m_highlighter = new MarkdownHighlighter(ui->editor->document());

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

    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(QStringLiteral("退出(&Q)"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
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
}

void MainWindow::onFileSaved(const QString &path)
{
    statusBar()->showMessage(QStringLiteral("已保存：%1（%2）")
                                 .arg(path, FileManager::encodingName(m_files.encoding())));
    updateWindowTitle();
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

// ============================ 其它 ============================

void MainWindow::updateWindowTitle()
{
    setWindowTitle(QStringLiteral("%1%2 - Markdown 编辑器")
                       .arg(m_files.isModified() ? QStringLiteral("*") : QString(), m_files.fileName()));
}
