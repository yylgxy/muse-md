#ifndef PREVIEWRENDERER_H
#define PREVIEWRENDERER_H

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

class QWebEnginePage;
class QWebEngineView;

namespace markdown_editor::core::document {

// 渲染管线服务：把「Markdown 源码」变成「预览页面上看得见的内容」。
//
// 这一段管线原本长在 MainWindow 里，问题是主窗口同时管着菜单、工具栏、文件对话框、
// 语法高亮、同步桥、防抖计时器和 JS 调用，想看清楚"预览到底怎么更新的"得在一千行里翻。
// 现在把它收拢成一个类，管线就只有四步：
//
//     updateContent(markdown)                    ← 唯一入口（内建防抖，打完字才渲染）
//          ↓  （防抖到期）
//     MarkdownParser::parseToHtml(markdown)      ← 解析成 HTML 片段
//     SyncBridge::buildLineMap(markdown)         ← 每个顶层块对应的源码行号（点击跳转要用）
//          ↓  JSON 打包成一行 JS
//     page->runJavaScript("applyContent.apply(null, [...])")   ← 交给页面里的 JS 替换内容
//
// 谁负责什么（这层封装想划清的界限）：
//   * 本类负责：模板加载、页面（QWebEnginePage）的持有与生命周期、防抖、渲染、推送
//   * 本类**不**负责：WebChannel 注册（那是 MainWindow 的事，桥的对象在那边）、
//                     滚动位置对齐（预览重排后要不要跟着编辑器滚，是窗口的决策）
//   * 本类**不**碰 QPlainTextEdit：它只知道"给我一段 Markdown"，不知道内容从哪来
//
// 渲染用 MarkdownParser（和别处同一个渲染服务），不读 MarkdownDocument 里的 HTML 缓存：
// 本类的契约只是"给我一段 Markdown"，它不需要认识文档模型。
// 代价是 MarkdownDocument::getRenderedHtml() 现在应用里没人调用了（预览不再走它）——
// 那份缓存留着给别的消费者（比如将来的"导出 HTML"），测试也还在覆盖它。
//
// 关于开销与 WebEngine 依赖：本类会构造 QWebEnginePage，所以链接它的人等于依赖
// Qt6::WebEngineWidgets（core_document 已经 PUBLIC 链接了）。为此把两个纯函数
// （buildApplyScript / baseUrlFromDir）抽成 static，让它们能脱离 WebEngine 单独测试。
class PreviewRenderer : public QObject
{
    Q_OBJECT

public:
    // 默认防抖时间（毫秒）：停止输入 300ms 后才真正渲染一次。
    // 太短会在打字时不停重排、明显卡顿；太长会觉得预览"跟不上手"。
    static constexpr int kDefaultDebounceMs = 300;

    explicit PreviewRenderer(QObject *parent = nullptr);
    ~PreviewRenderer() override;

    // 接管一个 QWebEngineView（主窗口里 .ui 的 preview），并给它换上一个
    // "把网页 console 输出转发到 Logger"的页面。返回新页面，方便调用方
    // 接着给 QWebChannel 用（channel 需要一个 QObject 当父对象）。
    //
    // 成功：返回新页面，此后 isPageReady() 在页面加载完成前一直是 false。
    // view 为空：返回 nullptr，本对象进入"未附着"状态 ——
    //           其余方法全部是安全空操作（不崩、不抛），适合防御性调用。
    // 重复用同一个 view 调用：幂等，直接返回已有的页面。
    QWebEnginePage *attach(QWebEngineView *view);

    QWebEngineView *webView() const;  // 未附着时返回 nullptr
    QWebEnginePage *page() const;     // 未附着时返回 nullptr

