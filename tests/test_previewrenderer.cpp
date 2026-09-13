// PreviewRenderer（4.1.4 渲染管线）的契约测试。
//
// 这里**故意不新建任何 QWebEnginePage / QWebEngineView**：真正渲染页面要 Chromium 运行时，
// 在受限环境里起不来（见 4.1.3 的记录）。所以拆成两块测，两块都能 100% 自动验证：
//
//   1. 序列化契约：buildApplyScript() 拼出来的那行 JS 到底长什么样 ——
//      HTML 里的引号/换行/反斜杠/中文必须被 JSON 转义，否则整段脚本会语法错误。
//      这是"内容怎么送进页面"的契约，而且它是 static 纯函数，不需要页面。
//   2. baseUrl 契约：相对路径图片能不能找到文件，全看 baseUrl 是不是"目录 + 结尾斜杠"。
//   3. 防抖时序：updateContent() 之后 hasPendingUpdate() 什么时候变 ——
//      只需要 QTimer + 事件循环，不需要页面（页面没就绪时推送本来就是空操作）。
//
// 唯一没被覆盖的是"渲染结果真的出现在页面上了"——那一步需要人工跑 GUI（见验收步骤）。
//
// 跑法：ctest -C Debug --output-on-failure

#include "previewrenderer.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <QTimer>

#include <cstdio>

