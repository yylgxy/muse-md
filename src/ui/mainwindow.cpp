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

    // 先把预览外壳页面装起来（内容等页面加载完、渲染器自己补推）
    m_renderer.loadTemplate(QString());
}

// 菜单/工具栏/动作：这些用 .ui 表达不了 ——
// 快捷键（QKeySequence::Open）、动作对象、triggered 连接都是 C++ 的事，
// 所以这一整块和"中央布局用不用 .ui"无关，永远是代码。
void MainWindow::initMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

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
    if (!m_document.loadFromFile(path)) {
        QMessageBox::warning(this,
                             QStringLiteral("打开失败"),
                             QStringLiteral("打不开这个文件，具体原因见日志：\n%1").arg(path));
        return false;
    }

    // 编辑器显示文档内容。setPlainText 会触发 textChanged → onEditorTextChanged
    // → m_document.setMarkdownText(同样的内容) → 内容没变，所以不会置脏 ✓
    ui->editor->setPlainText(m_document.getMarkdownText());
    m_document.setModified(false);

    // baseUrl 换成文档所在目录：预览里的相对路径图片靠它才能找到文件。
    // 换模板会重新加载页面，加载完成后渲染器会把当前内容补推上去。
    m_renderer.loadTemplate(QFileInfo(path).absolutePath());
    updateWindowTitle();

    LOG_INFO("已打开文档: %1", path);
    return true;
}

void MainWindow::onOpenFile()
{
    const QString startDir = m_document.getFilePath().isEmpty()
                                 ? QDir::homePath()
                                 : QFileInfo(m_document.getFilePath()).absolutePath();
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
    m_document.setMarkdownText(ui->editor->toPlainText());  // 保证模型里是最新内容

    if (m_document.getFilePath().isEmpty()) {
        onSaveFileAs();  // 新文档还没有路径 → 走另存为
        return;
    }
    if (!m_document.save()) {
        QMessageBox::warning(this,
                             QStringLiteral("保存失败"),
                             QStringLiteral("保存失败，具体原因见日志：\n%1").arg(m_document.getFilePath()));
    }
    updateWindowTitle();
}

void MainWindow::onSaveFileAs()
{
    const QString startPath =
        m_document.getFilePath().isEmpty() ? QDir::homePath() : m_document.getFilePath();
    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("另存为"),
                                                      startPath,
                                                      QStringLiteral("Markdown (*.md);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }

    m_document.setMarkdownText(ui->editor->toPlainText());
    if (!m_document.saveToFile(path)) {
        QMessageBox::warning(this,
                             QStringLiteral("保存失败"),
                             QStringLiteral("保存失败，具体原因见日志：\n%1").arg(path));
        return;
    }

    // 换目录了：baseUrl 要跟着换，否则文档里的相对图片会指错地方
    m_renderer.loadTemplate(QFileInfo(path).absolutePath());
    updateWindowTitle();
}

// ============================ 编辑器 → 预览 ============================

void MainWindow::onEditorTextChanged()
{
    // 把编辑器内容同步进文档模型（置脏 + 让 HTML 缓存失效）
    m_document.setMarkdownText(ui->editor->toPlainText());

    // 交给渲染管线。防抖（300ms）在 PreviewRenderer 里，主窗口不再自己管计时器：
    // 敲字时它会把这次更新一直往后推，停下来之后才真正渲染一次。
    m_renderer.updateContent(m_document.getMarkdownText());

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

// ============================ 其它 ============================

void MainWindow::updateWindowTitle()
{
    const QString name = m_document.getFilePath().isEmpty()
                             ? QStringLiteral("未命名")
                             : QFileInfo(m_document.getFilePath()).fileName();
    setWindowTitle(QStringLiteral("%1%2 - Markdown 编辑器")
                       .arg(m_document.isModified() ? QStringLiteral("*") : QString(), name));
}
