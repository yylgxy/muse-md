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
    : QSyntaxHighlighter(parent), m_palette(ThemePalette::light())
{
    rebuildFormats();
}

void MarkdownHighlighter::setPalette(const ThemePalette &palette)
{
    if (palette.editorBackground == m_palette.editorBackground && palette.heading == m_palette.heading
        && palette.codeBlockBackground == m_palette.codeBlockBackground && palette.link == m_palette.link
        && palette.editorForeground == m_palette.editorForeground) {
        // 这几项相同就认为"还是同一套"：省掉一次全文重新高亮。
        // （不需要逐字段比 —— 主题只有两套，且两套之间首字段一定不同。）
        return;
    }

    m_palette = palette;
    rebuildFormats();
    rehighlight();  // 立刻把整个文档按新配色重画
}

// 注意返回类型要写全限定名：C++ 解析返回类型时还没进入 MarkdownHighlighter 的作用域，
// 所以类里那个 ThemePalette 别名在这里是看不见的（参数类型在限定名之后，反而能用短名字）。
markdown_editor::core::document::ThemePalette MarkdownHighlighter::palette() const
{
    return m_palette;
}

void MarkdownHighlighter::rebuildFormats()
{
    // ========== 1. 各类元素的样式（颜色全部来自配色表）==========
    QTextCharFormat headerFormat;
    headerFormat.setForeground(m_palette.heading);
    headerFormat.setFontWeight(QFont::Bold);

    QTextCharFormat boldFormat;
    boldFormat.setForeground(m_palette.bold);
    boldFormat.setFontWeight(QFont::Bold);

    QTextCharFormat italicFormat;
    italicFormat.setForeground(m_palette.italic);
    italicFormat.setFontItalic(true);

    QTextCharFormat codeInlineFormat;
    codeInlineFormat.setForeground(m_palette.inlineCodeForeground);
    codeInlineFormat.setBackground(m_palette.inlineCodeBackground);

    QTextCharFormat linkFormat;
    linkFormat.setForeground(m_palette.link);
    linkFormat.setFontUnderline(true);

    QTextCharFormat blockQuoteFormat;
    blockQuoteFormat.setForeground(m_palette.blockQuote);

    QTextCharFormat listFormat;
    listFormat.setForeground(m_palette.listMarker);

    m_codeBlockFormat = QTextCharFormat();
    m_codeBlockFormat.setForeground(m_palette.codeBlockForeground);
    m_codeBlockFormat.setBackground(m_palette.codeBlockBackground);

    // ========== 2. 注册规则 ==========
    // 顺序有讲究：**先整行、后局部** —— 因为同一个字符后面 setFormat 的会覆盖前面的，
    // 否则"标题整行蓝色"会把标题里的 **粗体** 也吃掉。
    m_rules.clear();

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
