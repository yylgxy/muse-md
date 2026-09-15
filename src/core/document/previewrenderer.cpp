#include "previewrenderer.h"

#include "codehighlighter.h"
#include "logger.h"
#include "markdownparser.h"
#include "syncbridge.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantList>
#include <QWebEnginePage>
#include <QWebEngineView>

namespace markdown_editor::core::document {

namespace {

// 预览页里 console.log/warn/error 的输出默认没人看得到（浏览器控制台不在我们的窗口里）。
// 继承 QWebEnginePage 覆写这个虚函数，把 JS 的日志接到我们自己的 Logger 上 ——
// 调双向同步时（比如 data-line 数量对不上）这条"从网页里传出来的声音"特别值钱。
//
// 为什么放在本文件里而不是 ui 层：页面已经由渲染器持有，让"持有页面的人"顺便接日志，
// 主窗口就不必再认识 QWebEnginePage 的子类了。
class ConsoleLoggingPage : public QWebEnginePage
{
public:
    explicit ConsoleLoggingPage(QObject *parent) : QWebEnginePage(parent) {}

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

PreviewRenderer::PreviewRenderer(QObject *parent) : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDefaultDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &PreviewRenderer::pushNow);
}

PreviewRenderer::~PreviewRenderer() = default;

// ============================ 附着与模板 ============================

QWebEnginePage *PreviewRenderer::attach(QWebEngineView *view)
{
    if (!view) {
        LOG_WARN("PreviewRenderer::attach 收到空 view：渲染器保持未附着，后续调用都是空操作");
        return nullptr;
    }
    if (m_view == view && m_page) {
        return m_page;  // 同一个 view 重复附着 = 幂等
    }

    if (m_view) {
        // 换 view 了：跟旧 view 的连接要断掉，否则旧页面的 loadFinished 还会推到我们这里
        disconnect(m_view, nullptr, this, nullptr);
    }

    m_view = view;

    // 页面挂在 view 名下：QWebEngineView::setPage() 不接管所有权（父对象才是 owner），
    // 所以传 view 当 parent —— view 被销毁时页面自动回收，我们不需要手工 delete。
    auto *page = new ConsoleLoggingPage(view);
    view->setPage(page);
    m_page = page;

    // loadFinished 是"模板里的 JS 已经就绪"的信号，之后推内容才安全
    connect(view, &QWebEngineView::loadFinished, this, &PreviewRenderer::onLoadFinished);

    // ★ 渲染进程死了要能自己爬起来。
    // 为什么必须连这个信号：渲染进程被系统杀掉之后，**loadFinished 不会来**
    //（页面既没加载成功也没失败，就停在那儿），于是预览永远是白的、日志里连条错误都没有 ——
    // 这正是"以前预览好好的，打开一个大文件之后预览就再也不显示了，只能重启程序"的成因。
    // 连上它之后：重载模板 → Chromium 会为页面拉起新的渲染进程 → loadFinished 补推内容。
    connect(page, &QWebEnginePage::renderProcessTerminated, this, [this](QWebEnginePage::RenderProcessTerminationStatus status, int exitCode) {
        onRenderProcessTerminated(int(status), exitCode);
    });

    m_ready = false;
    return page;
}

QWebEngineView *PreviewRenderer::webView() const
{
    return m_view;
}

QWebEnginePage *PreviewRenderer::page() const
{
    return m_page;
}

bool PreviewRenderer::loadTemplate(const QString &baseDir, QString *error)
{
    // 失败统一从这里出去：写日志 + 填 *error（若给了）+ 返回 false
    const auto fail = [error](const QString &why) {
        if (error) {
            *error = why;
        }
        LOG_ERROR("%1", why);
        return false;
    };

    if (m_page.isNull() || m_view.isNull()) {
        return fail(QStringLiteral("预览视图还没附着（先调用 attach()），无法载入模板"));
    }

    QFile file(QStringLiteral(":/html/preview_template.html"));
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral(
            "预览模板打不开: :/html/preview_template.html（检查 resources.qrc 是否接进目标）"));
    }

    m_ready = false;  // 页面要重新加载，等 loadFinished 之后才能推内容
    emit pageReadyChanged(false);

    // 记住这次用的目录：渲染进程崩了要照原样重载（见 onRenderProcessTerminated）
    m_currentBaseDir = baseDir;

    m_view->setHtml(QString::fromUtf8(file.readAll()), baseUrlFromDir(baseDir));
    return true;
}

