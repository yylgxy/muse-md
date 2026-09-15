#ifndef CODEHIGHLIGHTER_H
#define CODEHIGHLIGHTER_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

namespace markdown_editor::core::document {

// 代码记号的种类。这些名字（除 Plain 外）会变成 HTML 里的 class，所以它们既是
// 内部枚举、也是对外契约 —— 预览模板的 CSS 就是照着它们写的（见 cssClassNameFor()）。
enum class CodeTokenKind {
    Plain,      // 不生成 span（普通文字）
    Comment,    // 注释
    Keyword,    // 关键字（if/class/def/let…）
    Type,       // 类型名（int/String/Vec<i32>…）
    Literal,    // 字面量（true/false/null/None/nil…）
    Builtin,    // 内置函数或常见库函数（print/len/printf…）
    String,     // 字符串
    Number,     // 数字
    Function,   // 函数/方法名（标识符后面跟着括号）
    Attribute,  // 属性名/键名（HTML 属性、CSS 属性、JSON/YAML/TOML 的键、$变量、${变量}）
    Tag,        // 标签名（HTML/XML）或选择器/小节名（CSS/INI/TOML）
    Operator,   // 运算符
    Meta,       // 预处理指令、注解、装饰器、diff 头（#include、@Override、---）
    DiffAdd,    // diff：新增行
    DiffDel,    // diff：删除行
};

// 一段记号：从 start 开始、长度 length、种类 kind。Plain 不会出现在结果里。
struct CodeToken
{
    int start = 0;
    int length = 0;
    CodeTokenKind kind = CodeTokenKind::Plain;
};

// 代码语法高亮（按语言把代码切成记号，再渲染成带 class 的 span）。
//
// ★ 为什么是 C++ 规则引擎，而不是塞一个 highlight.js 进 qrc：
//   预览（QWebEngineView）、导出的 HTML、导出的 PDF 必须**同源** —— 三条路看到的高亮必须一样。
//   如果高亮在 JS 里做，导出的 HTML 就得内联一份脚本、PDF 还得赌"打印时脚本已经跑完"，
//   而且**在这台机器上根本验证不了**（受控环境里 Chromium 起不来，只能靠人肉看）。
//   放在 C++ 里：预览、HTML、PDF 走的是同一个函数，且每个语言的每个记号都能被自动断言。
//   代价是规则没有 highlight.js 那么深（比如不做跨语言嵌入、不做模板语法），
//   这对"看自己的笔记"这个用途是够的。
//
// ★ 语言怎么选：Markdown 的围栏信息串就是语言名（```python），也就是"自己选"的那一步；
//   界面上还有「工具 → 插入代码块…」可以挑（见 MainWindow::onInsertCodeBlock）。
//   别名都认：py/PY/C++/cxx/js/ts/yml/sh/ps1/cs/rs/golang/kt/htm/xml…
//   不认识的语言名 **不着色但也不报错**（原样按纯文本显示），这一点很重要：
//   文档里出现一个冷门语言名不该让预览变丑或者弹错。
//
// 用法（三条路都是一行）：
//     html = CodeHighlighter::highlightCodeBlocks(MarkdownParser::parseToHtml(markdown));
class CodeHighlighter
{
public:
    // 支持的语言（规范名，按"常用程度"排好序，可以直接填进下拉框）
    static QStringList supportedLanguages();

    // 给人看的名字："cpp" → "C++"，"csharp" → "C#"
    static QString displayNameFor(const QString &language);

    // 围栏信息串 → 规范语言名。别名、大小写、前后空白都处理；
    // 不认识、或者明确表示"纯文本"（plaintext/text/none）时返回**空字符串**。
    static QString normalizeLanguage(const QString &info);

    static bool isSupported(const QString &language);

    // 把代码切成记号。返回值按 start 升序、互不重叠，且**不含 Plain** ——
    // 调用方只需从头到尾把"没被记号覆盖的部分"当普通文字输出。
    // 语言不认识/纯文本时返回空列表。
    static QVector<CodeToken> tokenize(const QString &code, const QString &language);

    // 代码 → 带 <span class="hljs-xxx"> 的 HTML。**内容一律转义**，
    // 所以代码里写 <script> 或 & 都不会破坏页面（这是安全底线，有测试钉着）。
    static QString highlightToHtml(const QString &code, const QString &language);

    // 把一整份 HTML 里所有 <pre><code class="language-X">…</code></pre> 就地换成高亮结果。
    // 没有语言名/语言不认识的代码块原样保留（缩进式代码块就没有语言名）。
    // 注意：只动代码块，行内 <code> 和正文一个字都不碰。
    static QString highlightCodeBlocks(const QString &html);

    // 记号种类 → CSS 类名。Plain 没有类名（返回空字符串）。
    static QString cssClassNameFor(CodeTokenKind kind);

    // HTML 转义/反转义。公开出来是因为"代码内容必须转义"这条要求
    // 在预览和导出两边都要成立，两边用的必须是同一个实现。
    static QString escapeHtml(const QString &text);
    static QString unescapeHtml(const QString &text);
};

}  // namespace markdown_editor::core::document

#endif // CODEHIGHLIGHTER_H
