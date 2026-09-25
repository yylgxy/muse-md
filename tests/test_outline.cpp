// MarkdownOutline（C4 大纲）的纯函数契约测试。
//
// ★ 这个测试**故意不链接 Qt6::Widgets、不需要 QApplication**：MarkdownOutline 是
//   core/document 里的纯函数（规则类逻辑），所以它能毫秒级跑完、不拉 Chromium。
//   这就是"规则下沉到 core、界面留在 business"这条分工的实际回报 ——
//   同一份规则要是在面板里实现，这个测试就得起一个窗口。
//
// 用例清单对着路线图 C4.2 Step 2 的 11 条，每条规则都钉一个断言；
// 另外补了围栏判据本身（isFenceLine）和高亮器共用同一份的检查。

#include "markdownoutline.h"

#include <QString>
#include <QStringList>

#include <cstdio>

using markdown_editor::core::document::MarkdownOutline;
using markdown_editor::core::document::OutlineItem;

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-62s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// 把结果压成 "level:line:title" 便于一眼比对
QStringList digest(const QList<OutlineItem> &items)
{
    QStringList out;
    for (const OutlineItem &item : items) {
        out << QStringLiteral("%1:%2:%3").arg(item.level).arg(item.line).arg(item.title);
    }
    return out;
}

}  // namespace