void PreviewRenderer::onLoadFinished(bool ok)
{
    m_ready = ok;

    if (!ok) {
        LOG_ERROR("预览页面加载失败");
        emit pageReadyChanged(false);
        return;
    }

    m_rendererRestarts = 0;  // 页面真的起来了：崩溃计数清零（下次崩了还能继续自动恢复）

    // 页面就绪：把最近一次要求渲染的内容补推上去（模板刚换、或加载期间来的编辑都在这里兑现）
    pushNow();
    emit pageReadyChanged(true);
}

void PreviewRenderer::onRenderProcessTerminated(int status, int exitCode)
{
    m_ready = false;
    emit pageReadyChanged(false);

    if (m_view.isNull() || m_page.isNull()) {
        return;  // 已经没视图可恢复了（未附着或正在销毁），什么都不做
    }

    if (m_rendererRestarts >= kMaxRendererRestarts) {
        // 反复崩：多半不是内容的问题（那是另一路：内容超限时根本不会推给页面），
        // 再重载只会变成"崩→重载→再崩"的空转，所以停下来并说清楚。
        LOG_ERROR("预览的渲染进程反复结束（已自动恢复 %1 次），停止自动恢复", m_rendererRestarts);
        emit contentSkipped(QStringLiteral("预览反复崩溃（已尝试自动恢复 %1 次，最后一次状态 %2 / 退出码 %3），"
                                           "已停止自动恢复；建议重启程序，并看一下日志")
                                .arg(m_rendererRestarts)
                                .arg(status)
                                .arg(exitCode));
        return;
    }

    ++m_rendererRestarts;
    LOG_WARN("预览的渲染进程结束了（状态 %1，退出码 %2），正在自动恢复（第 %3/%4 次）",
             status,
             exitCode,
             m_rendererRestarts,
             kMaxRendererRestarts);
    emit rendererRestarted(m_rendererRestarts);

    // 重新载模板 = 让 Chromium 拉起新的渲染进程；加载完成后的 loadFinished 会把
    // m_desired 里的内容补推上去（内容超限时 pushNow 推的是提示，所以不会又崩一次）。
    QString error;
    if (!loadTemplate(m_currentBaseDir, &error)) {
        LOG_ERROR("自动恢复预览失败：%1", error);
    }
}

// ============================ 内容入口与防抖 ============================

void PreviewRenderer::updateContent(const QString &markdown)
{
    m_desired = markdown;

    if (m_debounce.interval() <= 0) {
        pushNow();  // 防抖被关掉了：每次调用都立刻渲染
        return;
    }

    // 重启计时器 = 防抖。连续敲字时每次调用都把它推后 300ms，
    // 所以只有"停下来之后"的那一次能到期并渲染。
    m_debounce.start();
}

void PreviewRenderer::updateContentNow(const QString &markdown)
{
    m_desired = markdown;
    m_debounce.stop();  // 取消还在等的那一次，免得同一份内容被渲染两遍
    pushNow();
}

void PreviewRenderer::flush()
{
    if (!m_debounce.isActive()) {
        return;  // 没有待处理内容
    }
    m_debounce.stop();
    pushNow();
}

void PreviewRenderer::setDebounceInterval(int ms)
{
    m_debounce.setInterval(ms);
}

int PreviewRenderer::debounceInterval() const
{
    return m_debounce.interval();
}

bool PreviewRenderer::isPageReady() const
{
    return m_ready;
}

bool PreviewRenderer::hasPendingUpdate() const
{
    return m_debounce.isActive();
}

// ============================ 渲染与推送 ============================

