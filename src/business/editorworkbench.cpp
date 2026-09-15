#include "editorworkbench.h"

#include "editorwidget.h"
#include "logger.h"
#include "previewrenderer.h"
#include "syncbridge.h"

#include <QScrollBar>
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
        if (m_editor->document()->blockCount() <= 0) {
            return;
        }

        // 跳行只有一份实现（在 EditorWidget 里）：夹范围、居中、拿焦点都在那儿。
        // 全文搜索的结果跳转走的是同一个方法，两条路的行为因此不可能不一致。
        m_editor->goToLine(line);  // 行号 1 起算，越界会被夹到合法范围

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

    // 初始比例**不在这里设**：工作台只认"编辑器侧"和"预览侧"两块，但 splitter 里可能还有第三块
    //（比如左边的文件树侧边栏）。只有调用方知道一共有几块、要按什么比例分，
    // 所以初始比例由调用方在 setup() 之后自己 setSizes()。
    // 这里只把当前比例记下来，供显示模式来回切换时恢复。
    m_savedSizes = sizes();

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

    // 切回"看得到预览"的模式：如果之前有大文档/隐藏期间欠下的内容，补推一次（7.2）
    if (mode != ViewMode::EditorOnly) {
        pushDeferredContent();
    }

    emit viewModeChanged(mode);
}

void EditorWorkbench::setSplitSizes(const QList<int> &sizes)
{
    if (sizes.size() != count() || sizes.isEmpty()) {
        return;  // 数目和 splitter 里的块数对不上：宁可不动，也不要把某一块压成 0 宽
    }
    setSizes(sizes);
    m_savedSizes = sizes;  // 记住它，"切到单栏再切回分屏"时恢复的就是这一份
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
    if (m_savedSizes.isEmpty() || m_savedSizes.size() != count()) {
        // 记录的比例和当前的块数对不上（比如 setup() 之后又插了一块控件）：
        // 什么都别动。瞎 setSizes 会把某一侧压成 0 宽，那比"比例没恢复"糟糕得多。
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
        disconnect(m_editor, &EditorWidget::fastModeChanged, this, nullptr);
    }

    m_editor = editor;

    if (m_editor != nullptr) {
        connect(m_editor->verticalScrollBar(), &QScrollBar::valueChanged, this, &EditorWorkbench::syncScrollToPreview);
        // 这个编辑器进出"大文档快速模式"时，预览的推送要跟着恢复/暂停（7.2）
        connect(m_editor, &EditorWidget::fastModeChanged, this, [this](bool) { pushDeferredContent(); });
        syncScrollToPreview();  // 刚切过来先对齐一次，不然预览还停在上一个文档的位置
    }

    // 换了编辑器：新文档可能不是大文档 —— 把之前欠下的内容补推一次（7.2）
    pushDeferredContent();
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
    // 先看看这次该不该推迟（预览不可见 / 大文档快速模式）—— 见头文件里的说明
    if (shouldDeferContent()) {
        m_hasPendingContent = true;
        m_pendingMarkdown = markdown;
        m_pendingBaseDir = baseDir;
        m_pendingForceReload = forceReload;
        return;
    }
    m_hasPendingContent = false;

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

bool EditorWorkbench::hasDeferredContent() const
{
    return m_hasPendingContent;
}

bool EditorWorkbench::shouldDeferContent() const
{
    // 情况 1：预览这一侧被明确隐藏了（仅编辑模式）。
    // 用 isHidden() 而不是 isVisible()：窗口还没显示出来时 isVisible() 也是 false，
    // 那会让启动阶段的内容推送全部被推迟（而且没人会去补推）。
    if (m_previewSide != nullptr && m_previewSide->isHidden()) {
        return true;
    }

    // 情况 2：当前编辑器是大文档（快速模式）—— 预览渲染同样很贵
    if (m_editor != nullptr && m_editor->isFastMode()) {
        return true;
    }

    return false;
}

void EditorWorkbench::pushDeferredContent()
{
    if (!m_hasPendingContent) {
        return;
    }

    // 条件可能还没解除（比如用户又切回"仅编辑"）：那就继续欠着
    if (shouldDeferContent()) {
        return;
    }

    const QString markdown = m_pendingMarkdown;
    const QString baseDir = m_pendingBaseDir;
    const bool forceReload = m_pendingForceReload;

    m_hasPendingContent = false;
    m_pendingMarkdown.clear();
    m_pendingBaseDir.clear();
    m_pendingForceReload = false;

    showContent(markdown, baseDir, forceReload);  // 这时候不会再被推迟了
}

QString EditorWorkbench::previewBaseDir() const
{
    return m_previewBaseDir;
}