int main()
{
    // ============================ A. 基本识别 ============================
    {
        const auto items = MarkdownOutline::extract(QStringLiteral("# A\n## B\n### C"));
        check(digest(items) == QStringList({QStringLiteral("1:1:A"),
                                            QStringLiteral("2:2:B"),
                                            QStringLiteral("3:3:C")}),
              QStringLiteral("1) 三级标题：层级与行号都对"), digest(items).join(QStringLiteral(" | ")));
    }

    {
        // `#无空格` 不是标题（CommonMark：井号后必须有空白）
        check(MarkdownOutline::extract(QStringLiteral("#无空格")).isEmpty(),
              QStringLiteral("2) 「#无空格」不是标题（井号后必须有空白）"));
        // 但 `# 有空格` 是
        check(MarkdownOutline::extract(QStringLiteral("# 有空格")).size() == 1,
              QStringLiteral("2b) 「# 有空格」是标题"));
    }

    {
        check(MarkdownOutline::extract(QStringLiteral("####### 七级")).isEmpty(),
              QStringLiteral("3) 七个 # 不是标题（最多 6 级）"));
        check(MarkdownOutline::extract(QStringLiteral("###### 六级的可以")).size() == 1,
              QStringLiteral("3b) 六个 # 是合法的第 6 级"));
    }

    // ============================ B. 围栏代码块 ============================
    {
        const auto backtick = MarkdownOutline::extract(
            QStringLiteral("```\n# 不是标题\n```\n# 是标题"));
        check(digest(backtick) == QStringList({QStringLiteral("1:4:是标题")}),
              QStringLiteral("4) ★反引号围栏里的 # 被排除，围栏之后的算"), digest(backtick).join(QStringLiteral(" | ")));

        const auto tilde = MarkdownOutline::extract(
            QStringLiteral("~~~\n# 不是标题\n~~~\n# 是标题"));
        check(digest(tilde) == QStringList({QStringLiteral("1:4:是标题")}),
              QStringLiteral("5) ★波浪线围栏同样管用"), digest(tilde).join(QStringLiteral(" | ")));
    }

    {
        // 带语言的围栏（```cpp）也要认；围栏自己那行不是标题
        const auto items = MarkdownOutline::extract(
            QStringLiteral("```cpp\n# 注释不是标题\n```\n# 真的标题"));
        check(digest(items) == QStringList({QStringLiteral("1:4:真的标题")}),
              QStringLiteral("5b) ```cpp 这种带语言的围栏也认"), digest(items).join(QStringLiteral(" | ")));
    }

    {
        // 围栏没闭合 → 后面全是代码，标题一个都不该冒出来。
        // （这是"少报"而不是"多报"：宁可大纲里少一条，也不要把代码里的井号当标题跳过去。）
        const auto items = MarkdownOutline::extract(QStringLiteral("# 开头这个算\n```\n# 后面的不算"));
        check(digest(items) == QStringList({QStringLiteral("1:1:开头这个算")}),
              QStringLiteral("5c) 围栏没闭合 -> 后面全当代码，不再出标题"),
              digest(items).join(QStringLiteral(" | ")));
    }

    // ============================ C. 闭合井号与缩进 ============================
    {
        const auto items = MarkdownOutline::extract(QStringLiteral("## 标题 ##"));
        check(items.size() == 1 && items.first().title == QStringLiteral("标题"),
              QStringLiteral("6) 结尾的闭合 # 被剥掉"), items.isEmpty() ? QString() : items.first().title);
    }

    {
        // 结尾的 # 前面不是空白 → 那是标题文本的一部分，不能剥
        const auto items = MarkdownOutline::extract(QStringLiteral("# C# 语言"));
        check(items.size() == 1 && items.first().title == QStringLiteral("C# 语言"),
              QStringLiteral("6b) 「C# 语言」结尾的 # 不是闭合序列，保留"),
              items.isEmpty() ? QString() : items.first().title);
    }

    {
        const auto three = MarkdownOutline::extract(QStringLiteral("   ### 缩进三级"));
        check(three.size() == 1 && three.first().level == 3 && three.first().title == QStringLiteral("缩进三级"),
              QStringLiteral("7) 行首 3 个空格仍算标题"),
              three.isEmpty() ? QString() : QStringLiteral("level=%1").arg(three.first().level));

        check(MarkdownOutline::extract(QStringLiteral("    ### 缩进四级")).isEmpty(),
              QStringLiteral("7b) 行首 4 个空格 = 缩进代码块，不是标题"));
    }

    // ============================ D. 标题文本 ============================
    {
        // 行内标记原样保留：大纲显示的是源码文本，这样"点进去看到的那一行"和"列表里写的"
        // 是同一个东西（剥掉 `**` 反而会对不上）。
        const auto items = MarkdownOutline::extract(QStringLiteral("# 用 `#` 和 **粗体** 的标题"));
        check(items.size() == 1 && items.first().title == QStringLiteral("用 `#` 和 **粗体** 的标题"),
              QStringLiteral("8) 标题里的行内标记原样保留、不被截断"),
              items.isEmpty() ? QString() : items.first().title);
    }

    {
        // 空标题：`#` 后面只有空白 —— 策略是跳过（见 markdownoutline.cpp 里的注释）
        check(MarkdownOutline::extract(QStringLiteral("#")).isEmpty(),
              QStringLiteral("9) 裸 # 不是标题（后面没空白）"));
        check(MarkdownOutline::extract(QStringLiteral("#   ")).isEmpty(),
              QStringLiteral("9b) 只有 # 和空白 -> 空标题，跳过（不给一个点了会跳的空白条目）"));
        check(MarkdownOutline::extract(QStringLiteral("#  ###")).isEmpty(),
              QStringLiteral("9c) 整行都是 # -> 剥完是空标题，同样跳过"));
    }

    // ============================ E. 换行符与行号 ============================
    {
        // \r\n 输入：行号必须还是对的（不能因为 \r 多算一行，也不能让它混进标题）
        const auto items = MarkdownOutline::extract(QStringLiteral("# A\r\n正文\r\n## B\r\n### C"));
        check(digest(items) == QStringList({QStringLiteral("1:1:A"),
                                            QStringLiteral("2:3:B"),
                                            QStringLiteral("3:4:C")}),
              QStringLiteral("10) ★\\r\\n 文档：层级与行号都正确"), digest(items).join(QStringLiteral(" | ")));
    }

    {
        // 行号从 1 起算：第一个标题就在第 1 行（0 起算的话这里会是 0）
        const auto items = MarkdownOutline::extract(QStringLiteral("# 第一行就是标题"));
        check(items.size() == 1 && items.first().line == 1,
              QStringLiteral("10b) 行号从 1 起算（和 goToLine / 状态栏同一套）"));
    }

    // ============================ F. 规模 ============================
    {
        // 10 万行里只放 3 个标题：既验"只报这 3 个"，也顺带压一下性能
        QStringList lines;
        lines.reserve(100001);
        lines << QStringLiteral("# 一");
        for (int i = 0; i < 99999; ++i) {
            lines << QStringLiteral("普通正文一行，不是标题");
        }
        lines << QStringLiteral("## 二");
        lines << QStringLiteral("### 三");
        const auto items = MarkdownOutline::extract(lines.join(QLatin1Char('\n')));
        check(items.size() == 3, QStringLiteral("11) 10 万行里恰好 3 个标题"),
              QStringLiteral("size=%1").arg(items.size()));
        // 行号从 lines.size() 推，不要自己心算：我第一次就是手数错了 1 行
        //（"# 一" + 99999 行正文 + 2 个标题 = 100002 行，最后一行是第 100002 行）。
        check(!items.isEmpty() && items.last().line == lines.size(),
              QStringLiteral("11b) 最后那条的行号 == 总行数（换行处理没让它偏一行）"),
              items.isEmpty() ? QString() : QStringLiteral("line=%1 总行数=%2").arg(items.last().line).arg(lines.size()));
    }

    // ============================ G. 围栏判据（与高亮器共用同一份）============================
    {
        check(MarkdownOutline::isFenceLine(QStringLiteral("```")),
              QStringLiteral("12) isFenceLine: 裸 ``` 是围栏"));
        check(MarkdownOutline::isFenceLine(QStringLiteral("~~~")),
              QStringLiteral("12b) isFenceLine: 裸 ~~~ 是围栏"));
        check(MarkdownOutline::isFenceLine(QStringLiteral("    ```cpp")),
              QStringLiteral("12c) isFenceLine: 允许行首缩进 + 语言标注"));
        check(!MarkdownOutline::isFenceLine(QStringLiteral("``")),
              QStringLiteral("12d) isFenceLine: 两个反引号不是围栏（行内代码）"));
        check(!MarkdownOutline::isFenceLine(QStringLiteral("文本 ```")),
              QStringLiteral("12e) isFenceLine: 不在行首不算围栏"));
        check(!MarkdownOutline::isFenceLine(QString()),
              QStringLiteral("12f) isFenceLine: 空行不是围栏"));
        check(!MarkdownOutline::isFenceLine(QStringLiteral("~~ 两个波浪线")),
              QStringLiteral("12g) isFenceLine: 两个波浪线不算"));
        // ★ 边界（与高亮器**一致**的语义，不是大纲独有的）：
        //   围栏的关闭不需要和开启用同一个符号 —— `~~~` 能把 ``` 开的块关掉。
        //   理由是"两份判据必须一致"：高亮器原本就是"见到围栏行就翻转状态"，
        //   大纲要是自己加一条"关闭必须同符号"的规则，两边就会在这类文档上打架
        //   （代码被高亮成代码、大纲却把它当标题）。混用符号的文档很罕见，
        //   这种写法下两边表现一致比"单方面更正确"更重要 —— 这条取舍写进头文件的已知边界。
        const auto items = MarkdownOutline::extract(
            QStringLiteral("```\n### 不是\n~~~\n### 还是不是\n```\n### 这个算了"));
        check(digest(items) == QStringList({QStringLiteral("3:4:还是不是")}),
              QStringLiteral("12h) 混用围栏符号：~~~ 关掉了 ``` 的块（与高亮器同一语义）"),
              digest(items).join(QStringLiteral(" | ")));
    }

    // ============================ H. 缩进与显示 ============================
    {
        check(MarkdownOutline::indentForLevel(1) == 0 && MarkdownOutline::indentForLevel(2) == 14
                  && MarkdownOutline::indentForLevel(6) == 70,
              QStringLiteral("13) 缩进：每层 14px（第 1 层 0）"),
              QStringLiteral("L1=%1 L2=%2 L6=%3")
                  .arg(MarkdownOutline::indentForLevel(1))
                  .arg(MarkdownOutline::indentForLevel(2))
                  .arg(MarkdownOutline::indentForLevel(6)));
        // 越界的层级夹住，不崩也不返回负数（面板是显示用的，不该因为一个意外数字整块不画）
        check(MarkdownOutline::indentForLevel(0) == 0 && MarkdownOutline::indentForLevel(99) == 70,
              QStringLiteral("13b) 缩进：越界层级被夹到 1–6"));
    }

    {
        OutlineItem item;
        item.title = QStringLiteral("短标题");
        check(MarkdownOutline::displayTextFor(item) == QStringLiteral("短标题"),
              QStringLiteral("14) 显示：短标题原样"));

        // 30 个「长」字。★ 这里必须用 QChar(0x957F) 而不是 QLatin1Char('长') ——
        // QLatin1Char 只装得下单个 Latin-1 字节，'长' 会被截成半个 UTF-8 字节，
        // 于是这条"中文超长标题"的用例其实在测 30 个 'é'：断言照样绿，
        // 但它已经不再验证它声称要验证的东西了。
        item.title = QString(30, QChar(0x957F));
        const QString shown = MarkdownOutline::displayTextFor(item, 10);
        check(shown.size() == 10 && shown.endsWith(QStringLiteral("…")),
              QStringLiteral("14b) 显示：超长标题截断后补省略号，总宽不超 maxChars"),
              QStringLiteral("size=%1").arg(shown.size()));

        // 边界：maxChars 恰好等于标题长度 → 不截
        item.title = QStringLiteral("正好十个字符啊啊");
        check(MarkdownOutline::displayTextFor(item, 8) == item.title,
              QStringLiteral("14c) 显示：长度等于上限时不截"));

        check(MarkdownOutline::displayTextFor(item, 0).isEmpty(),
              QStringLiteral("14d) 显示：maxChars 为 0 时返回空串（不崩）"));
    }

    // ============================ I. 空输入 ============================
    {
        check(MarkdownOutline::extract(QString()).isEmpty(), QStringLiteral("15) 空文本 -> 0 项"));
        check(MarkdownOutline::extract(QStringLiteral("\n\n\n")).isEmpty(),
              QStringLiteral("15b) 只有空行 -> 0 项"));
        // 没有换行符的单行输入（split 之后只有一段）
        check(MarkdownOutline::extract(QStringLiteral("# 单行")).size() == 1,
              QStringLiteral("15c) 没有换行的单行文本也能识别"));
    }

    if (g_fail == 0) {
        std::printf("\n=== MarkdownOutline 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== MarkdownOutline 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
