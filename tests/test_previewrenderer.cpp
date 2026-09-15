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

    // ============================ 内容上限（大文件不再能把预览拖死）============================
    // 这一段是一个真实故障的回归测试：在文件树里双击打开了 7 MB 的 .ilk（二进制）之后，
    // 预览的渲染进程被拖死，界面从此一片空白、连日志都不报错，只能重启程序。
    // 现在的规则是"超限就只推一小段说明"，所以这两条纯函数必须钉死。
    {
        const int limit = PreviewRenderer::kMaxContentChars;
        check(limit == 1000000, QStringLiteral("上限: 就是 1,000,000 个字符"), QString::number(limit));
        check(PreviewRenderer::kMaxRendererRestarts == 3,
              QStringLiteral("上限: 渲染进程最多自动恢复 3 次（不会无限空转）"));

        const QString small = QStringLiteral("# 标题\n\n一段正常的内容");
        check(!PreviewRenderer::exceedsContentLimit(small), QStringLiteral("上限: 正常文档不超限"));
        check(!PreviewRenderer::exceedsContentLimit(QString()), QStringLiteral("上限: 空内容不超限"));

        // 边界：正好等于上限不算超，多一个字符才算
        const QString exactly(limit, QLatin1Char('a'));
        check(!PreviewRenderer::exceedsContentLimit(exactly), QStringLiteral("上限: 正好等于上限 -> 不超"));
        check(PreviewRenderer::exceedsContentLimit(exactly + QLatin1Char('a')),
              QStringLiteral("上限: 多一个字符 -> 超限"));

        // 超限时替代推送的那段 HTML：必须"自带宽高 + 说清多大 + 不夹带原文"
        const QString huge(limit + 12345, QLatin1Char('x'));
        const QString notice = PreviewRenderer::contentTooLargeHtml(huge);
        check(notice.contains(QStringLiteral("1.0 兆字符")) || notice.contains(QStringLiteral("100.1 万字符")),
              QStringLiteral("超限提示: 说得出到底多大"), notice.left(48));
        check(notice.contains(QString::number(limit)), QStringLiteral("超限提示: 说得出上限是多少"));
        check(notice.contains(QStringLiteral("预览已暂停")), QStringLiteral("超限提示: 明确说预览被暂停了"));
        check(notice.contains(QStringLiteral("二进制")),
              QStringLiteral("超限提示: 提醒「可能是二进制文件」（最常见的原因）"));
        check(notice.size() < 2000, QStringLiteral("超限提示: 它本身必须很小（不能又变成大内容）"),
              QStringLiteral("%1 字符").arg(notice.size()));
        check(!notice.contains(QStringLiteral("xxxxx")), QStringLiteral("超限提示: 不夹带原文"));

        // 超限内容不会被送去解析/推送 —— 没附着页面时 pushNow 本来就是空操作，
        // 这里验证"内容仍然记着、防抖仍然正常"，也就是程序状态没有被大内容搞乱
        PreviewRenderer renderer;
        renderer.setDebounceInterval(1);
        renderer.updateContent(huge);
        spin(30);
        check(!renderer.hasPendingUpdate(), QStringLiteral("超限: 处理完之后没有卡在待处理状态"));
        check(!renderer.isPageReady(), QStringLiteral("超限: 没附着页面时依然不是就绪状态（不崩）"));
    }

    // ============================ 渲染进程崩了要能自愈 ============================
    {
        PreviewRenderer renderer;
        check(renderer.rendererRestartCount() == 0, QStringLiteral("自愈: 一开始恢复次数是 0"));

        int restarted = 0;
        int skipped = 0;
        QObject::connect(&renderer, &PreviewRenderer::rendererRestarted, [&restarted](int) { ++restarted; });
        QObject::connect(&renderer, &PreviewRenderer::contentSkipped, [&skipped](const QString &) { ++skipped; });

        // 没附着页面（测试环境里就是如此）时被通知"渲染进程结束了"：
        // 必须什么都不做 —— 既不空转重载，也不发信号，更不许崩。
        const bool invoked = QMetaObject::invokeMethod(&renderer,
                                                      "onRenderProcessTerminated",
                                                      Q_ARG(int, 1),
                                                      Q_ARG(int, 139));
        check(invoked, QStringLiteral("自愈: 崩溃处理入口能被元对象系统调用"));
        check(renderer.rendererRestartCount() == 0, QStringLiteral("自愈: 没有页面时不空转（恢复次数仍是 0）"));
        check(restarted == 0 && skipped == 0, QStringLiteral("自愈: 没有页面时不发信号"));

        // 上面那次调用之后仍然能正常工作：说明"渲染进程没了"这件事没把渲染器弄坏
        renderer.setDebounceInterval(0);
        renderer.updateContent(QStringLiteral("# 还能用"));
        check(!renderer.hasPendingUpdate(), QStringLiteral("自愈: 崩溃之后渲染器还能继续接内容"));
    }

    // ============================ 7.2 防抖就是 300ms ============================
    {
        PreviewRenderer renderer;
        check(PreviewRenderer::kDefaultDebounceMs == 300,
              QStringLiteral("防抖: 默认就是 300ms（规格要求「停止输入 300ms 再渲染」）"),
              QStringLiteral("%1 ms").arg(PreviewRenderer::kDefaultDebounceMs));
        check(renderer.debounceInterval() == 300, QStringLiteral("防抖: 新建的渲染器用的就是这个值"),
              QStringLiteral("%1 ms").arg(renderer.debounceInterval()));

        // "打字不卡顿"的本质：连打十次，只有最后一次到期才真正渲染一次
        renderer.setDebounceInterval(120);
        for (int i = 0; i < 10; ++i) {
            renderer.updateContent(QStringLiteral("第 %1 次输入").arg(i));
            spin(20);  // 每次间隔都短于防抖窗口
        }
        check(renderer.hasPendingUpdate(),
              QStringLiteral("防抖: 连续敲十个字，一次都没提前渲染（都还在等）"));
        spin(150);
        check(!renderer.hasPendingUpdate(), QStringLiteral("防抖: 停手之后才渲染一次"));
    }

    if (g_fail == 0) {
        std::printf("\n=== PreviewRenderer 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== PreviewRenderer 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
