#include "previewrenderer.h"

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

    // 页面就绪：把最近一次要求渲染的内容补推上去（模板刚换、或加载期间来的编辑都在这里兑现）
    pushNow();
    emit pageReadyChanged(true);
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

    const QString html = MarkdownParser::parseToHtml(m_desired);
    const QList<int> lineMap = SyncBridge::buildLineMap(m_desired);

    m_page->runJavaScript(buildApplyScript(html, lineMap));

    emit contentRendered();
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
