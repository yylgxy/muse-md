#include "themepalette.h"

#include <QtGlobal>

#include <cmath>

namespace markdown_editor::core::document {

// ============================================================================
// 两套配色
// ============================================================================
//
// 取色的原则：
//   1. 正文和所有语法色对编辑器底色的对比度 **>= 4.5**（WCAG AA 的正文标准）——
//      颜色好看是次要的，"看得清"是硬要求（测试会算这个）。
//   2. 行号刻意压低对比度（>= 2.5 即可）：它是辅助信息，抢眼反而干扰阅读。
//   3. 暗色不是把亮色反相：底色用 #0d1117 这种带一点蓝的深灰（纯黑太刺眼），
//      语法色用亮一档的色相，保证在深底上不发闷。
//
// 这套色值刻意和预览模板里的 CSS 变量、以及 resources/styles/*.qss 里的
// 编辑器底色保持一致 —— test_thememanager 会检查这三处不打架。

ThemePalette ThemePalette::light()
{
    ThemePalette p;

    p.editorBackground = QColor(0xff, 0xff, 0xff);
    p.editorForeground = QColor(0x24, 0x29, 0x2f);

    p.gutterBackground = QColor(0xf6, 0xf8, 0xfa);
    p.gutterText = QColor(0x8c, 0x95, 0x9f);
    p.currentLineNumberText = QColor(0x24, 0x29, 0x2f);
    p.currentLineHighlight = QColor(0x09, 0x69, 0xda);

    p.heading = QColor(0x05, 0x50, 0xae);
    p.bold = QColor(0x95, 0x38, 0x00);
    p.italic = QColor(0x11, 0x63, 0x29);
    p.inlineCodeForeground = QColor(0x0a, 0x30, 0x69);
    p.inlineCodeBackground = QColor(0xf6, 0xf8, 0xfa);
    p.link = QColor(0x09, 0x69, 0xda);
    p.blockQuote = QColor(0x57, 0x60, 0x6a);
    p.listMarker = QColor(0x82, 0x50, 0xdf);

    p.codeBlockForeground = QColor(0x1f, 0x23, 0x28);
    p.codeBlockBackground = QColor(0xf6, 0xf8, 0xfa);

    return p;
}

ThemePalette ThemePalette::dark()
{
    ThemePalette p;

    p.editorBackground = QColor(0x0d, 0x11, 0x17);
    p.editorForeground = QColor(0xc9, 0xd1, 0xd9);

    p.gutterBackground = QColor(0x01, 0x04, 0x09);
    p.gutterText = QColor(0x7d, 0x85, 0x90);
    p.currentLineNumberText = QColor(0xc9, 0xd1, 0xd9);
    p.currentLineHighlight = QColor(0x38, 0x8b, 0xfd);

    p.heading = QColor(0x79, 0xc0, 0xff);
    p.bold = QColor(0xff, 0xa6, 0x57);
    p.italic = QColor(0x7e, 0xe7, 0x87);
    p.inlineCodeForeground = QColor(0xa5, 0xd6, 0xff);
    p.inlineCodeBackground = QColor(0x16, 0x1b, 0x22);
    p.link = QColor(0x58, 0xa6, 0xff);
    p.blockQuote = QColor(0x8b, 0x94, 0x9e);
    p.listMarker = QColor(0xd2, 0xa8, 0xff);

    p.codeBlockForeground = QColor(0xc9, 0xd1, 0xd9);
    p.codeBlockBackground = QColor(0x16, 0x1b, 0x22);

    return p;
}

// ============================================================================
// 对比度（WCAG 2.1）
// ============================================================================

double ThemePalette::relativeLuminance(const QColor &color)
{
    // sRGB 线性化 → 加权和。公式抄自 WCAG 2.1 的定义，别自己化简。
    const auto channel = [](double value) {
        const double c = value / 255.0;
        return (c <= 0.03928) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
    };

    return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green()) + 0.0722 * channel(color.blue());
}

double ThemePalette::contrastRatio(const QColor &a, const QColor &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double lighter = qMax(la, lb);
    const double darker = qMin(la, lb);
    return (lighter + 0.05) / (darker + 0.05);
}

}  // namespace markdown_editor::core::document
