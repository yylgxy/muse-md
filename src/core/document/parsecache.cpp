#include "parsecache.h"

#include "linediff.h"
#include "markdownoutline.h"  // 围栏判据全局唯一一份

namespace markdown_editor::core::document {

// ============================ 块切分 ============================

QList<ParseCache::Block> ParseCache::splitBlocks(const QString &text)
{
    QList<Block> blocks;

    // 先按 \n 切行（复用 LineDiff::splitLines：剥 \r、忽略末尾空行），
    // 行号用下标 + 1（1 起算）。这样 splitBlocks 和 LineDiff 用同一份行数组，
    // "改动行"和"块边界"的坐标系天然对齐。
    const QStringList lines = LineDiff::splitLines(text);
    if (lines.isEmpty()) {
        return blocks;  // 空文档：没有块
    }

    int blockStart = 1;     // 当前块的起始行（1 起算）
    bool inFence = false;   // 是否在围栏代码块里（围栏内的空行不算块分隔）

    const int n = lines.size();
    for (int i = 0; i < n; ++i) {
        const QString &line = lines.at(i);
        const int lineNo = i + 1;

        if (MarkdownOutline::isFenceLine(line)) {
            // 围栏行：切换状态。进入围栏时不切断当前块（围栏属于它所在的块），
            // 退出围栏后继续归入当前块，直到下一个空行。
            inFence = !inFence;
            continue;
        }

        const bool isBlank = line.trimmed().isEmpty();

        if (isBlank && !inFence) {
            // 空行且不在围栏里：这是块分隔。把 [blockStart, i] 收成一个块（如果非空）。
            if (i > 0 && blockStart <= i) {
                Block block;
                block.startLine = blockStart;
                block.endLine = i;  // 空行本身（第 i+1 行）不算进块
                blocks << block;
            }
            blockStart = lineNo + 1;  // 下一个块从空行之后开始
            continue;
        }
        // 非空行（或围栏内的空行）：继续当前块，不做事
    }

    // 收尾：最后一段（没有以空行结尾的）
    if (blockStart <= n) {
        Block block;
        block.startLine = blockStart;
        block.endLine = n;
        blocks << block;
    }

    return blocks;
}

// ============================ 块与行区间的重叠 ============================

QList<int> ParseCache::blocksOverlapping(const QList<Block> &blocks, int startLine, int endLine)
{
    QList<int> result;
    for (int i = 0; i < blocks.size(); ++i) {
        const Block &b = blocks.at(i);
        // 区间重叠：[b.start, b.end] 与 [startLine, endLine] 有交集
        if (b.endLine >= startLine && b.startLine <= endLine) {
            result << i;
        }
    }
    return result;
}

// ============================ 用 diff 定位失效块 ============================

QList<int> ParseCache::invalidatedBlocks(const QString &oldText, const QString &newText)
{
    const QList<Block> oldBlocks = splitBlocks(oldText);
    if (oldBlocks.isEmpty()) {
        return {};  // 旧文没有块，没有可失效的
    }

    const QStringList oldLines = LineDiff::splitLines(oldText);
    const QStringList newLines = LineDiff::splitLines(newText);
    const LineDiff::Result diff = LineDiff::compute(oldLines, newLines);

    // 降级（改动太大、diff 放弃逐行对齐）→ 全部失效，正确性优先。
    if (diff.degraded) {
        QList<int> all;
        for (int i = 0; i < oldBlocks.size(); ++i) {
            all << i;
        }
        return all;
    }

    // 收集所有"改动"涉及的行号（1 起算）。
    // Delete 块：oldStart..oldStart+oldCount-1；Insert 块：newStart..newStart+newCount-1。
    // 用 QSet 去重（同一行可能被删又被插）。
    int minChanged = -1;
    int maxChanged = -1;
    for (const LineDiff::Hunk &h : diff.hunks) {
        if (h.kind == LineDiff::Kind::Delete) {
            for (int i = 0; i < h.oldCount; ++i) {
                const int line = h.oldStart + i;
                if (minChanged < 0 || line < minChanged) minChanged = line;
                if (line > maxChanged) maxChanged = line;
            }
        } else if (h.kind == LineDiff::Kind::Insert) {
            for (int i = 0; i < h.newCount; ++i) {
                const int line = h.newStart + i;
                if (minChanged < 0 || line < minChanged) minChanged = line;
                if (line > maxChanged) maxChanged = line;
            }
        }
    }

    // 没有改动（identical）→ 不失效任何块。
    if (minChanged < 0) {
        return {};
    }

    // 改动行区间 → 覆盖到的旧块。
    return blocksOverlapping(oldBlocks, minChanged, maxChanged);
}

ParseCache::Invalidation ParseCache::computeInvalidation(const QString &oldText, const QString &newText)
{
    Invalidation result;
    result.newBlocks = splitBlocks(newText);
    result.invalidBlocks = invalidatedBlocks(oldText, newText);
    return result;
}

}  // namespace markdown_editor::core::document
