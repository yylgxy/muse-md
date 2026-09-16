#ifndef THEMEPALETTE_H
#define THEMEPALETTE_H

#include <QColor>
#include <QString>

namespace markdown_editor::core::document {

// 编辑器的主题配色（亮/暗两套）。
//
// 为什么单独有个"配色表"而不是到处写颜色：
//   * 语法高亮（MarkdownHighlighter）和行号栏（EditorWidget）用的是两套机制
//     （QTextCharFormat / QPainter），但**视觉上必须是一套** —— 都从这里取色就不会各写各的；
//   * 主题切换时只需要把一份 ThemePalette 交给编辑器和行号栏，颜色立刻整体换掉；
//   * 配色是"能被测的东西"：test_thememanager 会检查亮暗两套的每个颜色都不同、
//     而且文字色对背景色的对比度达标（WCAG）。颜色写死在两个 .cpp 里是不可能这样验的。
//
// 分层注记：本文件在 core/document（不是 business）。因为 MarkdownHighlighter 属于核心层，
// 而核心层不能反过来依赖业务层 —— 所以"配色表"归核心，"什么时候切换主题"归业务（ThemeManager）。
//
// 预览区的配色**不在**这里：预览是网页，用的是 CSS 变量（见 preview_template.html），
// 由 ThemeManager 通过 JS 改一个 data-theme 属性来切换（不重载页面，所以不会闪白）。
class ThemePalette
{
public:
    // ---------------- 编辑器底色 ----------------
    QColor editorBackground;
    QColor editorForeground;
    // 选中文字的背景/前景。原来写在 QSS 里（QPlainTextEdit { selection-background-color }），
    // 现在改成走 QPalette —— 给控件写 QSS 会让每次重绘都走 QStyleSheetStyle，
    // 而编辑器是重绘最频繁的控件（详见 EditorWidget::setThemePalette 的说明）。
    // 数值和原来 QSS 里的一模一样，视觉不变。
    QColor selectionBackground;
    QColor selectionForeground;

    // ---------------- 行号栏 ----------------
    QColor gutterBackground;
    QColor gutterText;             // 普通行号（刻意压低对比度：它是次要信息）
    QColor currentLineNumberText;  // 当前行的行号（要跟正文一样清楚）
    QColor currentLineHighlight;   // 当前行底色（使用时带透明度）

    // ---------------- Markdown 语法 ----------------
    QColor heading;
    QColor bold;
    QColor italic;
    QColor inlineCodeForeground;
    QColor inlineCodeBackground;
    QColor link;
    QColor blockQuote;
    QColor listMarker;

    // ---------------- 围栏代码块 ----------------
    QColor codeBlockForeground;
    QColor codeBlockBackground;

    // 亮色 / 暗色两套。**每个字段都必须不同**（有测试盯着）——
    // 抄漏一个字段的表现就是"切了主题但某处还是旧颜色"，很容易看漏。
    static ThemePalette light();
    static ThemePalette dark();

    // ---- 对比度计算（给测试和"配色自检"用）----
    // WCAG 2.1 的相对亮度与对比度公式。1.0 = 同色，21.0 = 纯黑对纯白。
    static double relativeLuminance(const QColor &color);
    static double contrastRatio(const QColor &a, const QColor &b);
};

}  // namespace markdown_editor::core::document

#endif // THEMEPALETTE_H
