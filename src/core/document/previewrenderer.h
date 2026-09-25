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
// 渲染用 MarkdownParser（和别处同一个渲染服务），不读 MarkdownDocument 里的缓存 ——
// 本类的契约只是"给我一段 Markdown"，它不需要认识文档模型。
// （4.2.1 之后 MarkdownDocument 里那份 HTML 缓存已经删掉了：预览走这条管线，
//   缓存放在数据流之外等于白算；一条渲染路径比两条好维护。）
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

    // 预览内容上限（字符数）：超过就不推给页面，改成推一句提示。
    //
    // 为什么必须有这条（不是想当然的防御，是被真实故障逼出来的）：
    // 把一份几 MB 的文本 —— 比如在文件树里双击误开的 .ilk / .pdb / .docx ——
    // 整段解析成 HTML、再打包成一条几 MB 的 JS 丢给 Chromium，页面会重排到卡死，
    // 预览的**渲染进程**甚至会被系统杀掉；而渲染进程死后连 loadFinished 都不来，
    // 预览于是永远是白的（只有重启程序才恢复，用户根本不知道发生了什么）。
    // 预览是"看"的地方，不能让"看"把整个程序拖坏：超限就明说"内容太大，预览已暂停"。
    // 1,000,000 个字符 ≈ 2 MB（UTF-16）到 3 MB（UTF-8），正常笔记远远用不到。
    static constexpr int kMaxContentChars = 1000000;

    // 渲染进程连续崩这么多次就停止自动恢复：免得"崩 → 重载 → 再崩"空转。
    static constexpr int kMaxRendererRestarts = 3;

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

    // ---- 主题（5.7）----
    // 切换预览的主题。**不重载页面**：页面里的颜色全是 CSS 变量，这里只调用页面里的
    // applyTheme() 改一个 data-theme 属性，所以是瞬时的、不闪白、不丢滚动位置。
    // themeId 只认 "dark"/"light"（别的值一律当 light）—— 这个字符串最终会进 JS，先收窄。
    void applyTheme(const QString &themeId);
    QString themeId() const;

    // 已经自动恢复过几次渲染进程（页面成功加载后清零）。给测试和日志看。
    int rendererRestartCount() const;

    // ---- 内容上限（纯函数，能脱离 Chromium 单独测）----

    // 这份内容是否超过 kMaxContentChars
    static bool exceedsContentLimit(const QString &markdown);

    // 超限时用来替代内容的那一小段 HTML：说清"多大、上限多少、怎么恢复"。
    // 全部用内联样式，不依赖预览模板里的 class（模板换了也不会变成没样式的裸文本）。
    static QString contentTooLargeHtml(const QString &markdown);

    // 把「HTML 片段 + 行号表」打包成一行 JS 调用：
    //     applyContent.apply(null, ["<h1>…</h1>",[1,4,7]]);
    // 为什么走 JSON 而不是字符串拼接：HTML 里的引号、换行、反斜杠会直接把 JS 语法弄坏。
    // 这里的 lineMap 必须和 HTML 里的顶层块**按顺序一一对应**（见 SyncBridge::buildLineMap）。
    // 抽成 static 纯函数（不碰页面、不要事件循环）是为了能单独测这条序列化契约。
    static QString buildApplyScript(const QString &html, const QList<int> &lineMap);

    // ---- 增量刷新（#2）：改动检测，纯函数、能单独测 ----
    // 计算"从 oldMarkdown 到 newMarkdown"的改动规模，供降级闸门判断。
    // 返回结构里：changedLines = 增删总行数；changedBlockRatio = 改动覆盖的块数 / 总块数
    //（块切分复用 ParseCache::splitBlocks）。degraded 表示 diff 已经放弃逐行对齐。
    // 降级闸门（写在这里，是策略而非魔法数）：changedBlockRatio > kIncrementalMaxBlockRatio
    // 或 degraded 时，调用方应回退整页 applyContent —— 增量只在改动足够小的时候才划算。
    struct ChangeStats
    {
        int changedLines = 0;      // 增删总行数
        double changedBlockRatio = 0.0;  // 0..1，改动覆盖的块占比
        bool degraded = false;     // diff 降级（改动太大，放弃逐行对齐）
    };
    static ChangeStats computeChangeStats(const QString &oldMarkdown, const QString &newMarkdown);

    // 改动块占比超过这个阈值就回退整页（增量只在改动小的时候才划算，否则 patch 反而更慢）。
    static constexpr double kIncrementalMaxBlockRatio = 0.30;

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

    // 内容超过上限，预览被跳过了。reason 是一句可以直接放进状态栏/提示的话。
    // （推给页面的是一段说明，而不是那份巨大的内容 —— 见 kMaxContentChars。）
    void contentSkipped(const QString &reason);

    // 预览的渲染进程崩了，本类正在自动恢复（第 attempt 次）。
    void rendererRestarted(int attempt);

private slots:
    // 预览的渲染进程没了（被系统杀掉 / 自己崩了）：重新载模板把它拉起来。
    // 参数用 int 而不是 QWebEnginePage::RenderProcessTerminationStatus ——
    // 那样头文件就得包含 QWebEnginePage，为了一个签名把 WebEngine 的头文件拖进接口不划算。
    // （做成"槽"不是为了被谁连，而是让元对象系统能调到它：测试可以验证"没有页面时它不空转"。）
    void onRenderProcessTerminated(int status, int exitCode);

private:
    // 真正干活的地方：解析 → 算行号 → 拼 JS → runJavaScript。
    // 页面未就绪时直接返回（内容留在 m_desired 里等下次）。
    void pushNow();
    void onLoadFinished(bool ok);

    // 把当前主题推进页面（页面没就绪时什么也不做：加载完成时会再调一次）
    void applyThemeToPage();

    QPointer<QWebEngineView> m_view;  // QPointer：view 被销毁（比如主窗口析构）后自动变空
    QPointer<QWebEnginePage> m_page;  // 页面挂在 view 名下，这里只是"借来看"，不拥有

    // 计时器做成值成员：生命期跟着本对象，不用 new/delete，也不会泄漏。
    QTimer m_debounce;

    QString m_desired;  // 最近一次被要求渲染的内容（页面就绪后补推的就是它）
    bool m_ready = false;

    // 最近一次**真的推给页面**的 Markdown（#2：增量刷新用它算改动规模 + 内容相同短路）。
    // 注意它和 m_desired 不同：m_desired 是"被要求渲染的"，m_lastPushed 是"已经渲染出去的"。
    QString m_lastPushed;

    // 最近一次载模板用的目录：渲染进程崩了要照原样重载（baseUrl 不能丢，
    // 否则重载后文档里的相对路径图片会找不到）。
    QString m_currentBaseDir;
    int m_rendererRestarts = 0;  // 已经自动恢复过几次（成功加载后清零）
    QString m_themeId = QStringLiteral("light");  // 当前预览主题（见 applyTheme）
};

}  // namespace markdown_editor::core::document

#endif // PREVIEWRENDERER_H
