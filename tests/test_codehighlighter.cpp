// CodeHighlighter（5.7 代码高亮）的契约测试。
//
// 不需要 QApplication：分词和拼 HTML 全是纯字符串处理 —— 这也是当初"不塞一个 JS 高亮库进
// qrc"的理由之一：高亮的每一条规则都能在这里被自动断言，不用靠人肉看预览。
//
// 测四层：
//   1. 语言识别：别名（py/PY/C++/cxx/js/ts/yml/sh/ps1/cs/rs/golang/kt/htm…）、大小写、
//      围栏里带额外信息（```cpp title=x）、不认识的/纯文本 → 不着色也不报错。
//   2. 分词：每种主流语言挑一段有代表性的代码，断言关键字/类型/字面量/字符串/数字/注释/
//      函数名/属性名这些记号真的被认出来了（这是"支持各种语言"的实际含义）。
//   3. 输出安全：代码里的 < > & " 必须转义 —— 否则代码块能把预览页面改写成别的东西。
//   4. 代码块替换：只动 <pre><code class="language-X">，不碰行内 <code> 和正文；
//      语言不认识/没有语言的块原样保留；多块都要处理。
//   另外还检查预览模板的 CSS 真的定义了高亮器产出的类名（高亮器 ↔ 样式的契约）。
//
// 跑法：ctest -C Debug --output-on-failure

#include "codehighlighter.h"
#include "markdownparser.h"

#include <QCoreApplication>
#include <QFile>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdio>

using markdown_editor::core::document::CodeHighlighter;
using markdown_editor::core::document::CodeToken;
using markdown_editor::core::document::CodeTokenKind;
using markdown_editor::core::document::MarkdownParser;

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-60s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// 把记号摊平成 "类名=原文 | 类名=原文" —— 一眼就能看出哪个词被认成了什么
QString tokensAsText(const QString &code, const QString &language)
{
    QStringList parts;
    const QVector<CodeToken> tokens = CodeHighlighter::tokenize(code, language);
    for (const CodeToken &token : tokens) {
        parts << QStringLiteral("%1=%2")
                     .arg(CodeHighlighter::cssClassNameFor(token.kind), code.mid(token.start, token.length));
    }
    return parts.join(QStringLiteral(" | "));
}

// 某个词（原文）在这段代码里有没有被认成指定种类
bool hasToken(const QString &code, const QString &language, CodeTokenKind kind, const QString &text)
{
    const QVector<CodeToken> tokens = CodeHighlighter::tokenize(code, language);
    for (const CodeToken &token : tokens) {
        if (token.kind == kind && code.mid(token.start, token.length) == text) {
            return true;
        }
    }
    return false;
}

