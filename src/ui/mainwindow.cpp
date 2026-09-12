#include "mainwindow.h"

#include "ui_mainwindow.h"  // uic 根据 mainwindow.ui 生成（AUTOUIC 负责，不用手工写）

#include "logger.h"
#include "markdownhighlighter.h"
#include "syncbridge.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
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
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVariantList>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineView>

using markdown_editor::core::document::SyncBridge;

namespace {

// 预览刷新的防抖时间（毫秒）。太短会在打字时不断重排、很卡；太长会觉得预览"跟不上"。
constexpr int kPreviewDebounceMs = 250;

// 预览页里 console.log/warn/error 的输出默认没人看得到。
// 继承 QWebEnginePage 覆写这个虚函数，把 JS 的日志接到我们自己的 Logger 上 ——
// 调双向同步时（比如 data-line 数量对不上）这条"从网页里传出来的声音"特别值钱。
class JsLoggingPage : public QWebEnginePage
{
public:
    explicit JsLoggingPage(QObject *parent) : QWebEnginePage(parent) {}

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString &message,
                                  int lineNumber,
                                  const QString &sourceId) override
    {
        switch (level) {
        case InfoMessageLevel:
            LOG_INFO("[JS] %1", message);
            break;
        case WarningMessageLevel:
            LOG_WARN("[JS] %1 (行 %2)", message, lineNumber);
            break;
        case ErrorMessageLevel:
            LOG_ERROR("[JS] %1 (%2:%3)", message, sourceId, lineNumber);
            break;
        }
    }
};

}  // namespace

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

    // 换一个 QWebEnginePage 子类：把网页里的 console 输出转发到我们的日志
    ui->preview->setPage(new JsLoggingPage(ui->preview));

    // 左右各占一半（splitter 的初始比例是运行期设置，.ui 里表达不了）
    ui->splitter->setStretchFactor(0, 1);
    ui->splitter->setStretchFactor(1, 1);
    ui->splitter->setSizes({600, 600});

    // ---- WebChannel：把 C++ 的同步桥暴露给页面里的 JS ----
    // 名字 "syncBridge" 必须和模板里 channel.objects.syncBridge 完全一致
    m_bridge = new SyncBridge(this);
    auto *channel = new QWebChannel(ui->preview->page());
    channel->registerObject(QStringLiteral("syncBridge"), m_bridge);
    ui->preview->page()->setWebChannel(channel);

    // ---- 信号槽接线 ----
    connect(m_bridge, &SyncBridge::previewClicked, this, &MainWindow::onPreviewClicked);
    connect(ui->editor, &QPlainTextEdit::textChanged, this, &MainWindow::onEditorTextChanged);
    connect(ui->editor->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::onEditorScrolled);
    connect(ui->preview, &QWebEngineView::loadFinished, this, &MainWindow::onPreviewLoadFinished);

    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(kPreviewDebounceMs);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::refreshPreview);

    // 先把预览外壳页面装起来（内容等 loadFinished 之后再推）
    loadPreviewPage(QString());
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

    // baseUrl 换成文档所在目录：预览里的相对路径图片靠它才能找到文件
    loadPreviewPage(QFileInfo(path).absolutePath());
    if (m_previewReady) {
        pushContentToPreview();
    }
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
    loadPreviewPage(QFileInfo(path).absolutePath());
    if (m_previewReady) {
        pushContentToPreview();
    }
    updateWindowTitle();
}

// ============================ 编辑器 → 预览 ============================

void MainWindow::onEditorTextChanged()
{
    // 把编辑器内容同步进文档模型（置脏 + 让 HTML 缓存失效）
    m_document.setMarkdownText(ui->editor->toPlainText());

    // 防抖：连续敲字时，只在停下来之后渲染一次
    if (m_previewTimer) {
        m_previewTimer->start();
    }
    updateWindowTitle();
}

void MainWindow::refreshPreview()
{
    pushContentToPreview();
}

void MainWindow::loadPreviewPage(const QString &baseDir)
{
    QFile file(QStringLiteral(":/html/preview_template.html"));
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_ERROR("预览模板打不开: :/html/preview_template.html（检查 resources.qrc 是否接进目标）");
        return;
    }

    m_previewReady = false;  // 页面要重新加载，等 loadFinished 再推内容

    QUrl baseUrl;
    if (baseDir.isEmpty()) {
        baseUrl = QUrl(QStringLiteral("about:blank"));
    } else {
        const QString dir = baseDir.endsWith(QLatin1Char('/')) ? baseDir : baseDir + QLatin1Char('/');
        baseUrl = QUrl::fromLocalFile(dir);
    }
    ui->preview->setHtml(QString::fromUtf8(file.readAll()), baseUrl);
}

void MainWindow::onPreviewLoadFinished(bool ok)
{
    m_previewReady = ok;
    if (!ok) {
        LOG_ERROR("预览页面加载失败");
        return;
    }
    pushContentToPreview();
}

void MainWindow::pushContentToPreview()
{
    if (!m_previewReady) {
        return;  // 页面还没就绪（JS 里的 bridge 还没连上），等 loadFinished
    }

    const QString html = m_document.getRenderedHtml();
    const QList<int> lineMap = SyncBridge::buildLineMap(m_document.getMarkdownText());

    // 用 JSON 把参数"打包"成安全的 JS 字面量：
    // 直接拼字符串的话，HTML 里的引号、换行、反斜杠都会把 JS 语法弄坏。
    QVariantList lineVariants;
    for (int line : lineMap) {
        lineVariants.append(line);
    }
    QJsonArray args;
    args.append(html);
    args.append(QJsonArray::fromVariantList(lineVariants));

    const QString script = QStringLiteral("applyContent.apply(null, %1);")
                               .arg(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)));
    ui->preview->page()->runJavaScript(script);

    // 内容重排后预览会回到顶部，这里顺手跟编辑器的当前顶行对齐
    onEditorScrolled();
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