void PreviewRenderer::pushNow()
{
    if (!m_ready || m_page.isNull()) {
        // 页面（和它里面的 JS）还没就绪。内容不丢：留在 m_desired 里，
        // 等 loadFinished 触发的 pushNow() 再送出去。
        return;
    }

    // ---- 内容上限：超限不硬推，改成推一段"内容太大"的说明 ----
    // 这一条是"预览永远不会被内容拖死"的保证：几 MB 的文本走 md4c → HTML → 几 MB 的
    // JS 字符串塞给 Chromium，会把这个渲染进程拖到卡死甚至被杀，而杀了之后预览就再也不恢复。
    // 现在超限时页面收到的是一条很短、很稳的说明，用户一眼就知道为什么没内容。
    if (exceedsContentLimit(m_desired)) {
        m_page->runJavaScript(buildApplyScript(contentTooLargeHtml(m_desired), QList<int>()));
        LOG_WARN("内容超过预览上限：%1 个字符（上限 %2），预览改为显示提示", m_desired.size(), kMaxContentChars);
        emit contentSkipped(QStringLiteral("内容太大（%1 个字符，超过预览上限 %2），预览已暂停")
                                .arg(m_desired.size())
                                .arg(kMaxContentChars));
        emit contentRendered();  // 页面确实换过内容了：让界面重新对齐滚动位置
        return;
    }

    // 渲染 → 代码高亮：把 HTML 里 <pre><code class="language-x"> 的内容换成带 span 的高亮版本。
    // ★ 高亮放在 C++ 里而不是页面里的 JS，是为了让预览、导出的 HTML、导出的 PDF
    //   三条路共用同一份结果（JS 方案得内联脚本，PDF 还得赌"打印时脚本已经跑完"）。
    const QString html = CodeHighlighter::highlightCodeBlocks(MarkdownParser::parseToHtml(m_desired));
    const QList<int> lineMap = SyncBridge::buildLineMap(m_desired);

    m_page->runJavaScript(buildApplyScript(html, lineMap));

    emit contentRendered();
}

// ============================ 内容上限（纯规则）============================

bool PreviewRenderer::exceedsContentLimit(const QString &markdown)
{
    return markdown.size() > kMaxContentChars;
}

QString PreviewRenderer::contentTooLargeHtml(const QString &markdown)
{
    const int chars = markdown.size();
    // 大小按"人眼能比"的单位说：百万级说兆字符，其余说万字符
    const QString size = (chars >= 1000000)
                             ? QStringLiteral("%1 兆字符").arg(double(chars) / 1000000.0, 0, 'f', 1)
                             : QStringLiteral("%1 万字符").arg(double(chars) / 10000.0, 0, 'f', 1);

    return QStringLiteral(
               "<div style=\"margin:2rem auto;max-width:32rem;padding:1rem 1.25rem;"
               "border-left:4px solid #d0a000;background:#fffbe6;color:#5a4600;"
               "font-family:system-ui,-apple-system,'Segoe UI','Microsoft YaHei',sans-serif;"
               "line-height:1.7;border-radius:4px;\">"
               "<div style=\"font-weight:600;margin-bottom:.5rem;\">内容太大，预览已暂停</div>"
               "<div>这份文档有 %1（%2 个字符），超过预览上限 %3 个字符。</div>"
               "<div>编辑器里照样可以正常查看、编辑和保存；换到小一点的文档，预览会自动恢复。</div>"
               "<div style=\"margin-top:.5rem;color:#8a6d00;\">"
               "（提示：如果你是在文件树里双击打开的，它可能是一个二进制文件，并不是 Markdown 文本。）"
               "</div></div>")
        .arg(size)
        .arg(chars)
        .arg(kMaxContentChars);
}

int PreviewRenderer::rendererRestartCount() const
{
    return m_rendererRestarts;
}

QString PreviewRenderer::buildApplyScript(const QString &html, const QList<int> &lineMap)
{
    QVariantList lineVariants;
    lineVariants.reserve(lineMap.size());
    for (int line : lineMap) {
        lineVariants.append(line);
    }

    // 一个数组装两个参数：HTML 字符串 + 行号数组
    QJsonArray args;
    args.append(html);
    args.append(QJsonArray::fromVariantList(lineVariants));

    // QJsonDocument 负责转义：引号、换行、反斜杠、控制字符都会变成合法的 JS 字面量，
    // 于是"网页内容"永远无法把这一行脚本改写成别的代码。
    return QStringLiteral("applyContent.apply(null, %1);")
        .arg(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)));
}

QUrl PreviewRenderer::baseUrlFromDir(const QString &baseDir)
{
    if (baseDir.isEmpty()) {
        return QUrl(QStringLiteral("about:blank"));
    }

    // 结尾必须有 '/'：否则相对路径 "a.png" 会被解析成"上一级目录里的 a.png"
    const QString dir = baseDir.endsWith(QLatin1Char('/')) ? baseDir : baseDir + QLatin1Char('/');
    return QUrl::fromLocalFile(dir);
}

}  // namespace markdown_editor::core::document
