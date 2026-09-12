#include "markdownhighlighter.h"   // ← 原来写成了 markdowndhighlighter.h（多了个 d），编译不过

#include <QColor>
#include <QFont>
#include <QTextDocument>

namespace {

// 代码块的状态标记（存在每个 block 的 state 里，用来跨行判断）
constexpr int kNotInCodeBlock = 0;
constexpr int kInCodeBlock = 1;

}  // namespace

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
{
    // ========== 1. 各类元素的样式 ==========
    // 说明：这些颜色现在是写死的（浅色主题下看着还行）。深色主题、用户自定义配色，
    // 以后再改成从配置读；那时把这些 QTextCharFormat 提升为成员、并加一个
    // rebuildRules() 重新构建 m_rules 即可（注意：改成员不会自动影响已注册的规则，
    // 因为规则里存的是当时的副本）。
    QTextCharFormat headerFormat;
    headerFormat.setForeground(Qt::blue);
    headerFormat.setFontWeight(QFont::Bold);

    QTextCharFormat boldFormat;
    boldFormat.setForeground(QColor(180, 0, 0));
    boldFormat.setFontWeight(QFont::Bold);

    QTextCharFormat italicFormat;
    italicFormat.setForeground(Qt::darkGreen);
    italicFormat.setFontItalic(true);

    QTextCharFormat codeInlineFormat;
    codeInlineFormat.setForeground(Qt::darkGray);
    codeInlineFormat.setBackground(QColor(240, 240, 240));

    QTextCharFormat linkFormat;
    linkFormat.setForeground(Qt::blue);
    linkFormat.setFontUnderline(true);

    QTextCharFormat blockQuoteFormat;
    blockQuoteFormat.setForeground(Qt::darkGray);

    QTextCharFormat listFormat;
    listFormat.setForeground(QColor(128, 0, 128));

    m_codeBlockFormat.setForeground(Qt::darkGray);
    m_codeBlockFormat.setBackground(QColor(245, 245, 245));

    // ========== 2. 注册规则 ==========
    // 顺序有讲究：**先整行、后局部** —— 因为同一个字符后面 setFormat 的会覆盖前面的，
    // 否则"标题整行蓝色"会把标题里的 **粗体** 也吃掉。

    // 标题：整行（CommonMark 要求 # 后面有空格）
    m_rules.append({QRegularExpression(QStringLiteral("^\\s*#{1,6}\\s+.*$")), headerFormat});

    // 粗体：**文字**
    // 两个负向环视 (?<!\*) / (?!\*) 是关键：没有它们的话，单星号斜体规则
    // 会把 **粗体** 整段也匹配一遍（引擎会从第一个 * 一直贪到最后一个 *），
    // 于是斜体的绿色把粗体的深红覆盖掉 —— 结果 **粗体** 显示成绿色斜体。
    m_rules.append({QRegularExpression(QStringLiteral("(?<!\\*)\\*\\*([^*\\n]+?)\\*\\*(?!\\*)")),
                    boldFormat});

    // 斜体：*文字*
    m_rules.append({QRegularExpression(QStringLiteral("(?<!\\*)\\*([^*\\n]+?)\\*(?!\\*)")),
                    italicFormat});

    // 行内代码：`code`
    m_rules.append({QRegularExpression(QStringLiteral("`([^`\\n]+?)`")), codeInlineFormat});

    // 链接：[文字](url)
    m_rules.append({QRegularExpression(QStringLiteral("\\[([^\\]\\n]+?)\\]\\(([^)\\n]+?)\\)")),
                    linkFormat});

    // 引用：**只给 > 这个标记上色**（捕获组 1）。
    // 原来是 ^>.* 整行上色 —— 那样后面行内规则标好的粗体/链接颜色会被整行色盖掉。
    m_rules.append({QRegularExpression(QStringLiteral("^\\s*(>)\\s?")), blockQuoteFormat, 1});

    // 无序列表：只给 - * + 标记上色
    m_rules.append({QRegularExpression(QStringLiteral("^\\s*([-*+])\\s")), listFormat, 1});

    // 有序列表：只给 1. 这样的编号上色
    m_rules.append({QRegularExpression(QStringLiteral("^\\s*(\\d+\\.)\\s")), listFormat, 1});
}

void MarkdownHighlighter::highlightBlock(const QString &text)
{
    // ---------- 1) 先处理跨行的代码块 ----------
    // 围栏行：行首（允许缩进）是 ``` 或 ~~~
    static const QRegularExpression fencePattern(QStringLiteral("^\\s*(```|~~~)"));

    int state = previousBlockState();
    if (state < 0) {
        state = kNotInCodeBlock;  // 上一行没设过状态（文档开头）
    }

    if (fencePattern.match(text).hasMatch()) {
        // 围栏行本身也按代码块样式显示，并把状态翻转（进入 / 离开代码块）
        setFormat(0, text.length(), m_codeBlockFormat);
        setCurrentBlockState(state == kInCodeBlock ? kNotInCodeBlock : kInCodeBlock);
        return;  // 围栏行不再套用行内规则
    }

    if (state == kInCodeBlock) {
        // 代码块内部：整行代码块样式，而且**不再套用**行内规则
        // （这样代码里的 **、[x](y) 不会被当成粗体/链接去着色）
        setFormat(0, text.length(), m_codeBlockFormat);
        setCurrentBlockState(kInCodeBlock);
        return;
    }

    setCurrentBlockState(kNotInCodeBlock);

    // ---------- 2) 行内规则 ----------
    for (const HighlightRule &rule : m_rules) {
        QRegularExpressionMatchIterator iter = rule.pattern.globalMatch(text);
        while (iter.hasNext()) {
            const QRegularExpressionMatch match = iter.next();
            // captureGroup 为 1 时只给括号里的部分上色（例如只给列表标记上色）
            setFormat(match.capturedStart(rule.captureGroup),
                      match.capturedLength(rule.captureGroup),
                      rule.format);
        }
    }
}
