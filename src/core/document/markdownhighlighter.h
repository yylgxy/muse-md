#ifndef MARKDOWNHIGHLIGHTER_H
#define MARKDOWNHIGHLIGHTER_H

#include <QList>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include "themepalette.h"

// Markdown 编辑器侧的**语法高亮**（只管"看着清楚"，渲染成 HTML 是 MarkdownParser 的事）。
//
// 理解这个类要先理解 QSyntaxHighlighter 的四条工作机制：
//   1. Qt 把文档按"块"（block ≈ 一行）喂给 highlightBlock()，一次一行；
//   2. setFormat(起始位置, 长度, 格式) 给这一行里的一段文字上色；
//   3. **同一个字符被多次 setFormat 时，后设置的在它设置过的属性上覆盖先设置的**
//      —— 所以规则顺序很重要，也所以"整行上色"的规则会吃掉行内规则的效果；
//   4. 需要**跨行**的状态（比如 ``` 代码块从哪到哪）要用
//      setCurrentBlockState() / previousBlockState() 自己记，框架不管这个。
//
// 注意：这里的规则是"够用就好"的正则，目标是高亮好看，**不追求和 CommonMark 完全一致**
// （真正的解析权威是 MarkdownParser/md4c）。所以它不需要也不应该去调用解析器。
//
// 配色（5.7 主题系统）：颜色**不再写死在构造函数里**，而是来自一份 ThemePalette。
// setPalette() 会重建规则并重新高亮整个文档 —— 切主题时一行调用就能整体换色。
class MarkdownHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    // 本类在**全局命名空间**（历史遗留：mainwindow.ui 提升它时 uic 只会写简单类名），
    // 而配色表住在 markdown_editor::core::document 里 —— 用别名把它接进来，
    // 这样接口上的写法仍然简短，也不会让本文件被命名空间的细节淹没。
    using ThemePalette = markdown_editor::core::document::ThemePalette;

    explicit MarkdownHighlighter(QTextDocument *parent = nullptr);

    // 换一套配色（亮/暗），并立刻重新高亮。
    // 与当前配色完全相同时直接返回：主题重复应用不该造成无谓的重绘（闪烁就是这么来的）。
    void setPalette(const ThemePalette &palette);
    ThemePalette palette() const;

protected:
    // 每一行文本都会进入这里做高亮
    void highlightBlock(const QString &text) override;

private:
    // 一条规则：正则 + 格式 + 只给第几个捕获组上色
    struct HighlightRule
    {
        QRegularExpression pattern;
        QTextCharFormat format;
        int captureGroup = 0;  // 0 = 整个匹配；1 = 只给第 1 个括号里的部分上色
    };

    // 按当前配色重建 m_rules 与代码块格式
    void rebuildFormats();

    ThemePalette m_palette;

    // 代码块整块用的格式：highlightBlock 里要用到，所以留作成员
    QTextCharFormat m_codeBlockFormat;

    QList<HighlightRule> m_rules;
};

#endif // MARKDOWNHIGHLIGHTER_H
