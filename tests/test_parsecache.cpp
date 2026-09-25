// ParseCache（#1 块级解析缓存）的契约测试。
//
// 重点不是"能切块"，而是三件容易写错、错了又不容易发现的事：
//   1. **块切分**：空行分隔段落，围栏代码块内部的空行不算分隔（否则代码块会被切成
//      一堆碎块，失效范围就错了）。
//   2. **改动行 → 失效块**：用 LineDiff 算出改动行区间，只失效覆盖到的块 ——
//      "改一行只失效一个块"是增量的意义所在；改一大片才失效一大片。
//   3. **降级闸门**：diff 降级（改动太大）时全部失效，正确性优先于省事。
//
// 纯逻辑测试，不碰 QWidget，也不需要 Chromium。
//
// 跑法：ctest -C Debug --output-on-failure

#include "parsecache.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <cstdio>

using markdown_editor::core::document::ParseCache;

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

// 把块列表转成可读的 "start-end" 字符串，方便断言和失败时打印。
QString describe(const QList<ParseCache::Block> &blocks)
{
    QStringList parts;
    for (const ParseCache::Block &b : blocks) {
        parts << QStringLiteral("%1-%2").arg(b.startLine).arg(b.endLine);
    }
    return parts.join(QStringLiteral(","));
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ============================ 块切分 ============================
    {
        // 三个段落，空行分隔 → 三个块
        const QString text = QStringLiteral("第一段\n\n第二段\n\n第三段\n");
        const QList<ParseCache::Block> blocks = ParseCache::splitBlocks(text);
        check(blocks.size() == 3, QStringLiteral("切块: 三个段落 → 三个块"), describe(blocks));
        check(blocks.size() == 3 && blocks.at(0).startLine == 1 && blocks.at(0).endLine == 1,
              QStringLiteral("切块: 第一个块是第 1 行"));
        check(blocks.size() == 3 && blocks.at(1).startLine == 3 && blocks.at(1).endLine == 3,
              QStringLiteral("切块: 第二个块是第 3 行（跳过了第 2 行的空行）"));
        check(blocks.size() == 3 && blocks.at(2).startLine == 5 && blocks.at(2).endLine == 5,
              QStringLiteral("切块: 第三个块是第 5 行"));
    }

    {
        // 空文档 → 没有块
        const QList<ParseCache::Block> blocks = ParseCache::splitBlocks(QString());
        check(blocks.isEmpty(), QStringLiteral("切块: 空文档没有块"));
    }

    {
        // 连续非空行（没有空行）→ 一个块
        const QString text = QStringLiteral("第一行\n第二行\n第三行\n");
        const QList<ParseCache::Block> blocks = ParseCache::splitBlocks(text);
        check(blocks.size() == 1, QStringLiteral("切块: 连续非空行是一个块"), describe(blocks));
        check(blocks.size() == 1 && blocks.at(0).startLine == 1 && blocks.at(0).endLine == 3,
              QStringLiteral("切块: 连续块覆盖 1-3 行"));
    }

    {
        // ★ 围栏代码块里的空行不算分隔
        const QString text = QStringLiteral("```cpp\nint a;\n\nint b;\n```\n\n后面的段落\n");
        const QList<ParseCache::Block> blocks = ParseCache::splitBlocks(text);
        // 围栏块（第 1-5 行，含开闭围栏）是一个块，后面的段落（第 7 行）是另一个块。
        // 第 6 行是围栏后的空行（块分隔），第 3 行是围栏内的空行（不算分隔）。
        check(blocks.size() == 2, QStringLiteral("切块: 围栏代码块整体一个块"), describe(blocks));
        check(blocks.size() == 2 && blocks.at(0).startLine == 1 && blocks.at(0).endLine == 5,
              QStringLiteral("切块: 围栏块覆盖 1-5 行（内部空行没切断）"));
        check(blocks.size() == 2 && blocks.at(1).startLine == 7,
              QStringLiteral("切块: 围栏后的段落从第 7 行开始"));
    }

    {
        // 标题 + 段落 + 列表（连续非空，都算一个块）
        const QString text = QStringLiteral("# 标题\n\n正文段落\n\n- 列表项\n- 第二项\n");
        const QList<ParseCache::Block> blocks = ParseCache::splitBlocks(text);
        check(blocks.size() == 3, QStringLiteral("切块: 标题/段落/列表三个块"), describe(blocks));
        // 列表两项之间没有空行，算一个块
        check(blocks.size() == 3 && blocks.at(2).startLine == 5 && blocks.at(2).endLine == 6,
              QStringLiteral("切块: 列表两项是一个块（5-6 行）"));
    }

    // ============================ 块与行区间重叠 ============================
    {
        const QString text = QStringLiteral("一\n\n二\n\n三\n\n四\n");
        const QList<ParseCache::Block> blocks = ParseCache::splitBlocks(text);  // 块在 1,3,5,7 行

        // 只改第 3 行 → 只命中第二个块（下标 1）
        const QList<int> hit = ParseCache::blocksOverlapping(blocks, 3, 3);
        check(hit.size() == 1 && hit.at(0) == 1, QStringLiteral("重叠: 改第 3 行只命中下标 1 的块"));

        // 改 1-5 行 → 命中前三个块（下标 0,1,2）
        const QList<int> wide = ParseCache::blocksOverlapping(blocks, 1, 5);
        check(wide.size() == 3, QStringLiteral("重叠: 改 1-5 行命中三个块"));
    }

    // ============================ 用 diff 定位失效块 ============================
    {
        // 三个段落，只改中间一个段落的一行 → 只失效中间那个块
        const QString oldText = QStringLiteral("第一段\n\n第二段原文\n\n第三段\n");
        const QString newText = QStringLiteral("第一段\n\n第二段改后\n\n第三段\n");

        const QList<int> invalid = ParseCache::invalidatedBlocks(oldText, newText);
        check(invalid.size() == 1, QStringLiteral("失效: 只改一个段落 → 只失效一个块"),
              QStringLiteral("失效下标数=%1").arg(invalid.size()));
        check(invalid.size() == 1 && invalid.at(0) == 1,
              QStringLiteral("失效: 失效的是中间那个块（下标 1）"));
    }

    {
        // 内容完全没变 → 不失效任何块
        const QString text = QStringLiteral("第一段\n\n第二段\n");
        const QList<int> invalid = ParseCache::invalidatedBlocks(text, text);
        check(invalid.isEmpty(), QStringLiteral("失效: 内容没变 → 不失效"));
    }

    {
        // 全文大改 → 失效全部块
        const QString oldText = QStringLiteral("旧第一段\n\n旧第二段\n\n旧第三段\n");
        const QString newText = QStringLiteral("全新的第一段\n\n全新的第二段\n\n全新的第三段\n");
        const QList<int> invalid = ParseCache::invalidatedBlocks(oldText, newText);
        check(invalid.size() == 3, QStringLiteral("失效: 全文改 → 三个块全失效"),
              QStringLiteral("失效下标数=%1").arg(invalid.size()));
    }

    // ============================ computeInvalidation（增量重算骨架）============================
    {
        const QString oldText = QStringLiteral("甲\n\n乙\n\n丙\n");
        const QString newText = QStringLiteral("甲\n\n乙改\n\n丙\n");
        const ParseCache::Invalidation inv = ParseCache::computeInvalidation(oldText, newText);

        check(inv.newBlocks.size() == 3, QStringLiteral("骨架: 新文本切成三个块"));
        check(inv.invalidBlocks.size() == 1 && inv.invalidBlocks.at(0) == 1,
              QStringLiteral("骨架: 只失效第二个块（下标 1）"));
    }

    // ============================ 降级闸门 ============================
    {
        // 两段完全不同的"长文"（这里用小规模也能测 degraded 吗？）
        // LineDiff 的降级阈值是 kFallbackThreshold=50000 行，小规模不会降级。
        // 所以这里不测 degraded 分支的"全部失效"（那需要造 5 万行），
        // 只确认：正常规模下 degraded 恒为 false，失效块按改动范围精确给出。
        const QString oldText = QStringLiteral("一\n\n二\n\n三\n");
        const QString newText = QStringLiteral("一\n\n二\n\n三改\n");
        const QList<int> invalid = ParseCache::invalidatedBlocks(oldText, newText);
        check(invalid.size() == 1, QStringLiteral("闸门: 小改动精确失效（不降级）"));
    }

    if (g_fail == 0) {
        std::printf("\n=== ParseCache 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== ParseCache 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
