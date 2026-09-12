// MarkdownHighlighter 的契约测试：验证"哪一段文字被上了什么格式"。
//
// 跑法：ctest -C Debug --output-on-failure   或直接运行 bin/Debug/test_markdownhighlighter.exe
//
// 为什么这样能测：QSyntaxHighlighter 并不改文本内容，它只是往每个 block 的
// QTextLayout 里塞"格式区间"(QTextLayout::FormatRange)。所以我们可以把文本交给它，
// 再把 layout()->formats() 读出来断言 —— 完全不需要把窗口显示出来。

#include "markdownhighlighter.h"

#include <QGuiApplication>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <cstdio>

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

QList<QTextLayout::FormatRange> formatsOf(const QTextDocument &doc, int line)
{
    const QTextBlock block = doc.findBlockByNumber(line);
    if (!block.isValid() || block.layout() == nullptr) {
        return {};
    }
    return block.layout()->formats();
}

// 算出某个字符位置上"最终生效"的格式：按顺序 merge，后写的覆盖先写的
// （这和 Qt 渲染时的覆盖顺序一致，也就是这个测试能验证"规则顺序/互相覆盖"的原因）
QTextCharFormat effectiveFormat(const QTextDocument &doc, int line, int pos)
{
    QTextCharFormat merged;
    const QList<QTextLayout::FormatRange> ranges = formatsOf(doc, line);
    for (const QTextLayout::FormatRange &range : ranges) {
        if (pos >= range.start && pos < range.start + range.length) {
            merged.merge(range.format);
        }
    }
    return merged;
}

// 某个位置有没有被"任何"格式覆盖到（用来判断代码块整行是否被上色）
bool coveredAt(const QTextDocument &doc, int line, int pos)
{
    const QList<QTextLayout::FormatRange> ranges = formatsOf(doc, line);
    for (const QTextLayout::FormatRange &range : ranges) {
        if (pos >= range.start && pos < range.start + range.length) {
            return true;
        }
    }
    return false;
}

const QColor kBoldColor(180, 0, 0);
const QColor kCodeBackground(245, 245, 245);

}  // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);   // QTextDocument / QFont 需要 GUI 应用对象

    QTextDocument doc;
    MarkdownHighlighter highlighter(&doc);

    // ---------------- 标题：整行蓝色 + 粗体 ----------------
    doc.setPlainText(QStringLiteral("# 标题"));
    highlighter.rehighlight();
    check(effectiveFormat(doc, 0, 2).foreground().color() == QColor(Qt::blue), "标题: 整行蓝色");
    check(effectiveFormat(doc, 0, 2).fontWeight() == QFont::Bold, "标题: 粗体");

    // ---------------- ★ 关键：粗体不能被斜体规则吃掉 ----------------
    // 旧规则 \\*(.+?)\\* 会把 **粗体** 整段也匹配一遍，于是斜体的绿色覆盖深红，
    // 结果 **粗体** 显示成绿色斜体。
    doc.setPlainText(QStringLiteral("**粗体**"));
    highlighter.rehighlight();
    check(effectiveFormat(doc, 0, 2).foreground().color() == kBoldColor, "粗体: 颜色是深红(不被斜体覆盖)",
          effectiveFormat(doc, 0, 2).foreground().color().name());
    check(!effectiveFormat(doc, 0, 2).fontItalic(), "粗体: 没有被错加斜体");
    check(effectiveFormat(doc, 0, 2).fontWeight() == QFont::Bold, "粗体: 是粗体");

    // ---------------- 斜体 ----------------
    doc.setPlainText(QStringLiteral("*斜体*"));
    highlighter.rehighlight();
    check(effectiveFormat(doc, 0, 1).foreground().color() == QColor(Qt::darkGreen), "斜体: 绿色");
    check(effectiveFormat(doc, 0, 1).fontItalic(), "斜体: 是斜体");

    // ---------------- ★ 列表只给标记上色，不吃掉行内格式 ----------------
    // 旧规则 ^\\s*[-*] .* 把整行涂紫，行内的 **重点** 就不红了
    doc.setPlainText(QStringLiteral("- **重点**"));
    highlighter.rehighlight();
    check(effectiveFormat(doc, 0, 0).foreground().color() == QColor(128, 0, 128), "列表: 标记 - 是紫色");
    check(effectiveFormat(doc, 0, 4).foreground().color() == kBoldColor, "列表: 行内粗体仍是深红");
    check(effectiveFormat(doc, 0, 4).fontWeight() == QFont::Bold, "列表: 行内粗体仍是粗体");

    // ---------------- ★ 引用同理 ----------------
    doc.setPlainText(QStringLiteral("> **引用粗体**"));
    highlighter.rehighlight();
    check(effectiveFormat(doc, 0, 0).foreground().color() == QColor(Qt::darkGray), "引用: 标记 > 是深灰");
    check(effectiveFormat(doc, 0, 4).foreground().color() == kBoldColor, "引用: 行内粗体仍是深红");

    // ---------------- 有序列表标记 ----------------
    doc.setPlainText(QStringLiteral("1. 有序项"));
    highlighter.rehighlight();
    check(effectiveFormat(doc, 0, 0).foreground().color() == QColor(128, 0, 128), "有序列表: 编号 1. 是紫色");
    check(!coveredAt(doc, 0, 3), "有序列表: 正文没有被涂色");

    // ---------------- 行内代码 / 链接 ----------------
    doc.setPlainText(QStringLiteral("这是 `code` 结束"));
    highlighter.rehighlight();
    check(effectiveFormat(doc, 0, 4).background().color() == QColor(240, 240, 240), "行内代码: 有浅灰背景");
    check(effectiveFormat(doc, 0, 4).foreground().color() == QColor(Qt::darkGray), "行内代码: 深灰字");

    doc.setPlainText(QStringLiteral("[文字](https://a.b)"));
    highlighter.rehighlight();
    check(effectiveFormat(doc, 0, 1).fontUnderline(), "链接: 有下划线");

    // ---------------- ★ 代码块跨行 ----------------
    doc.setPlainText(QStringLiteral("```\ncode **粗体**\n```\n普通行"));
    highlighter.rehighlight();
    check(coveredAt(doc, 0, 0) && effectiveFormat(doc, 0, 0).background().color() == kCodeBackground,
          "代码块: 起始围栏行被整行上色");
    check(effectiveFormat(doc, 1, 6).background().color() == kCodeBackground,
          "代码块: 块内那一行也被上色（跨行状态生效）");
    check(effectiveFormat(doc, 1, 7).foreground().color() == QColor(Qt::darkGray),
          "代码块: 块内不再套用行内规则（粗体没被标红）");
    check(effectiveFormat(doc, 1, 7).fontWeight() != QFont::Bold, "代码块: 块内文字不是粗体");
    check(effectiveFormat(doc, 2, 0).background().color() == kCodeBackground, "代码块: 结束围栏行也上色");
    check(!coveredAt(doc, 3, 0), "代码块结束后: 普通行不再上色（状态已复位）");

    // ---------------- 未闭合的围栏：后面的内容都算代码块 ----------------
    doc.setPlainText(QStringLiteral("```\n还没闭合\n也是代码"));
    highlighter.rehighlight();
    check(effectiveFormat(doc, 2, 0).background().color() == kCodeBackground,
          "未闭合围栏: 之后的整块都按代码显示");

    std::printf("\nFAIL count = %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
