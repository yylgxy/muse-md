// MarkdownParser 的契约测试：把 markdownparser.h 里写的约定验一遍。
//
// 跑法：ctest -C Debug --output-on-failure   或直接运行 bin/Debug/test_markdownparser.exe
//
// 注意：这个文件里**没有一行 md4c 代码**。md4c 是 core_document 的实现细节
// （CMake 里 PRIVATE 链接），这里拿不到它的头文件 —— 这正是分层想要的效果。

#include "markdownparser.h"

#include <QString>

#include <cstdio>

using markdown_editor::core::document::MarkdownParser;

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-52s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// 渲染并去掉首尾空白：md4c 会在块级元素后补一个 '\n'，精确断言前先 trim
QString render(const QString &markdown)
{
    return MarkdownParser::parseToHtml(markdown).trimmed();
}

}  // namespace

int main()
{
    // ---------------- 验收标准 ----------------
    check(render(QStringLiteral("# 标题 **加粗**")) == QStringLiteral("<h1>标题 <strong>加粗</strong></h1>"),
          "acceptance: h1 + strong", render(QStringLiteral("# 标题 **加粗**")));

    // ---------------- CommonMark 基础语法 ----------------
    check(render(QStringLiteral("*斜体*")) == QStringLiteral("<p><em>斜体</em></p>"), "commonmark: em");
    check(render(QStringLiteral("`code`")) == QStringLiteral("<p><code>code</code></p>"), "commonmark: inline code");
    check(render(QStringLiteral("- a\n- b")).contains(QStringLiteral("<ul>")), "commonmark: unordered list");
    check(render(QStringLiteral("> 引用")).contains(QStringLiteral("<blockquote>")), "commonmark: blockquote");
    check(render(QStringLiteral("1. 第一")).contains(QStringLiteral("<ol>")), "commonmark: ordered list");
    check(render(QStringLiteral("[文字](https://example.com)"))
              .contains(QStringLiteral("<a href=\"https://example.com\">文字</a>")),
          "commonmark: link");
    check(render(QStringLiteral("```\nint a = 1;\n```")).contains(QStringLiteral("<pre><code>")),
          "commonmark: fenced code block");

    // ---------------- GFM 扩展（本阶段要支持的部分）----------------
    const QString tableHtml = render(QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |"));
    check(tableHtml.contains(QStringLiteral("<table>")) && tableHtml.contains(QStringLiteral("<th"))
              && tableHtml.contains(QStringLiteral(">a</th>")) && tableHtml.contains(QStringLiteral(">1</td>")),
          "gfm: table", tableHtml);

    const QString taskHtml = render(QStringLiteral("- [x] 完成\n- [ ] 未完成"));
    check(taskHtml.contains(QStringLiteral("type=\"checkbox\"")) && taskHtml.contains(QStringLiteral("checked")),
          "gfm: task list", taskHtml);

    check(render(QStringLiteral("~~删除~~")) == QStringLiteral("<p><del>删除</del></p>"), "gfm: strikethrough");
    check(render(QStringLiteral("https://example.com")).contains(QStringLiteral("<a href=\"https://example.com\">")),
          "gfm: autolink without <>");

    // ---------------- 编码与边界 ----------------
    check(render(QStringLiteral("# 中文 😀")) == QStringLiteral("<h1>中文 😀</h1>"), "utf8: chinese + emoji");
    check(MarkdownParser::parseToHtml(QString()).isEmpty(), "empty input -> empty string");
    check(MarkdownParser::parseToHtml(QStringLiteral("   \n\n  ")).trimmed().isEmpty(), "whitespace-only input");
    check(render(QStringLiteral("# 标题\r\n\r\n正文")) == render(QStringLiteral("# 标题\n\n正文")),
          "crlf input == lf input");
    check(render(QStringLiteral("a < b")) == QStringLiteral("<p>a &lt; b</p>"), "escaping: < becomes &lt;");
    check(!render(QStringLiteral("**未闭合 *斜*")).isEmpty(), "unclosed emphasis does not blow up");
    check(render(QStringLiteral("**加粗**")).contains(QStringLiteral("<strong>")), "strong at line start");

    // ---------------- 大输入：只验不崩、输出非空（不测时间，Debug 下计时不稳）----------------
    QString big;
    for (int i = 0; i < 2000; ++i) {
        big += QStringLiteral("- 第 %1 项 **加粗**\n").arg(i);
    }
    const QString bigHtml = MarkdownParser::parseToHtml(big);
    check(!bigHtml.isEmpty() && bigHtml.contains(QStringLiteral("<li>")), "large input (2000 list items)",
          QStringLiteral("in=%1B out=%2B").arg(big.toUtf8().size()).arg(bigHtml.toUtf8().size()));

    std::printf("\nFAIL count = %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
