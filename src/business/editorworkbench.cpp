#include "editorworkbench.h"

#include "editorwidget.h"
#include "logger.h"
#include "previewrenderer.h"
#include "syncbridge.h"

#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QtGlobal>

using markdown_editor::core::document::PreviewRenderer;
using markdown_editor::core::document::SyncBridge;

EditorWorkbench::EditorWorkbench(QWidget *parent) : QSplitter(parent)
{
    setOrientation(Qt::Horizontal);
    // 分隔条两侧都不许拖成 0 宽：想只看一边请用显示模式，不要靠"把另一边拖没"
    setChildrenCollapsible(false);

    // 渲染管线和同步桥都由工作台持有：它们是"编辑器 + 预览"这对东西的内部机制，
    // 主窗口不必知道（它只管文件和标签）。父对象设成 this，生命周期自动跟着走。
    m_renderer = new PreviewRenderer(this);
    m_bridge = new SyncBridge(this);

    // ---- 预览 → 编辑器：点预览里的某一块，光标跳到对应源码行 ----
    connect(m_bridge, &SyncBridge::previewClicked, this, [this](int line) {
        if (m_editor == nullptr) {
            return;
        }
        QTextDocument *doc = m_editor->document();
        if (doc->blockCount() <= 0) {
            return;
        }

        // 1 起算的行号 → blockNumber()（0 起算），并夹到合法范围，
        // 防止"预览的行号比编辑器行数还大"时越界
        const int blockNumber = qBound(0, line - 1, doc->blockCount() - 1);
        QTextCursor cursor(doc->findBlockByNumber(blockNumber));
        m_editor->setTextCursor(cursor);
        m_editor->centerCursor();  // 让目标行落在屏幕中间，而不是贴着边
        m_editor->setFocus();

        emit editorLineClicked(line);
    });

    // ---- 内容刚推给页面 → 页面里的块是新的，滚动位置要重新对齐一次 ----
    connect(m_renderer, &PreviewRenderer::contentRendered, this, &EditorWorkbench::syncScrollToPreview);
}

bool EditorWorkbench::setup(QWidget *editorSide, QWidget *previewSide)
{
    if (editorSide == nullptr || previewSide == nullptr) {
        return false;  // 缺任何一边都不算搭好，调用方应该先修好再进来
    }

    m_editorSide = editorSide;
    m_previewSide = previewSide;

    // 两块控件必须真的是本 splitter 的子控件。
    // 在 .ui 场景下 uic 已经用 addWidget 摆好了（父对象就是 splitter），这里是空操作；
    // 而代码里手工传进来的控件（比如测试里造的两个裸控件）需要在这里被"收编"，
    // 否则它们根本不属于这个分屏，sizes()/隐藏逻辑都会失去意义。
    const auto adopt = [this](QWidget *side) {
        if (side != nullptr && side->parentWidget() != this) {
            addWidget(side);  // 会把父对象改成本 splitter，并排到末尾
        }
    };
    adopt(editorSide);
    adopt(previewSide);

    // 让当前模式立刻生效（默认是左右分屏）。
    // 少了这一步，传进来的控件如果本来是隐藏的，就要等用户第一次切模式才会出现。
    applyViewMode(m_mode);

    // 初始比例：左右各一半（原来这段在 .ui 的 QSplitter 上，现在归工作台管）
    m_savedSizes = {1, 1};
    setSizes({600, 600});

    // 预览侧如果真的是 QWebEngineView，就把渲染管线和 WebChannel 接上。
    // 用 qobject_cast 而不是写死类型：这样测试可以传一个普通 QWidget，
    // 分屏/模式/同步桥的接线照样能测，不需要 Chromium 运行时。
    m_previewView = qobject_cast<QWebEngineView *>(previewSide);
    if (m_previewView != nullptr) {
        // attach() 会把 view 的页面换成"能转发 JS console 日志"的那种
        QWebEnginePage *page = m_renderer->attach(m_previewView);

        // 把同步桥暴露给页面里的 JS：名字必须和模板里 channel.objects.syncBridge 一致
        auto *channel = new QWebChannel(page);
        channel->registerObject(QStringLiteral("syncBridge"), m_bridge);
        page->setWebChannel(channel);

        // 先把外壳页面装起来（内容等页面加载完、渲染器自己补推）
        m_renderer->loadTemplate(QString());
        LOG_INFO("工作台已接上预览视图");
    }

    return true;
}

QWidget *EditorWorkbench::editorSide() const
{
    return m_editorSide;
}

