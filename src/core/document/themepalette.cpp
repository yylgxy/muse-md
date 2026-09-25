#include "themepalette.h"

#include <QJsonObject>

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
    // 选中色沿用原来 QSS 里的值（视觉一模一样，只是改成通过 QPalette 传递）
    p.selectionBackground = QColor(0xb6, 0xd7, 0xff);
    p.selectionForeground = QColor(0x24, 0x29, 0x2f);

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
    // 选中色沿用原来 QSS 里的值（视觉一模一样，只是改成通过 QPalette 传递）
    p.selectionBackground = QColor(0x1f, 0x6f, 0xeb);
    p.selectionForeground = QColor(0xff, 0xff, 0xff);

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

// ============================================================================
// JSON 序列化（C7）
// ============================================================================
//
// 19 个字段，序列化/反序列化都靠同一张"字段表"驱动，而不是手写 19 对 get/set ——
// 手写的话，哪天加一个新配色字段，极可能只改了 toJson 忘了改 fromJson（或者反过来），
// 于是"导出再导入"会悄悄丢一个字段。表驱动让"字段清单"只有一份，从根上杜绝这个错位。
//
// 每个字段项记录：JSON 键名、在 ThemePalette 里的偏移（用成员指针取地址）、
// 以及一个给校验提示用的中文名。成员指针是 QColor ThemePalette::*，
// 序列化用 this->*field，反序列化用 out->*field。

namespace {

struct FieldSpec
{
    const char *key;                 // JSON 键名
    QColor ThemePalette::*member;    // 成员指针
    const char *displayName;         // 报错时给人看的中文名
};

// ★ 顺序就是 JSON 里字段出现的顺序（也是导入时要求的字段集合）。加新字段**只改这里**。
const FieldSpec kFields[] = {
    {"editorBackground", &ThemePalette::editorBackground, "编辑器底色"},
    {"editorForeground", &ThemePalette::editorForeground, "编辑器文字"},
    {"selectionBackground", &ThemePalette::selectionBackground, "选中底色"},
    {"selectionForeground", &ThemePalette::selectionForeground, "选中文字"},
    {"gutterBackground", &ThemePalette::gutterBackground, "行号栏底色"},
    {"gutterText", &ThemePalette::gutterText, "行号"},
    {"currentLineNumberText", &ThemePalette::currentLineNumberText, "当前行号"},
    {"currentLineHighlight", &ThemePalette::currentLineHighlight, "当前行高亮"},
    {"heading", &ThemePalette::heading, "标题"},
    {"bold", &ThemePalette::bold, "粗体"},
    {"italic", &ThemePalette::italic, "斜体"},
    {"inlineCodeForeground", &ThemePalette::inlineCodeForeground, "行内代码文字"},
    {"inlineCodeBackground", &ThemePalette::inlineCodeBackground, "行内代码底色"},
    {"link", &ThemePalette::link, "链接"},
    {"blockQuote", &ThemePalette::blockQuote, "引用"},
    {"listMarker", &ThemePalette::listMarker, "列表标记"},
    {"codeBlockForeground", &ThemePalette::codeBlockForeground, "代码块文字"},
    {"codeBlockBackground", &ThemePalette::codeBlockBackground, "代码块底色"},
};

}  // namespace

QJsonObject ThemePalette::toJson() const
{
    QJsonObject obj;
    for (const FieldSpec &f : kFields) {
        obj.insert(QString::fromLatin1(f.key), (this->*f.member).name());
    }
    return obj;
}

bool ThemePalette::fromJson(const QJsonObject &obj, ThemePalette *out, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };

    if (out == nullptr) {
        return fail(QStringLiteral("输出参数为空"));
    }

    // 先全部解析到一个临时对象，**全部通过**才写回 out ——
    // 这样"导入一半失败"不会留下一个"改了一半"的配色（那比不改还糟）。
    ThemePalette parsed;
    for (const FieldSpec &f : kFields) {
        const QString key = QString::fromLatin1(f.key);
        if (!obj.contains(key)) {
            return fail(QStringLiteral("缺少字段 %1（%2）").arg(key, QString::fromLatin1(f.displayName)));
        }
        const QJsonValue value = obj.value(key);
        if (!value.isString()) {
            return fail(QStringLiteral("字段 %1（%2）不是字符串").arg(key, QString::fromLatin1(f.displayName)));
        }
        const QColor color(value.toString());
        if (!color.isValid()) {
            return fail(QStringLiteral("字段 %1（%2）不是有效的颜色：%3")
                            .arg(key, QString::fromLatin1(f.displayName), value.toString()));
        }
        parsed.*f.member = color;
    }

    *out = parsed;
    return true;
}

}  // namespace markdown_editor::core::document