QString kindNameFor(const QString &code, const QString &language, const QString &text)
{
    const QVector<CodeToken> tokens = CodeHighlighter::tokenize(code, language);
    for (const CodeToken &token : tokens) {
        if (code.mid(token.start, token.length) == text) {
            return CodeHighlighter::cssClassNameFor(token.kind);
        }
    }
    return QStringLiteral("(没有这个记号)");
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ============================ A. 语言识别 ============================
    {
        std::printf("---- A. 语言识别与别名 ----\n");

        const QStringList languages = CodeHighlighter::supportedLanguages();
        check(languages.size() >= 25, QStringLiteral("语言表: 至少 25 种主流语言"),
              QStringLiteral("%1 种").arg(languages.size()));
        for (const QString &expected : {QStringLiteral("cpp"), QStringLiteral("python"), QStringLiteral("javascript"),
                                        QStringLiteral("typescript"), QStringLiteral("java"), QStringLiteral("csharp"),
                                        QStringLiteral("rust"), QStringLiteral("go"), QStringLiteral("bash"),
                                        QStringLiteral("sql"), QStringLiteral("html"), QStringLiteral("css"),
                                        QStringLiteral("json"), QStringLiteral("yaml"), QStringLiteral("cmake"),
                                        QStringLiteral("dockerfile"), QStringLiteral("diff")}) {
            check(languages.contains(expected), QStringLiteral("语言表: 有 %1").arg(expected));
        }

        check(CodeHighlighter::normalizeLanguage(QStringLiteral("python")) == QStringLiteral("python"),
              QStringLiteral("别名: 规范名原样返回"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("PY")) == QStringLiteral("python"),
              QStringLiteral("别名: 大写也认"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("  py  ")) == QStringLiteral("python"),
              QStringLiteral("别名: 前后空白无所谓"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("C++")) == QStringLiteral("cpp"),
              QStringLiteral("别名: C++ -> cpp（md4c 会把围栏里的 C++ 原样放进 class）"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("cxx")) == QStringLiteral("cpp"),
              QStringLiteral("别名: cxx -> cpp"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("js")) == QStringLiteral("javascript"),
              QStringLiteral("别名: js -> javascript"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("ts")) == QStringLiteral("typescript"),
              QStringLiteral("别名: ts -> typescript"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("yml")) == QStringLiteral("yaml"),
              QStringLiteral("别名: yml -> yaml"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("sh")) == QStringLiteral("bash"),
              QStringLiteral("别名: sh -> bash"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("ps1")) == QStringLiteral("powershell"),
              QStringLiteral("别名: ps1 -> powershell"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("cs")) == QStringLiteral("csharp"),
              QStringLiteral("别名: cs -> csharp"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("rs")) == QStringLiteral("rust"),
              QStringLiteral("别名: rs -> rust"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("golang")) == QStringLiteral("go"),
              QStringLiteral("别名: golang -> go"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("kt")) == QStringLiteral("kotlin"),
              QStringLiteral("别名: kt -> kotlin"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("htm")) == QStringLiteral("html"),
              QStringLiteral("别名: htm -> html"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("docker")) == QStringLiteral("dockerfile"),
              QStringLiteral("别名: docker -> dockerfile"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("cpp title=x")) == QStringLiteral("cpp"),
              QStringLiteral("别名: 围栏信息串里有别的词也只取第一个"));

        // 不认识/纯文本：不着色，但也**不报错**（文档里写个冷门语言名不该让预览变丑或者弹错）
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("klingon")).isEmpty(),
              QStringLiteral("未知语言: 返回空（不报错）"));
        check(CodeHighlighter::normalizeLanguage(QString()).isEmpty(), QStringLiteral("空语言名: 返回空"));
        check(CodeHighlighter::normalizeLanguage(QStringLiteral("plaintext")).isEmpty(),
              QStringLiteral("纯文本: text/plaintext/none 明确表示不要高亮"));
        check(CodeHighlighter::tokenize(QStringLiteral("x = 1"), QStringLiteral("klingon")).isEmpty(),
              QStringLiteral("未知语言: 分词结果为空"));
        check(CodeHighlighter::highlightToHtml(QStringLiteral("x = 1"), QStringLiteral("klingon"))
                  == QStringLiteral("x = 1"),
              QStringLiteral("未知语言: 原样输出（还是要转义）"));
        check(CodeHighlighter::isSupported(QStringLiteral("cpp")) && !CodeHighlighter::isSupported(QStringLiteral("nope")),
              QStringLiteral("isSupported 与 normalizeLanguage 一致"));
        check(CodeHighlighter::displayNameFor(QStringLiteral("cpp")) == QStringLiteral("C++")
                  && CodeHighlighter::displayNameFor(QStringLiteral("csharp")) == QStringLiteral("C#"),
              QStringLiteral("显示名: cpp -> C++、csharp -> C#"));
    }

    // ============================ B. 分词：各种语言 ============================
    {
        std::printf("---- B. 各语言分词 ----\n");

        // ---- C++ ----
        {
            const QString code = QStringLiteral("int main() {\n    // 注释\n    std::string s = \"hi\";\n    return 0;\n}");
            check(hasToken(code, QStringLiteral("cpp"), CodeTokenKind::Type, QStringLiteral("int")),
                  QStringLiteral("C++: int 认成类型"));
            check(hasToken(code, QStringLiteral("cpp"), CodeTokenKind::Function, QStringLiteral("main")),
                  QStringLiteral("C++: main 认成函数名"));
            check(hasToken(code, QStringLiteral("cpp"), CodeTokenKind::Keyword, QStringLiteral("return")),
                  QStringLiteral("C++: return 认成关键字"));
            check(hasToken(code, QStringLiteral("cpp"), CodeTokenKind::Comment, QStringLiteral("// 注释")),
                  QStringLiteral("C++: 行注释"));
            check(hasToken(code, QStringLiteral("cpp"), CodeTokenKind::String, QStringLiteral("\"hi\"")),
                  QStringLiteral("C++: 字符串"));
            check(hasToken(code, QStringLiteral("cpp"), CodeTokenKind::Number, QStringLiteral("0")),
                  QStringLiteral("C++: 数字"));
            check(hasToken(code, QStringLiteral("cpp"), CodeTokenKind::Type, QStringLiteral("std::string"))
                      || kindNameFor(code, QStringLiteral("cpp"), QStringLiteral("string")) == QStringLiteral("hljs-type"),
                  QStringLiteral("C++: std::string 里的 string 认成类型"));
        }
        {
            const QString code = QStringLiteral("#include <stdio.h>\n#define MAX 10\n");
            check(hasToken(code, QStringLiteral("cpp"), CodeTokenKind::Meta, QStringLiteral("#include <stdio.h>")),
                  QStringLiteral("C++: 预处理指令整行认成 Meta"));
            check(hasToken(code, QStringLiteral("cpp"), CodeTokenKind::Meta, QStringLiteral("#define MAX 10")),
                  QStringLiteral("C++: #define 也是 Meta"));
        }

        // ---- C ----
        {
            const QString code = QStringLiteral("static size_t len = strlen(s); /* 块注释 */");
            check(hasToken(code, QStringLiteral("c"), CodeTokenKind::Keyword, QStringLiteral("static")),
                  QStringLiteral("C: static"));
            check(hasToken(code, QStringLiteral("c"), CodeTokenKind::Type, QStringLiteral("size_t")),
                  QStringLiteral("C: size_t 是类型"));
            check(hasToken(code, QStringLiteral("c"), CodeTokenKind::Builtin, QStringLiteral("strlen")),
                  QStringLiteral("C: strlen 是内置函数"));
            check(hasToken(code, QStringLiteral("c"), CodeTokenKind::Comment, QStringLiteral("/* 块注释 */")),
                  QStringLiteral("C: 块注释"));
        }

        // ---- Python ----
        {
            const QString code = QStringLiteral("@app.route(\"/\")\n"
                                                "def index(n=0):\n"
                                                "    \"\"\"多行\n"
                                                "    字符串\"\"\"\n"
                                                "    if n is None:\n"
                                                "        return True  # 注释\n"
                                                "    print(len(items))\n");
            check(hasToken(code, QStringLiteral("python"), CodeTokenKind::Meta, QStringLiteral("@app.route")),
                  QStringLiteral("Python: 装饰器认成 Meta"));
            check(hasToken(code, QStringLiteral("python"), CodeTokenKind::Keyword, QStringLiteral("def")),
                  QStringLiteral("Python: def"));
            check(hasToken(code, QStringLiteral("python"), CodeTokenKind::Function, QStringLiteral("index")),
                  QStringLiteral("Python: 函数名"));
            check(hasToken(code, QStringLiteral("python"), CodeTokenKind::String, QStringLiteral("\"\"\"多行\n    字符串\"\"\"")),
                  QStringLiteral("Python: 三引号字符串（跨行）"));
            check(hasToken(code, QStringLiteral("python"), CodeTokenKind::Literal, QStringLiteral("None"))
                      && hasToken(code, QStringLiteral("python"), CodeTokenKind::Literal, QStringLiteral("True")),
                  QStringLiteral("Python: None/True 认成字面量"));
            check(hasToken(code, QStringLiteral("python"), CodeTokenKind::Builtin, QStringLiteral("print")),
                  QStringLiteral("Python: print 认成内置"));
            check(hasToken(code, QStringLiteral("python"), CodeTokenKind::Comment, QStringLiteral("# 注释")),
                  QStringLiteral("Python: # 注释"));
        }

        // ---- JavaScript / TypeScript ----
        {
            const QString code = QStringLiteral("const f = async (x) => {\n"
                                                "  /* c */\n"
                                                "  let s = `模板${x}`;\n"
                                                "  return null;\n"
                                                "};");
            check(hasToken(code, QStringLiteral("javascript"), CodeTokenKind::Keyword, QStringLiteral("const")),
                  QStringLiteral("JS: const"));
            check(hasToken(code, QStringLiteral("javascript"), CodeTokenKind::Keyword, QStringLiteral("async")),
                  QStringLiteral("JS: async"));
            check(hasToken(code, QStringLiteral("javascript"), CodeTokenKind::String, QStringLiteral("`模板${x}`")),
                  QStringLiteral("JS: 反引号模板字符串"));
            check(hasToken(code, QStringLiteral("javascript"), CodeTokenKind::Literal, QStringLiteral("null")),
                  QStringLiteral("JS: null 是字面量"));
            const QString tsCode = QStringLiteral("interface A { x: number }\ntype B = A | null;");
            check(hasToken(tsCode, QStringLiteral("typescript"), CodeTokenKind::Keyword, QStringLiteral("interface")),
                  QStringLiteral("TypeScript: interface 是关键字（JS 里不是）"));
            check(!hasToken(tsCode, QStringLiteral("javascript"), CodeTokenKind::Keyword, QStringLiteral("interface")),
                  QStringLiteral("JavaScript: interface 不是关键字（两种语言的规则确实是分开的）"));
        }

        // ---- Rust / Go / Java / C# / Kotlin / Swift ----
        {
            const QString code = QStringLiteral("fn main() {\n    let mut v: Vec<i32> = vec![1, 2];\n    println!(\"hi\");\n}");
            check(hasToken(code, QStringLiteral("rust"), CodeTokenKind::Keyword, QStringLiteral("fn")),
                  QStringLiteral("Rust: fn"));
            check(hasToken(code, QStringLiteral("rust"), CodeTokenKind::Type, QStringLiteral("Vec")),
                  QStringLiteral("Rust: Vec 是类型"));
            check(hasToken(code, QStringLiteral("rust"), CodeTokenKind::Builtin, QStringLiteral("println"))
                      || hasToken(code, QStringLiteral("rust"), CodeTokenKind::Function, QStringLiteral("println")),
                  QStringLiteral("Rust: 宏 println! 被认出来了（内置或函数名）"),
                  kindNameFor(code, QStringLiteral("rust"), QStringLiteral("println")));
        }
        {
            const QString code = QStringLiteral("func main() {\n    defer wg.Done()\n    if err != nil { return }\n}");
            check(hasToken(code, QStringLiteral("go"), CodeTokenKind::Keyword, QStringLiteral("func")) &&
                      hasToken(code, QStringLiteral("go"), CodeTokenKind::Keyword, QStringLiteral("defer")),
                  QStringLiteral("Go: func/defer 是关键字"));
            check(hasToken(code, QStringLiteral("go"), CodeTokenKind::Literal, QStringLiteral("nil")),
                  QStringLiteral("Go: nil 是字面量"));
        }
        {
            const QString code = QStringLiteral("@Override\npublic class A extends B {\n  private final int x = 1;\n}");
            check(hasToken(code, QStringLiteral("java"), CodeTokenKind::Meta, QStringLiteral("@Override")),
                  QStringLiteral("Java: @Override 认成注解"));
            check(hasToken(code, QStringLiteral("java"), CodeTokenKind::Keyword, QStringLiteral("class"))
                      && hasToken(code, QStringLiteral("java"), CodeTokenKind::Keyword, QStringLiteral("extends")),
                  QStringLiteral("Java: class/extends"));
            check(hasToken(code, QStringLiteral("java"), CodeTokenKind::Type, QStringLiteral("int")),
                  QStringLiteral("Java: int 是类型"));
        }
        {
            const QString code = QStringLiteral("public async Task<int> RunAsync() { var x = await F(); return 0; }");
            check(hasToken(code, QStringLiteral("csharp"), CodeTokenKind::Keyword, QStringLiteral("async")),
                  QStringLiteral("C#: async"));
            check(hasToken(code, QStringLiteral("csharp"), CodeTokenKind::Type, QStringLiteral("Task")),
                  QStringLiteral("C#: Task 是类型"));
            check(hasToken(code, QStringLiteral("csharp"), CodeTokenKind::Keyword, QStringLiteral("var")),
                  QStringLiteral("C#: var"));
        }
        {
            const QString code = QStringLiteral("fun main() {\n    val items = listOf(1, 2)\n    println(items.size)\n}");
            check(hasToken(code, QStringLiteral("kotlin"), CodeTokenKind::Keyword, QStringLiteral("fun"))
                      && hasToken(code, QStringLiteral("kotlin"), CodeTokenKind::Keyword, QStringLiteral("val")),
                  QStringLiteral("Kotlin: fun/val"));
            check(hasToken(code, QStringLiteral("kotlin"), CodeTokenKind::Builtin, QStringLiteral("listOf"))
                      || hasToken(code, QStringLiteral("kotlin"), CodeTokenKind::Function, QStringLiteral("listOf")),
                  QStringLiteral("Kotlin: listOf 被认出来了"),
                  kindNameFor(code, QStringLiteral("kotlin"), QStringLiteral("listOf")));
        }
        {
            const QString code = QStringLiteral("guard let x = y else { return }\nlet s: String = \"a\"");
            check(hasToken(code, QStringLiteral("swift"), CodeTokenKind::Keyword, QStringLiteral("guard"))
                      && hasToken(code, QStringLiteral("swift"), CodeTokenKind::Keyword, QStringLiteral("let")),
                  QStringLiteral("Swift: guard/let"));
            check(hasToken(code, QStringLiteral("swift"), CodeTokenKind::Type, QStringLiteral("String")),
                  QStringLiteral("Swift: String 是类型"));
        }

        // ---- 脚本类 ----
        {
            const QString code = QStringLiteral("#!/bin/bash\nexport PATH=$PATH:/opt/bin\nif [ -f \"$HOME/a.txt\" ]; then\n"
                                                "    echo \"found\"  # 注释\nfi");
            check(hasToken(code, QStringLiteral("bash"), CodeTokenKind::Comment, QStringLiteral("#!/bin/bash")),
                  QStringLiteral("Shell: shebang 也是注释"));
            check(hasToken(code, QStringLiteral("bash"), CodeTokenKind::Keyword, QStringLiteral("if"))
                      && hasToken(code, QStringLiteral("bash"), CodeTokenKind::Keyword, QStringLiteral("fi")),
                  QStringLiteral("Shell: if/fi"));
            check(hasToken(code, QStringLiteral("bash"), CodeTokenKind::Attribute, QStringLiteral("$PATH"))
                      || hasToken(code, QStringLiteral("bash"), CodeTokenKind::Attribute, QStringLiteral("$HOME")),
                  QStringLiteral("Shell: $变量认成属性"));
        }
        {
            const QString code = QStringLiteral("function Get-Thing {\n    param($Name)\n    Write-Host \"hi\"  # 注释\n}");
            check(hasToken(code, QStringLiteral("powershell"), CodeTokenKind::Keyword, QStringLiteral("function")),
                  QStringLiteral("PowerShell: function"));
            check(hasToken(code, QStringLiteral("powershell"), CodeTokenKind::Builtin, QStringLiteral("Write-Host")),
                  QStringLiteral("PowerShell: Write-Host（带连字符的名字也要认）"));
        }
        {
            const QString code = QStringLiteral("puts \"hello\"  # 注释\nitems.each { |x| print x }");
            check(hasToken(code, QStringLiteral("ruby"), CodeTokenKind::Keyword, QStringLiteral("puts")) == false,
                  QStringLiteral("Ruby: puts 不是关键字（它是内置方法，走 Builtin）"),
                  kindNameFor(code, QStringLiteral("ruby"), QStringLiteral("puts")));
            check(hasToken(code, QStringLiteral("ruby"), CodeTokenKind::Builtin, QStringLiteral("print")),
                  QStringLiteral("Ruby: print 是内置"));
        }
        {
            const QString code = QStringLiteral("<?php\n$name = \"x\";\nif ($name) { echo strlen($name); }\n?>");
            check(hasToken(code, QStringLiteral("php"), CodeTokenKind::Attribute, QStringLiteral("$name")),
                  QStringLiteral("PHP: $name 认成属性（变量）"));
            check(hasToken(code, QStringLiteral("php"), CodeTokenKind::Keyword, QStringLiteral("echo")),
                  QStringLiteral("PHP: echo"));
        }

        // ---- SQL ----
        {
            const QString code = QStringLiteral("SELECT id, COUNT(*) FROM t\nWHERE name LIKE 'a%' -- 注释\nGROUP BY id;");
            check(hasToken(code, QStringLiteral("sql"), CodeTokenKind::Keyword, QStringLiteral("SELECT")),
                  QStringLiteral("SQL: 大写关键字"));
            check(hasToken(QStringLiteral("select id from t"), QStringLiteral("sql"),
                           CodeTokenKind::Keyword, QStringLiteral("select")),
                  QStringLiteral("SQL: 小写也认（大小写不敏感）"));
            check(hasToken(code, QStringLiteral("sql"), CodeTokenKind::Comment, QStringLiteral("-- 注释")),
                  QStringLiteral("SQL: -- 注释"));
            check(hasToken(code, QStringLiteral("sql"), CodeTokenKind::String, QStringLiteral("'a%'")),
                  QStringLiteral("SQL: 单引号字符串"));
        }

        // ---- 标记 / 样式 ----
        {
            const QString code = QStringLiteral("<div class=\"box\" id='main'>\n    <!-- 注释 -->\n    <span>&amp;</span>\n</div>");
            check(hasToken(code, QStringLiteral("html"), CodeTokenKind::Tag, QStringLiteral("div")),
                  QStringLiteral("HTML: 标签名"));
            check(hasToken(code, QStringLiteral("html"), CodeTokenKind::Attribute, QStringLiteral("class")),
                  QStringLiteral("HTML: 属性名"));
            check(hasToken(code, QStringLiteral("html"), CodeTokenKind::String, QStringLiteral("\"box\"")),
                  QStringLiteral("HTML: 属性值"));
            check(hasToken(code, QStringLiteral("html"), CodeTokenKind::Comment, QStringLiteral("<!-- 注释 -->")),
                  QStringLiteral("HTML: 注释"));
            check(hasToken(code, QStringLiteral("html"), CodeTokenKind::Literal, QStringLiteral("&amp;")),
                  QStringLiteral("HTML: 实体"));
        }
        {
            const QString code = QStringLiteral("@media (max-width: 600px) {\n  .box #main { color: #fff; margin: 8px auto; }\n}");
            check(hasToken(code, QStringLiteral("css"), CodeTokenKind::Meta, QStringLiteral("@media")),
                  QStringLiteral("CSS: @规则"));
            check(hasToken(code, QStringLiteral("css"), CodeTokenKind::Tag, QStringLiteral("box")),
                  QStringLiteral("CSS: 类选择器"));
            check(hasToken(code, QStringLiteral("css"), CodeTokenKind::Attribute, QStringLiteral("color")),
                  QStringLiteral("CSS: 属性名"));
            check(hasToken(code, QStringLiteral("css"), CodeTokenKind::Number, QStringLiteral("#fff")),
                  QStringLiteral("CSS: 颜色当数字（#fff 不是选择器）"));
            check(hasToken(code, QStringLiteral("css"), CodeTokenKind::Number, QStringLiteral("8px")),
                  QStringLiteral("CSS: 带单位的数字"));
        }

        // ---- 数据 / 配置 ----
        {
            const QString code = QStringLiteral("{\n  \"name\": \"muse\",\n  \"count\": 3,\n  \"ok\": true,\n  \"none\": null\n}");
            check(hasToken(code, QStringLiteral("json"), CodeTokenKind::Attribute, QStringLiteral("\"count\"")),
                  QStringLiteral("JSON: 键认成属性（不是字符串）"));
            check(hasToken(code, QStringLiteral("json"), CodeTokenKind::String, QStringLiteral("\"muse\"")),
                  QStringLiteral("JSON: 值还是字符串"));
            check(hasToken(code, QStringLiteral("json"), CodeTokenKind::Literal, QStringLiteral("true"))
                      && hasToken(code, QStringLiteral("json"), CodeTokenKind::Literal, QStringLiteral("null")),
                  QStringLiteral("JSON: true/null 是字面量"));
        }
        {
            const QString code = QStringLiteral("# 注释\nname: muse\nlist:\n  - one\n  - two\nflat: {a: 1}");
            check(hasToken(code, QStringLiteral("yaml"), CodeTokenKind::Comment, QStringLiteral("# 注释")),
                  QStringLiteral("YAML: # 注释"));
            check(hasToken(code, QStringLiteral("yaml"), CodeTokenKind::Attribute, QStringLiteral("name")),
                  QStringLiteral("YAML: 键"));
            check(hasToken(code, QStringLiteral("yaml"), CodeTokenKind::Operator, QStringLiteral("-")),
                  QStringLiteral("YAML: 列表项的 - "));
        }
        {
            const QString code = QStringLiteral("[package]\nname = \"muse\"\nversion = \"0.1\"  # 注释");
            check(hasToken(code, QStringLiteral("toml"), CodeTokenKind::Tag, QStringLiteral("package")),
                  QStringLiteral("TOML: [section]"));
            check(hasToken(code, QStringLiteral("toml"), CodeTokenKind::Attribute, QStringLiteral("version")),
                  QStringLiteral("TOML: 键"));
            check(hasToken(code, QStringLiteral("toml"), CodeTokenKind::Comment, QStringLiteral("# 注释")),
                  QStringLiteral("TOML: 注释"));
        }
        {
            const QString code = QStringLiteral("; 注释\n[core]\n  editor = vim\n  autocrlf = true");
            check(hasToken(code, QStringLiteral("ini"), CodeTokenKind::Comment, QStringLiteral("; 注释")),
                  QStringLiteral("INI: ; 注释"));
            check(hasToken(code, QStringLiteral("ini"), CodeTokenKind::Tag, QStringLiteral("core")),
                  QStringLiteral("INI: [section]"));
            check(hasToken(code, QStringLiteral("ini"), CodeTokenKind::Attribute, QStringLiteral("editor")),
                  QStringLiteral("INI: 键"));
        }
        {
            const QString code = QStringLiteral("cmake_minimum_required(VERSION 3.24)\ntarget_link_libraries(app PRIVATE Qt6::Core)\n"
                                                "message(${CMAKE_SOURCE_DIR})");
            check(hasToken(code, QStringLiteral("cmake"), CodeTokenKind::Keyword, QStringLiteral("cmake_minimum_required")),
                  QStringLiteral("CMake: 命令名（当关键字着色）"));
            check(hasToken(code, QStringLiteral("cmake"), CodeTokenKind::Type, QStringLiteral("PRIVATE")),
                  QStringLiteral("CMake: PRIVATE 是类型/范围词"));
            check(hasToken(code, QStringLiteral("cmake"), CodeTokenKind::Attribute, QStringLiteral("${CMAKE_SOURCE_DIR}")),
                  QStringLiteral("CMake: ${变量}"));
        }
        {
            const QString code = QStringLiteral("FROM ubuntu:22.04 AS base\nRUN apt-get update \\\n    && apt-get install -y git");
            check(hasToken(code, QStringLiteral("dockerfile"), CodeTokenKind::Keyword, QStringLiteral("FROM")),
                  QStringLiteral("Dockerfile: FROM"));
            check(hasToken(code, QStringLiteral("dockerfile"), CodeTokenKind::Keyword, QStringLiteral("AS")),
                  QStringLiteral("Dockerfile: AS（大小写不敏感）"));
        }
        {
            const QString code = QStringLiteral("--- a/x.md\n+++ b/x.md\n@@ -1,3 +1,4 @@\n-old\n+new\n context");
            check(hasToken(code, QStringLiteral("diff"), CodeTokenKind::DiffAdd, QStringLiteral("+new")),
                  QStringLiteral("Diff: 新增行"));
            check(hasToken(code, QStringLiteral("diff"), CodeTokenKind::DiffDel, QStringLiteral("-old")),
                  QStringLiteral("Diff: 删除行"));
            check(hasToken(code, QStringLiteral("diff"), CodeTokenKind::Meta, QStringLiteral("@@ -1,3 +1,4 @@")),
                  QStringLiteral("Diff: hunk 头是 Meta"));
            check(hasToken(code, QStringLiteral("diff"), CodeTokenKind::Meta, QStringLiteral("+++ b/x.md")),
                  QStringLiteral("Diff: 文件头（+++）是 Meta，不是新增行"));
        }

        // 记号必须有序、不重叠（否则拼 HTML 会错位）
        {
            const QString code = QStringLiteral("int a = 1; // c\n\"s\"\n");
            const QVector<CodeToken> tokens = CodeHighlighter::tokenize(code, QStringLiteral("cpp"));
            bool ordered = true;
            int lastEnd = 0;
            for (const CodeToken &token : tokens) {
                if (token.start < lastEnd || token.length <= 0) {
                    ordered = false;
                    break;
                }
                lastEnd = token.start + token.length;
            }
            check(ordered && !tokens.isEmpty(), QStringLiteral("记号: 有序、互不重叠、长度为正"),
                  QStringLiteral("%1 个记号").arg(tokens.size()));
        }
    }

    // ============================ C. 输出与安全 ============================
    {
        std::printf("---- C. 输出与转义 ----\n");

        // ★ 安全底线：任何语言下，代码里的 HTML 特殊字符都必须被转义
        const QString nasty = QStringLiteral("<script>alert(\"x\")</script> & <b>");
        const QString escaped = CodeHighlighter::highlightToHtml(nasty, QStringLiteral("klingon"));
        check(!escaped.contains(QStringLiteral("<script")), QStringLiteral("安全: 代码里的 <script> 不会原样进 HTML"));
        check(escaped.contains(QStringLiteral("&lt;script&gt;")), QStringLiteral("安全: 尖括号被转义"),
              escaped.left(40));
        check(escaped.contains(QStringLiteral("&amp;")), QStringLiteral("安全: & 被转义"));
        check(escaped.contains(QStringLiteral("&quot;")), QStringLiteral("安全: 双引号被转义"));

        // 高亮之后也一样：span 是我们加的，代码内容依然全转义
        const QString html = CodeHighlighter::highlightToHtml(QStringLiteral("x = \"<b>&\""), QStringLiteral("python"));
        check(html.contains(QStringLiteral("<span class=\"hljs-string\">")), QStringLiteral("高亮: 字符串有 span"));
        check(!html.contains(QStringLiteral("<b>")), QStringLiteral("高亮: 字符串里的 <b> 依然是转义过的"));

        check(CodeHighlighter::escapeHtml(QStringLiteral("a<b>&\"c\"")) == QStringLiteral("a&lt;b&gt;&amp;&quot;c&quot;"),
              QStringLiteral("escapeHtml: 四个字符都转"));
        const QString roundTrip = QStringLiteral("a<b>&\"c\"'d' &amp; &#39; &#x4e2d;");
        check(CodeHighlighter::unescapeHtml(CodeHighlighter::escapeHtml(roundTrip)) == roundTrip,
              QStringLiteral("unescapeHtml: 与 escapeHtml 能来回"),
              CodeHighlighter::unescapeHtml(CodeHighlighter::escapeHtml(roundTrip)));
        check(CodeHighlighter::unescapeHtml(QStringLiteral("&lt;p&gt; &amp; &quot;q&quot;")) == QStringLiteral("<p> & \"q\""),
              QStringLiteral("unescapeHtml: 认识 md4c 会产出的实体"));
        check(CodeHighlighter::unescapeHtml(QStringLiteral("&unknown; x")) == QStringLiteral("&unknown; x"),
              QStringLiteral("unescapeHtml: 不认识的实体原样保留"));

        // 每个种类都有独立的类名（除了 Plain 没有）
        {
            const QVector<CodeTokenKind> kinds = {CodeTokenKind::Comment, CodeTokenKind::Keyword,   CodeTokenKind::Type,
                                                  CodeTokenKind::Literal, CodeTokenKind::Builtin,   CodeTokenKind::String,
                                                  CodeTokenKind::Number,  CodeTokenKind::Function,  CodeTokenKind::Attribute,
                                                  CodeTokenKind::Tag,     CodeTokenKind::Operator,  CodeTokenKind::Meta,
                                                  CodeTokenKind::DiffAdd, CodeTokenKind::DiffDel};
            QSet<QString> names;
            bool allNamed = true;
            for (CodeTokenKind kind : kinds) {
                const QString name = CodeHighlighter::cssClassNameFor(kind);
                if (name.isEmpty() || names.contains(name)) {
                    allNamed = false;
                }
                names.insert(name);
            }
            check(allNamed && names.size() == kinds.size(),
                  QStringLiteral("类名: 每种记号都有互不相同的类名"), QStringLiteral("%1 个").arg(names.size()));
            check(CodeHighlighter::cssClassNameFor(CodeTokenKind::Plain).isEmpty(),
                  QStringLiteral("类名: Plain 没有类名（不会产生空 span）"));
        }

        // 预览模板的 CSS 必须定义了这些类名（高亮器 ↔ 样式的契约）
        {
            QFile file(QStringLiteral(":/html/preview_template.html"));
            const bool opened = file.open(QIODevice::ReadOnly);
            const QString templateHtml = opened ? QString::fromUtf8(file.readAll()) : QString();
            check(opened, QStringLiteral("样式: 能打开预览模板（qrc 接进来了）"));
            const QStringList needed = {QStringLiteral(".hljs-comment"),   QStringLiteral(".hljs-keyword"),
                                        QStringLiteral(".hljs-type"),      QStringLiteral(".hljs-literal"),
                                        QStringLiteral(".hljs-built_in"),  QStringLiteral(".hljs-string"),
                                        QStringLiteral(".hljs-number"),    QStringLiteral(".hljs-function"),
                                        QStringLiteral(".hljs-attr"),      QStringLiteral(".hljs-tag"),
                                        QStringLiteral(".hljs-operator"),  QStringLiteral(".hljs-meta"),
                                        QStringLiteral(".hljs-diff-add"),  QStringLiteral(".hljs-diff-del")};
            QStringList missing;
            for (const QString &selector : needed) {
                if (!templateHtml.contains(selector)) {
                    missing << selector;
                }
            }
            check(missing.isEmpty(),
                  QStringLiteral("样式: 预览模板里定义了全部 %1 个高亮类名（否则高亮等于没颜色）").arg(needed.size()),
                  missing.join(QStringLiteral(", ")));
        }
    }

    // ============================ D. 代码块就地替换 ============================
    {
        std::printf("---- D. 代码块替换 ----\n");

        const QString single = QStringLiteral("<pre><code class=\"language-python\">print(1)\n</code></pre>\n");
        const QString replaced = CodeHighlighter::highlightCodeBlocks(single);
        check(replaced.contains(QStringLiteral("class=\"language-python hljs\"")),
              QStringLiteral("替换: class 保留 language-python 并加上 hljs"));
        check(replaced.contains(QStringLiteral("<span class=\"hljs-built_in\">print</span>")),
              QStringLiteral("替换: print 被包上 span"), replaced);

        // 没有语言名（缩进式代码块）→ 一个字都不动
        const QString noLang = QStringLiteral("<pre><code>just text\n</code></pre>");
        check(CodeHighlighter::highlightCodeBlocks(noLang) == noLang,
              QStringLiteral("替换: 没有语言名的代码块原样保留"));

        // 不认识的语言 → 原样保留（不报错、不破坏）
        const QString unknown = QStringLiteral("<pre><code class=\"language-klingon\">x &amp; y\n</code></pre>");
        check(CodeHighlighter::highlightCodeBlocks(unknown) == unknown,
              QStringLiteral("替换: 不认识的语言原样保留"));

        // 行内 <code> 与正文不许被碰
        const QString inlineCode = QStringLiteral("<p>这是 <code>inline</code> 和 <code>x=1</code> 文字</p>");
        check(CodeHighlighter::highlightCodeBlocks(inlineCode) == inlineCode,
              QStringLiteral("替换: 行内代码和正文一个字都不动"));

        // 多个代码块都要处理，且顺序/其它内容不变
        const QString twoBlocks = QStringLiteral("<p>前</p>\n"
                                                "<pre><code class=\"language-cpp\">int a;</code></pre>\n"
                                                "<p>中</p>\n"
                                                "<pre><code class=\"language-json\">{\"k\": 1}</code></pre>\n"
                                                "<p>后</p>\n");
        const QString twoReplaced = CodeHighlighter::highlightCodeBlocks(twoBlocks);
        check(twoReplaced.count(QStringLiteral("hljs\">")) == 2,
              QStringLiteral("替换: 两个代码块都处理了"),
              QStringLiteral("%1 个").arg(twoReplaced.count(QStringLiteral("hljs\">"))));
        check(twoReplaced.startsWith(QStringLiteral("<p>前</p>")) && twoReplaced.endsWith(QStringLiteral("<p>后</p>\n")),
              QStringLiteral("替换: 代码块之外的顺序与内容不变"));

        // ★ 端到端：真的从 Markdown 走一遍（这才是预览和导出实际走的路）
        const QString markdown = QStringLiteral("# 标题\n\n"
                                                "普通段落和行内 `code`。\n\n"
                                                "```cpp\n"
                                                "int main() { return 0; }  // 注释\n"
                                                "```\n\n"
                                                "```\n"
                                                "没有语言名，不该着色\n"
                                                "```\n");
        const QString rendered = CodeHighlighter::highlightCodeBlocks(MarkdownParser::parseToHtml(markdown));
        check(rendered.contains(QStringLiteral("<h1>标题</h1>")), QStringLiteral("端到端: 标题正常渲染"));
        check(rendered.contains(QStringLiteral("hljs-type\">int</span>")), QStringLiteral("端到端: cpp 块里的 int 上了色"),
              rendered.mid(rendered.indexOf(QStringLiteral("<pre>")), 120));
        check(rendered.contains(QStringLiteral("hljs-comment\">// 注释</span>")), QStringLiteral("端到端: 注释上了色"));
        check(rendered.contains(QStringLiteral("hljs-function\">main</span>")), QStringLiteral("端到端: 函数名上了色"));
        check(rendered.contains(QStringLiteral("<code>code</code>")), QStringLiteral("端到端: 行内代码没被碰"));
        check(rendered.contains(QStringLiteral("没有语言名，不该着色")), QStringLiteral("端到端: 无语言名的块保留原样"));
        check(!rendered.contains(QStringLiteral("language- hljs")), QStringLiteral("端到端: 空语言名不会产生假 class"));

        // 顶层块数量不能因为高亮而改变（同步用的行号映射靠它对齐）
        const int topLevelBlocks = rendered.count(QStringLiteral("<pre>")) + rendered.count(QStringLiteral("<h1>"))
                                   + rendered.count(QStringLiteral("<p>"));
        check(topLevelBlocks == 4, QStringLiteral("端到端: 顶层块数量不变（行号同步不受影响）"),
              QStringLiteral("%1 个").arg(topLevelBlocks));
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
