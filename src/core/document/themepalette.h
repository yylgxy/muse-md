#ifndef THEMEPALETTE_H
#define THEMEPALETTE_H

#include <QColor>
#include <QJsonObject>
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

    // ---- JSON 序列化（C7 主题导入导出）----
    // 把整套配色压成一个 QJsonObject：键是字段名（"editorBackground" 这种），
    // 值是 "#RRGGBB" 字符串。颜色用名字而不是 RGB 分量，是为了让导出的文件人眼可读
    //（用户可能会想手工调一个主题，看 `"heading": "#ff0000"` 比看三个数字直观得多）。
    QJsonObject toJson() const;

    // 从 QJsonObject 反序列化。**这是唯一一个需要严格校验的入口**：
    //   * 所有字段都必须存在且是字符串、能解析成有效颜色 —— 缺一个键或者值非法都算失败；
    //   * 为什么"全部要、一个不能少"：配色表的所有字段是**一起被用**的，
    //     少一个字段就会让那个位置退回 QColor 的默认值（黑色），
    //     而"某个语法元素突然变黑"这种 bug 比"整份主题拒绝导入"难查得多。
    // 失败时 *error 写一句人话（哪个键出了什么问题），返回 false 且不改动 out。
    // 返回 true 时 out 一定是完整的（所有字段都填了）。
    static bool fromJson(const QJsonObject &obj, ThemePalette *out, QString *error = nullptr);
};

}  // namespace markdown_editor::core::document

#endif // THEMEPALETTE_H
