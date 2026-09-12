// SyncBridge 的契约测试，两件事：
//   1. buildLineMap() 扫出来的块数和**真实渲染结果**的顶层元素个数是否一致
//      —— 这是验证那个"源码块扫描"最有力的办法：拿 md4c 的实际输出当基准。
//   2. 预览模板（.qrc 资源）是否具备双向同步必需的那几样东西。
//
// 跑法：ctest -C Debug --output-on-failure

#include "markdownparser.h"
#include "syncbridge.h"

#include <QFile>
#include <QString>
#include <QStringList>

#include <cstdio>

using markdown_editor::core::document::MarkdownParser;
using markdown_editor::core::document::SyncBridge;

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

// 数 HTML 片段里有几个"顶层元素"—— 相当于浏览器里 #content.children.length
int countTopLevelElements(const QString &html)
{
    static const QStringList voidTags = {QStringLiteral("hr"),   QStringLiteral("br"),
                                         QStringLiteral("img"),  QStringLiteral("input"),
                                         QStringLiteral("meta"), QStringLiteral("link")};

    int depth = 0;
    int count = 0;
    int i = 0;

    while (i < html.size()) {
        const int lt = html.indexOf(QLatin1Char('<'), i);
        if (lt < 0) {
            break;
        }
        const int gt = html.indexOf(QLatin1Char('>'), lt);
        if (gt < 0) {
            break;
        }
        const QString tag = html.mid(lt + 1, gt - lt - 1).trimmed();
        i = gt + 1;

        if (tag.isEmpty() || tag.startsWith(QLatin1Char('!')) || tag.startsWith(QLatin1Char('?'))) {
            continue;  // 注释 / 声明
        }
        if (tag.startsWith(QLatin1Char('/'))) {
            --depth;
            continue;
        }

        const QString name = tag.section(QLatin1Char(' '), 0, 0).toLower();
        const bool selfClosing = tag.endsWith(QLatin1Char('/')) || voidTags.contains(name);

        if (depth == 0) {
            ++count;
        }
        if (!selfClosing) {
            ++depth;
        }
    }
    return count;
}

// 核心断言：扫描出的块数 == 渲染出的顶层元素个数
void checkAligned(const QString &label, const QString &markdown)
{
    const QList<int> lines = SyncBridge::buildLineMap(markdown);
    const int domCount = countTopLevelElements(MarkdownParser::parseToHtml(markdown));
    check(lines.size() == domCount, label,
          QStringLiteral("源码块=%1 渲染元素=%2").arg(lines.size()).arg(domCount));
}

}  // namespace

int main()
{
    // ================= 1) 行号表 vs 真实渲染 =================

    // 一个"什么都有"的文档，同时断言每一块的行号
    const QString sample = QString::fromUtf8(
        "# 标题\n"           // 1  -> h1
        "\n"                  // 2
        "第一段。\n"          // 3  -> p
        "同段第二行。\n"      // 4
        "\n"                  // 5
        "- 列表一\n"          // 6  -> ul
        "- 列表二\n"          // 7
        "\n"                  // 8
        "```\n"               // 9  -> pre
        "code **not bold**\n" // 10
        "```\n"               // 11
        "\n"                  // 12
        "| a | b |\n"         // 13 -> table
        "|---|---|\n"         // 14
        "| 1 | 2 |\n"         // 15
        "\n"                  // 16
        "> 引用\n"            // 17 -> blockquote
        "\n"                  // 18
        "---\n");             // 19 -> hr
    const QList<int> sampleLines = SyncBridge::buildLineMap(sample);
    checkAligned("综合文档: 块数 == 顶层元素数", sample);
    check(sampleLines == QList<int>({1, 3, 6, 9, 13, 17, 19}), "综合文档: 每块起始行正确",
          QStringLiteral("[%1]").arg([&sampleLines] {
              QStringList s;
              for (int v : sampleLines) s << QString::number(v);
              return s.join(QLatin1Char(','));
          }()));

    // ---- 逐个语法元素 ----
    checkAligned("ATX 标题相邻（无空行）", QStringLiteral("# 一\n# 二\n"));
    checkAligned("setext 标题（文字 + ===）", QStringLiteral("标题\n===\n"));
    checkAligned("setext 标题（文字 + ---）", QStringLiteral("标题\n---\n"));
    checkAligned("段落 + 分隔线", QStringLiteral("文字\n\n---\n"));
    checkAligned("无序列表（一个 ul）", QStringLiteral("- a\n- b\n"));
    checkAligned("有序列表（一个 ol）", QStringLiteral("1. a\n2. b\n"));
    checkAligned("任务列表", QStringLiteral("- [x] 完成\n- [ ] 未完成\n"));
    checkAligned("标记类型变了 → 两个列表", QStringLiteral("- a\n\n1. b\n"));
    checkAligned("嵌套列表", QStringLiteral("- a\n  - a1\n  - a2\n- b\n"));
    checkAligned("列表中断段落", QStringLiteral("段落\n- a\n"));
    checkAligned("引用（多行）", QStringLiteral("> 一\n> 二\n"));
    checkAligned("引用 + 懒续行", QStringLiteral("> 一\n继续\n"));
    checkAligned("缩进代码块", QStringLiteral("    code\n    more\n"));
    checkAligned("围栏里的伪 Markdown 不算块", QStringLiteral("```\n# 不是标题\n- 不是列表\n```\n"));
    checkAligned("未闭合围栏（吃到文件尾）", QStringLiteral("```\nabc\n# 也算代码\n"));
    checkAligned("表格 + 后面的段落", QStringLiteral("| a |\n|---|\n| 1 |\n\n后面\n"));
    checkAligned("CRLF 换行", QStringLiteral("# 标题\r\n\r\n正文\r\n"));
    checkAligned("空文档", QString());
    checkAligned("只有空白", QStringLiteral("\n\n   \n"));
    checkAligned("代码块 + 列表 + 引用混排",
                 QStringLiteral("段落\n\n```\ncode\n```\n\n- a\n\n> q\n\n    ind\n"));

    // ================= 2) 预览模板的契约 =================
    QFile templateFile(QStringLiteral(":/html/preview_template.html"));
    const bool opened = templateFile.open(QIODevice::ReadOnly);
    check(opened, "资源: .qrc 接线正确，能打开 :/html/preview_template.html");
    if (opened) {
        const QString html = QString::fromUtf8(templateFile.readAll());
        check(html.contains(QStringLiteral("id=\"content\"")), "模板: 有 #content 容器");
        check(html.contains(QStringLiteral("qrc:///qtwebchannel/qwebchannel.js")),
              "模板: 从 Qt 内置资源引入 qwebchannel.js");
        check(html.contains(QStringLiteral("channel.objects.syncBridge")),
              "模板: 取的是 syncBridge 对象（名字必须和 C++ registerObject 一致）");
        check(html.contains(QStringLiteral("function scrollToLine")), "模板: 定义了 scrollToLine()");
        check(html.contains(QStringLiteral("function getLineAtPosition")),
              "模板: 定义了 getLineAtPosition()");
        check(html.contains(QStringLiteral("bridge.editorScrolled.connect")),
              "模板: 连上了 C++ 的 editorScrolled 信号");
        check(html.contains(QStringLiteral("bridge.reportPreviewClick")),
              "模板: 点击时回调 C++ 的 reportPreviewClick()");
        check(html.contains(QStringLiteral("setAttribute('data-line'")),
              "模板: 给顶层元素写 data-line");
        check(html.contains(QStringLiteral("function applyContent")), "模板: 定义了 applyContent()");
    }

    std::printf("\nFAIL count = %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