    // 载入预览"外壳"模板（qrc 里的 :/html/preview_template.html）并**重新建立页面**。
    // baseDir 是当前文档所在目录：文档里 ![](img/a.png) 这种相对路径要靠它当基准
    // 才能找到磁盘上的文件；传空字符串表示"没有文档路径"，用 about:blank 兜底。
    //
    // 成功：返回 true。注意页面是**异步**加载的，返回时 isPageReady() 仍为 false，
    //       要等 loadFinished 之后（pageReadyChanged(true)）才算真的能推内容。
    //       期间到来的 updateContent 不会丢：内容会记在"待渲染"里，就绪后补推。
    // 失败：返回 false。唯一现实原因就是模板资源打不开（resources.qrc 没接进目标），
    //       此时已经写了一条 LOG_ERROR，并把同样一句话放进 *error（若给了非空指针）；
    //       页面保持原样、不重载，isPageReady() 也不变。
    bool loadTemplate(const QString &baseDir, QString *error = nullptr);

    // ★ 唯一的内容入口：内建防抖 —— 连续调用只在"停下来" debounceInterval() 毫秒
    //   之后真正渲染一次（防抖 = 每次调用都重启计时器，只有最后一次能到期）。
    // 页面还没就绪时不渲染，但内容不会丢：记住最近一次，页面就绪后由 loadFinished 补推。
    void updateContent(const QString &markdown);

    // 立刻渲染，不等防抖（同时取消还在等待的那次，避免同一份内容渲染两遍）。
    // 用在"打开文件 / 另存为换目录"这类必须马上看到结果的场景。
    void updateContentNow(const QString &markdown);

    // 把还在等防抖的内容立刻推出去。没有待处理内容时什么也不做（也不会发信号）。
    void flush();

    // 防抖时间。传 <= 0 表示关掉防抖（每次 updateContent 都立刻渲染）——测试用得多。
    void setDebounceInterval(int ms);
    int debounceInterval() const;

    bool isPageReady() const;        // 页面（以及里面的 JS）是否已经可以接收内容
    bool hasPendingUpdate() const;   // 是否还有"等防抖到期"的内容

    // 把「HTML 片段 + 行号表」打包成一行 JS 调用：
    //     applyContent.apply(null, ["<h1>…</h1>",[1,4,7]]);
    // 为什么走 JSON 而不是字符串拼接：HTML 里的引号、换行、反斜杠会直接把 JS 语法弄坏。
    // 这里的 lineMap 必须和 HTML 里的顶层块**按顺序一一对应**（见 SyncBridge::buildLineMap）。
    // 抽成 static 纯函数（不碰页面、不要事件循环）是为了能单独测这条序列化契约。
    static QString buildApplyScript(const QString &html, const QList<int> &lineMap);

    // 文档目录 → 页面 baseUrl。空目录 = about:blank；
    // 非空则补上结尾的 '/' 再转成 file:// URL（缺了结尾斜杠的话，
    // "a.png" 会被当成上一级目录里的文件）。
    static QUrl baseUrlFromDir(const QString &baseDir);

signals:
    // 已经把内容通过 runJavaScript 交给页面了（不保证页面里的脚本已经执行完）。
    // MainWindow 连这个信号做"内容重排后重新对齐滚动位置"。
    void contentRendered();

    // 页面加载状态变化：开始重载时发 false，加载结束时发 loadFinished 的结果。
    void pageReadyChanged(bool ready);

private:
    // 真正干活的地方：解析 → 算行号 → 拼 JS → runJavaScript。
    // 页面未就绪时直接返回（内容留在 m_desired 里等下次）。
    void pushNow();
    void onLoadFinished(bool ok);

    QPointer<QWebEngineView> m_view;  // QPointer：view 被销毁（比如主窗口析构）后自动变空
    QPointer<QWebEnginePage> m_page;  // 页面挂在 view 名下，这里只是"借来看"，不拥有

    // 计时器做成值成员：生命期跟着本对象，不用 new/delete，也不会泄漏。
    QTimer m_debounce;

    QString m_desired;  // 最近一次被要求渲染的内容（页面就绪后补推的就是它）
    bool m_ready = false;
};

}  // namespace markdown_editor::core::document

#endif // PREVIEWRENDERER_H