using markdown_editor::core::document::PreviewRenderer;

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-58s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// 让事件循环真的跑 ms 毫秒（QTimer 必须有事件循环才会到期）
void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 把 buildApplyScript() 的输出**反解**回参数，用来验证转义是否无损。
// 脚本固定是 "applyContent.apply(null, <JSON 数组>);"，所以剥掉外壳就是 JSON。
bool parseScriptArgs(const QString &script, QJsonArray *args)
{
    const QString prefix = QStringLiteral("applyContent.apply(null, ");
    const QString suffix = QStringLiteral(");");
    if (!script.startsWith(prefix) || !script.endsWith(suffix)) {
        return false;
    }

    const QString json = script.mid(prefix.size(), script.size() - prefix.size() - suffix.size());
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return false;
    }
    *args = doc.array();
    return true;
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ---------------- 防抖时间的默认值（4.1.4 要求 300ms）----------------
    {
        PreviewRenderer renderer;
        check(renderer.debounceInterval() == 300,
              QStringLiteral("默认防抖 = 300ms"),
              QString::number(renderer.debounceInterval()));
        check(PreviewRenderer::kDefaultDebounceMs == 300, QStringLiteral("kDefaultDebounceMs 常量 = 300"));
        check(!renderer.isPageReady(), QStringLiteral("刚构造时页面未就绪"));
        check(renderer.page() == nullptr && renderer.webView() == nullptr,
              QStringLiteral("未 attach 时 page()/webView() 都是空指针（安全可判空）"));
    }

    // ---------------- 序列化契约：buildApplyScript ----------------
    {
        const QString script = PreviewRenderer::buildApplyScript(
            QStringLiteral("<h1>标题</h1>"), QList<int>{1});
        check(script == QStringLiteral("applyContent.apply(null, [\"<h1>标题</h1>\",[1]]);"),
              QStringLiteral("简单内容：脚本就是一行 applyContent.apply"),
              script);
        check(!script.contains(QLatin1Char('\n')) && !script.contains(QLatin1Char('\r')),
              QStringLiteral("脚本是单行（没有裸换行）"));
    }

    // 带刺的内容：引号 / 反斜杠 / 换行 / 中文 / 能提前闭合 <script> 的结尾标签
    {
        const QString html = QStringLiteral(
            "<p>他说：\"引号\"、反斜杠 \\ 和 </script></body> 都在这里</p>\n"
            "<pre>第一行\n第二行</pre>");
        const QList<int> lines{1, 5, 9};

        const QString script = PreviewRenderer::buildApplyScript(html, lines);

        QJsonArray args;
        const bool parsed = parseScriptArgs(script, &args);
        check(parsed, QStringLiteral("脚本能被解析回 JSON 数组（外壳格式正确）"));

        if (parsed) {
            check(args.size() == 2, QStringLiteral("参数个数 = 2（HTML + 行号表）"),
                  QString::number(args.size()));
            check(args.at(0).toString() == html,
                  QStringLiteral("HTML 转义无损：解析回来和原文一模一样"));
            check(!args.at(0).toString().isEmpty(), QStringLiteral("HTML 没被转成空串"));

            QList<int> back;
            for (const QJsonValue &v : args.at(1).toArray()) {
                back.append(v.toInt());
            }
            check(back == lines, QStringLiteral("行号表顺序和内容完整保留"),
                  QStringLiteral("%1 个").arg(back.size()));
        }

        check(!script.contains(QStringLiteral("\n<pre>")),
              QStringLiteral("原文里的裸换行没有漏进脚本（漏了就是 JS 语法错误）"));
        check(script.count(QLatin1Char('\n')) == 0, QStringLiteral("整段脚本仍然只有一行"));
    }

    // 空文档：也要拼出合法的 JS（预览区被清空，不是崩溃）
    {
        const QString script = PreviewRenderer::buildApplyScript(QString(), QList<int>{});
        check(script == QStringLiteral("applyContent.apply(null, [\"\",[]]);"),
              QStringLiteral("空内容 + 空行号表 → 合法的空推送"), script);
    }

    // ---------------- baseUrl 契约（相对路径图片靠它）----------------
    {
        check(PreviewRenderer::baseUrlFromDir(QString()) == QUrl(QStringLiteral("about:blank")),
              QStringLiteral("空目录 → about:blank（新文档没有路径）"));

        const QUrl url = PreviewRenderer::baseUrlFromDir(QStringLiteral("D:/a/b"));
        check(url.isLocalFile(), QStringLiteral("非空目录 → file:// URL"));
        check(url.toString().endsWith(QLatin1Char('/')),
              QStringLiteral("自动补上结尾斜杠（否则相对路径会跳到上一级）"),
              url.toString());
        check(url.toLocalFile() == QStringLiteral("D:/a/b/"),
              QStringLiteral("目录内容原样保留"), url.toLocalFile());

        const QUrl withSlash = PreviewRenderer::baseUrlFromDir(QStringLiteral("D:/a/b/"));
        check(withSlash == url, QStringLiteral("已经带结尾斜杠时不会变成双斜杠"), withSlash.toString());

        // 中文路径（QFileInfo::absolutePath() 在 Windows 上返回正斜杠，所以只有这一种输入）
        const QUrl cjk = PreviewRenderer::baseUrlFromDir(QStringLiteral("D:/我的文档/笔记"));
        check(cjk.isLocalFile() && cjk.toLocalFile() == QStringLiteral("D:/我的文档/笔记/"),
              QStringLiteral("中文目录无损（编码/解码能对上）"), cjk.toLocalFile());
    }

    // ---------------- 未附着时的防御性行为 ----------------
    {
        PreviewRenderer renderer;
        check(renderer.attach(nullptr) == nullptr, QStringLiteral("attach(nullptr) 返回空指针，不崩"));

        QString error;
        const bool ok = renderer.loadTemplate(QString(), &error);
        check(!ok, QStringLiteral("没 attach 就 loadTemplate → 失败"));
        check(!error.isEmpty(), QStringLiteral("失败时 *error 里有可读原因"), error);

        // 未附着时的其它调用必须是安全空操作
        renderer.updateContent(QStringLiteral("# 标题"));
        renderer.updateContentNow(QStringLiteral("# 标题"));
        renderer.flush();
        check(!renderer.isPageReady(), QStringLiteral("未附着时 isPageReady() 一直是 false（不假装成功）"));
    }

    // ---------------- 防抖时序 ----------------
    {
        PreviewRenderer renderer;
        int rendered = 0;
        QObject::connect(&renderer, &PreviewRenderer::contentRendered, [&rendered]() { ++rendered; });

        renderer.updateContent(QStringLiteral("# 一"));
        check(renderer.hasPendingUpdate(), QStringLiteral("updateContent 后进入待处理（等防抖）"));

        spin(400);  // 默认 300ms，多给 100ms 余量
        check(!renderer.hasPendingUpdate(), QStringLiteral("静置 400ms 后待处理结束（防抖已到期）"));
        check(rendered == 0, QStringLiteral("未附着页面时不会发 contentRendered（不会假装渲染过）"),
              QString::number(rendered));
    }

    // 防抖的本质：期间每来一次输入都重新计时，所以"总量"够久也不会到期
    {
        PreviewRenderer renderer;
        renderer.setDebounceInterval(120);

        renderer.updateContent(QStringLiteral("一"));
        spin(80);
        renderer.updateContent(QStringLiteral("二"));  // 重启计时器
        spin(80);                                      // 距第一次 160ms、距第二次 80ms
        check(renderer.hasPendingUpdate(), QStringLiteral("不断输入时不会到期（每次都重新计时）"));

        spin(80);  // 距第二次 160ms > 120ms
        check(!renderer.hasPendingUpdate(), QStringLiteral("停手 120ms 后才到期"));
    }

    // updateContentNow / flush：绕过防抖的两个出口
    {
        PreviewRenderer renderer;
        int rendered = 0;
        QObject::connect(&renderer, &PreviewRenderer::contentRendered, [&rendered]() { ++rendered; });

        renderer.setDebounceInterval(5000);  // 故意设很长，证明"立刻"是真的绕过它
        renderer.updateContent(QStringLiteral("慢慢来"));
        check(renderer.hasPendingUpdate(), QStringLiteral("先积累一个待处理内容"));

        renderer.flush();
        check(!renderer.hasPendingUpdate(), QStringLiteral("flush() 立刻清空待处理"));
        check(rendered == 0, QStringLiteral("flush() 在未附着时也不会发 contentRendered"));

        renderer.updateContent(QStringLiteral("再积累"));
        renderer.updateContentNow(QStringLiteral("马上"));
        check(!renderer.hasPendingUpdate(), QStringLiteral("updateContentNow 取消等待中的那一次（不会双重渲染）"));

        renderer.flush();  // 没有待处理内容
        check(rendered == 0, QStringLiteral("无待处理内容时 flush() 什么也不做"));
    }

    // 关掉防抖（<= 0）等价于"每次调用都立刻渲染"
    {
        PreviewRenderer renderer;
        renderer.setDebounceInterval(0);
        renderer.updateContent(QStringLiteral("# 立刻"));
        check(!renderer.hasPendingUpdate(), QStringLiteral("防抖 = 0 时不进入待处理（等于不防抖）"));
    }

    if (g_fail == 0) {
        std::printf("\n=== PreviewRenderer 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== PreviewRenderer 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
