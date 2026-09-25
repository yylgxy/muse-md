// TextStats（C5 写作统计）的纯函数契约测试。
//
// ★ 不链接 Qt6::Widgets、不需要 QApplication：统计口径是**规则**，不是界面，
//   所以它能在 core/document 里被毫秒级验证。这一点很重要 —— 口径类的东西最容易被
//   后来的人"顺手改一下"，而它一旦变了，用户看到的字数就变了，却没人会立刻发现。
//
// 这个测试逐项钉住 TextStats 头文件里写下的口径：
//   字符（含空白标点）/ 词（中文按连续汉字串，英文按空白标点切）/
//   段（空行分隔的非空块）/ 句（连续终止符算一句）/ 阅读时长（中英各算各的）

#include "textstats.h"

#include <QChar>
#include <QString>
#include <QStringList>

#include <cstdio>

using markdown_editor::core::document::TextStats;

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-58s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// 把五项压成一行便于比对
QString digest(const TextStats &s)
{
    return QStringLiteral("字%1 词%2 段%3 句%4 分%5")
        .arg(s.characters)
        .arg(s.words)
        .arg(s.paragraphs)
        .arg(s.sentences)
        .arg(s.readingMinutes);
}

}  // namespace

int main()
{
    // ============================ A. 字符 ============================
    {
        const TextStats s = TextStats::compute(QStringLiteral("你好 world"));
        // "你好 world" = 2 + 1 + 5 = 8 个字符（空格也算 —— 口径写明了）
        check(s.characters == 8, QStringLiteral("A1) 字符数含空白与标点"), digest(s));
        check(TextStats::compute(QString()).characters == 0, QStringLiteral("A2) 空串 -> 0 字符"));
    }

    // ============================ B. 词 ============================
    {
        // 纯英文：按空白切
        check(TextStats::compute(QStringLiteral("hello world")).words == 2,
              QStringLiteral("B1) 英文按空白切：2 个词"));
        check(TextStats::compute(QStringLiteral("  hello   world  ")).words == 2,
              QStringLiteral("B2) 多余空白不产生空词"));

        // 纯中文：连续汉字串算一个词（这是与"字数"最关键的区别）
        check(TextStats::compute(QStringLiteral("你好世界")).words == 1,
              QStringLiteral("B3) ★连续汉字串算 1 个词（不是 4 个）"));
        check(TextStats::compute(QStringLiteral("你好 世界")).words == 2,
              QStringLiteral("B4) 中文之间加空格 -> 2 个词"));

        // 中英混排：边界处各算一次
        check(TextStats::compute(QStringLiteral("用 Qt 写 Markdown")).words == 4,
              QStringLiteral("B5) ★中英混排：用/1 + Qt/1 + 写/1 + Markdown/1 = 4"),
              digest(TextStats::compute(QStringLiteral("用 Qt 写 Markdown"))));
        check(TextStats::compute(QStringLiteral("abc你好")).words == 2,
              QStringLiteral("B6) 英文紧接中文（没空格）：2 个词"));
        check(TextStats::compute(QStringLiteral("你好abc")).words == 2,
              QStringLiteral("B7) 中文紧接英文（没空格）：2 个词"));

        // 标点切英文词（口径里写明的近似）
        check(TextStats::compute(QStringLiteral("a-b c.d")).words == 4,
              QStringLiteral("B8) 标点也切英文词：a-b c.d = 4（口径里写明的近似）"),
              digest(TextStats::compute(QStringLiteral("a-b c.d"))));
        check(TextStats::compute(QStringLiteral("。。。")).words == 0,
              QStringLiteral("B9) 全是标点 -> 0 个词"));
    }

    // ============================ C. 段 ============================
    {
        check(TextStats::compute(QStringLiteral("第一段\n第二行\n\n第二段")).paragraphs == 2,
              QStringLiteral("C1) ★连续非空行算一段，空行分段"));
        check(TextStats::compute(QStringLiteral("一段\n\n\n\n另一段")).paragraphs == 2,
              QStringLiteral("C2) 连续多个空行不产生空段落"));
        check(TextStats::compute(QStringLiteral("只有一段，没有换行")).paragraphs == 1,
              QStringLiteral("C3) 单行 -> 1 段"));
        check(TextStats::compute(QStringLiteral("\n\n\n")).paragraphs == 0,
              QStringLiteral("C4) 全是空行 -> 0 段"));
        check(TextStats::compute(QStringLiteral("   \n\t\n")).paragraphs == 0,
              QStringLiteral("C5) 只有空白的行不算段落"));
        check(TextStats::compute(QStringLiteral("一段\n\n")).paragraphs == 1,
              QStringLiteral("C6) 结尾多余换行不影响段数"));
        // 与焦点模式的口径一致：标点也行"算内容"（"——"单独一行是一段）
        check(TextStats::compute(QStringLiteral("——")).paragraphs == 1,
              QStringLiteral("C7) 只由标点组成的一行仍算一段（和焦点模式同口径）"));
    }

    // ============================ D. 句 ============================
    {
        check(TextStats::compute(QStringLiteral("第一句。第二句！第三句？")).sentences == 3,
              QStringLiteral("D1) 中英句末标点都认：3 句"));
        check(TextStats::compute(QStringLiteral("真的吗！！！")).sentences == 1,
              QStringLiteral("D2) ★连续终止符算一句（「！！！」不是三句）"));
        check(TextStats::compute(QStringLiteral("One. Two! Three?")).sentences == 3,
              QStringLiteral("D3) 英文句末标点：3 句"));
        check(TextStats::compute(QStringLiteral("没有句末标点的一段话")).sentences == 0,
              QStringLiteral("D4) 没有句末标点 -> 0 句"));
        check(TextStats::compute(QStringLiteral("先一句。 又一句。")).sentences == 2,
              QStringLiteral("D5) 句末标点后的空格不会多算一句"));
    }

    // ============================ E. 阅读时长 ============================
    {
        // 300 个汉字 = 1 分钟
        const QString cjk300 = QString(300, QChar(0x4E2D));  // "中" × 300
        check(TextStats::compute(cjk300).readingMinutes == 1,
              QStringLiteral("E1) 300 个汉字 -> 1 分钟"), digest(TextStats::compute(cjk300)));

        // 200 个英文词 = 1 分钟
        QStringList words;
        for (int i = 0; i < 200; ++i) {
            words << QStringLiteral("word");
        }
        check(TextStats::compute(words.join(QLatin1Char(' '))).readingMinutes == 1,
              QStringLiteral("E2) 200 个英文词 -> 1 分钟"),
              digest(TextStats::compute(words.join(QLatin1Char(' ')))));

        // 有内容时至少 1 分钟（显示"0 分钟"会让人以为算坏了）
        check(TextStats::compute(QStringLiteral("短")).readingMinutes == 1,
              QStringLiteral("E3) 有内容时至少 1 分钟"));
        check(TextStats::compute(QString()).readingMinutes == 0,
              QStringLiteral("E4) 空文档 -> 0 分钟（这个 0 是对的）"));
    }

    // ============================ F. 输出格式 ============================
    {
        TextStats s;
        s.characters = 1234;
        s.words = 56;
        s.paragraphs = 12;
        s.sentences = 13;
        s.readingMinutes = 3;
        check(TextStats::format(s) == QStringLiteral("1234 字符 · 56 词 · 12 段 · 3 分钟"),
              QStringLiteral("F1) 摘要一行"), TextStats::format(s));

        // 详情里必须带口径说明：数字旁边没有口径就等于没说清
        const QString detail = TextStats::detail(s);
        check(detail.contains(QStringLiteral("1234 字符")) && detail.contains(QStringLiteral("连续汉字串"))
                  && detail.contains(QStringLiteral("300 字/分钟")),
              QStringLiteral("F2) 详情里带上了每项的口径说明"));
    }

    // ============================ G. 规模（不慢到离谱就行）============================
    {
        // 30 万字符：compute 是按需调用的，所以它只要"不慢到离谱"就行（毫秒级）
        //
        // ★ 这里刻意**两种文本都建**：段的口径是"空行分隔"，所以 3 万行**连续非空行**
        //   是 1 段、3 万个**空行分隔块**才是 3 万段。只测一种就会把口径写反 ——
        //   我第一版就是这么把"每行一段"当成了正确行为（C1 立刻抓住了，见 G3）。
        QString blockSeparated;   // 空行分隔：3 万个独立段落
        QString lineSeparated;    // 只有换行：3 万行属于同一段
        blockSeparated.reserve(340000);
        lineSeparated.reserve(310000);
        for (int i = 0; i < 30000; ++i) {
            blockSeparated += QStringLiteral("这一行是中文内容。\n\n");
            lineSeparated += QStringLiteral("这一行是中文内容。\n");
        }

        const TextStats s = TextStats::compute(blockSeparated);
        check(s.paragraphs == 30000, QStringLiteral("G1) 3 万个空行分隔块 = 3 万段"),
              QStringLiteral("段=%1").arg(s.paragraphs));
        check(s.sentences == 30000, QStringLiteral("G2) 3 万句的长文句数正确"),
              QStringLiteral("句=%1").arg(s.sentences));

        // 同一批内容、只把空行去掉：段数必须塌成 1。这一条是 G1 的反面，
        // 两条一起才能证明"段"不是按行数的。
        const TextStats s2 = TextStats::compute(lineSeparated);
        check(s2.paragraphs == 1, QStringLiteral("G3) ★3 万行连续非空行 = 1 段（不是 3 万段）"),
              QStringLiteral("段=%1").arg(s2.paragraphs));
        check(s2.sentences == 30000, QStringLiteral("G4) 句数不受空行影响"),
              QStringLiteral("句=%1").arg(s2.sentences));
    }

    if (g_fail == 0) {
        std::printf("\n=== TextStats 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== TextStats 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
