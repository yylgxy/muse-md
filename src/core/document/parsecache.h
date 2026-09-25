#ifndef PARSECACHE_H
#define PARSECACHE_H

#include <QList>
#include <QString>
#include <QStringList>

namespace markdown_editor::core::document {

// 块级解析缓存（#1）：把"整篇重算"变成"按块失效"。★ 零 Widgets 依赖，能单独测。
//
// ---- 要解决的问题 ----
// 打字时大纲、字数、高亮这些"从 Markdown 源码派生"的结果，最省事的做法是"改一处、整篇重算"。
// 1MB 的文档上，一次重算要遍历几十万字符 —— 打字会明显发黏。但用户每次按键通常只改
// 少数几行，只影响附近的几个"块"，其余块的结果完全可以复用上次的。
//
// ---- 什么是"块" ----
// Markdown 的顶层语义块：段落、标题、列表、代码块、引用…… 它们共同的特征是
// "被空行分隔"（代码块还有围栏）。所以本类用"空行 + 围栏"把文档切成若干块，
// 每个块记它的 [起始行, 结束行]（1 起算，含两端）。这个切分是**纯源码行扫描**，
// 不依赖 md4c（和 MarkdownOutline / SyncBridge 用的是同一个约束：md4c 回调不给行号）。
//
// ---- 失效策略（面试必问）----
// 不猜"哪些块要重算"，而是用 LineDiff 算出**改动行区间**，再把这些行所属的块全部失效。
// 这样"改一行"只失效一个块，"改一屏"才失效一大片 —— 失效范围精确跟随改动范围。
// 极端情况（改动跨越全文、或 diff 降级）自动回退成"全部失效"，保证正确性优先于省事。
//
// ---- 已知边界（写清楚免得当 bug）----
//   * 只按空行 + 围栏切块，不解析标题层级 / 列表嵌套：那是"内容"层面的东西，
//     本类只关心"哪几行算一个不可分割的重算单元"。
//   * 围栏判据复用 MarkdownOutline::isFenceLine —— 全局唯一一份，不会和高亮器/大纲漂移。
//   * 块边界是**近似**：md4c 真实解析里"4 空格缩进代码块""Setext 标题"这类边界
//     会和空行切分有细微出入，但对"哪些行该一起重算"这个目的来说足够准。
class ParseCache
{
public:
    // 一个块：[startLine, endLine]，都是 1 起算、含两端。
    struct Block
    {
        int startLine = 1;
        int endLine = 1;  // >= startLine
    };

    // 把整篇文档切成块（纯函数，能单独测）。
    // 规则：空行是块之间的分隔；围栏代码块（``` / ~~~）整体算一个块，内部的空行不算分隔。
    static QList<Block> splitBlocks(const QString &text);

    // 哪些块覆盖了 [startLine, endLine] 这个行区间（含边界重叠）。
    // 返回这些块在 splitBlocks 结果里的下标。用于"改动行 → 失效块"的映射。
    static QList<int> blocksOverlapping(const QList<Block> &blocks, int startLine, int endLine);

    // 计算"从 oldText 变成 newText"后，哪些块需要失效（返回它们的下标）。
    // 内部走 LineDiff::compute：改动行区间 → 覆盖到的块 → 失效。
    // degraded 或改动跨全文时返回"全部失效"（下标 0..块数-1），正确性优先。
    static QList<int> invalidatedBlocks(const QString &oldText, const QString &newText);

    // ---- 供上层（大纲/字数）使用的"增量重算"骨架 ----
    // 给定旧文、新文、旧的块列表，返回 (失效块下标, 新块列表)。
    // 上层拿到失效下标后，只重算这些块，其余块复用缓存。
    struct Invalidation
    {
        QList<int> invalidBlocks;   // 需要重算的块下标（针对 newBlocks 的坐标系）
        QList<Block> newBlocks;     // 新文本切出的块（供上层更新自己的缓存）
    };
    static Invalidation computeInvalidation(const QString &oldText, const QString &newText);
};

}  // namespace markdown_editor::core::document

#endif // PARSECACHE_H