QWidget *EditorWorkbench::previewSide() const
{
    return m_previewSide;
}

QWebEngineView *EditorWorkbench::previewView() const
{
    return m_previewView;
}

PreviewRenderer *EditorWorkbench::renderer()
{
    return m_renderer;
}

SyncBridge *EditorWorkbench::bridge()
{
    return m_bridge;
}

// ============================ 三种显示模式 ============================

EditorWorkbench::ViewMode EditorWorkbench::viewMode() const
{
    return m_mode;
}

void EditorWorkbench::setViewMode(ViewMode mode)
{
    if (mode == m_mode) {
        return;  // 没变化就不折腾界面，也不发信号
    }

    // 从分屏切走之前先记住比例；从单栏切回分屏时再恢复。
    // 少了这一步，切一次"仅编辑"再切回来，两边就会变成一边倒的怪比例。
    if (m_mode == ViewMode::Split && mode != ViewMode::Split) {
        rememberSplitSizes();
    }

    m_mode = mode;
    applyViewMode(mode);

    if (mode == ViewMode::Split) {
        restoreSplitSizes();
    }

    emit viewModeChanged(mode);
}

void EditorWorkbench::applyViewMode(ViewMode mode)
{
    // 直接操作"哪一块可见"，不依赖它们在 splitter 里的先后顺序
    const bool showEditor = (mode != ViewMode::PreviewOnly);
    const bool showPreview = (mode != ViewMode::EditorOnly);

    if (m_editorSide != nullptr) {
        m_editorSide->setVisible(showEditor);
    }
    if (m_previewSide != nullptr) {
        m_previewSide->setVisible(showPreview);
    }
}

void EditorWorkbench::rememberSplitSizes()
{
    const QList<int> current = sizes();
    // 只有两侧都还占着地方时才值得记（否则记下来的是"隐藏那侧宽度为 0"）
    int positive = 0;
    for (int size : current) {
        if (size > 0) {
            ++positive;
        }
    }
    if (positive == current.size() && !current.isEmpty()) {
        m_savedSizes = current;
    }
}

void EditorWorkbench::restoreSplitSizes()
{
    if (m_savedSizes.size() != count()) {
        setSizes({600, 600});  // 结构不对（比如根本没 setup）时退回默认比例
        return;
    }
    setSizes(m_savedSizes);
}

// ============================ 同步 ============================

void EditorWorkbench::setCurrentEditor(EditorWidget *editor)
{
    if (m_editor == editor) {
        return;
    }

    // 断开上一个编辑器：不然后台标签滚动也会把预览带走
    if (m_editor != nullptr) {
        disconnect(m_editor->verticalScrollBar(), nullptr, this, nullptr);
    }

    m_editor = editor;

    if (m_editor != nullptr) {
        connect(m_editor->verticalScrollBar(), &QScrollBar::valueChanged, this, &EditorWorkbench::syncScrollToPreview);
        syncScrollToPreview();  // 刚切过来先对齐一次，不然预览还停在上一个文档的位置
    }
}

EditorWidget *EditorWorkbench::currentEditor() const
{
    return m_editor;
}

void EditorWorkbench::syncScrollToPreview()
{
    if (m_editor == nullptr || m_bridge == nullptr) {
        return;
    }

    // 当前最顶可见行 = 视口左上角对应的文本块。
    // 注意：QPlainTextEdit::firstVisibleBlock() 是 protected 的，外部调不到，
    // 所以走 cursorForPosition()（它接受的是视口坐标）。
    const int blockNumber = m_editor->cursorForPosition(QPoint(0, 0)).blockNumber();
    m_bridge->reportEditorScroll(blockNumber + 1);  // +1：换成 1 起算的人类行号
}

void EditorWorkbench::showContent(const QString &markdown, const QString &baseDir, bool forceReload)
{
    const bool dirChanged = (baseDir != m_previewBaseDir);

    if (forceReload || dirChanged) {
        // 文档目录变了：必须重新加载模板，baseUrl 跟着换
        //（文档里的相对路径图片靠它才找得到）
        m_previewBaseDir = baseDir;
        m_renderer->updateContent(markdown);
        if (m_previewView != nullptr) {
            m_renderer->loadTemplate(baseDir);
        }
        return;
    }

    // 只是换了个标签/文档但目录没变：只推内容，页面不重载。
    // 重载页面会闪一下白屏、还会让 WebChannel 重连，切标签时手感很差。
    m_renderer->updateContentNow(markdown);
}

QString EditorWorkbench::previewBaseDir() const
{
    return m_previewBaseDir;
}
