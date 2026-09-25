#include "linediff.h"

#include <QHash>
#include <QPair>
#include <QtGlobal>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace markdown_editor::core::document {

namespace {

// 一条"逐行"的操作：Equal 两个索引都有效；Delete 只有 oldIndex；Insert 只有 newIndex。
// 索引都是 **0 起算**（内部用），生成 Hunk 时才 +1 换成 1 起算。
struct LineOp
{
    LineDiff::Kind kind = LineDiff::Kind::Equal;
    int oldIndex = -1;
    int newIndex = -1;
};

// 一次"匹配"：旧文本的第 oldIndex 行 == 新文本的第 newIndex 行（0 起算）。
// 匹配序列必须严格递增（两个坐标都递增），这是下面所有算法共同的产出契约。
using Match = QPair<int, int>;

// ---------------------------------------------------------------------------
// 阶段 1｜行哈希预去重（性价比最高的一步，且**不改变结果**）
// ---------------------------------------------------------------------------
//
// LCS / Myers 的内层循环是 O(N·M) 次"两个元素是否相等"的比较。
// 比较 QString（可能上百字符，逐字符 memcmp + 长度检查）和比较 int 差一个数量级。
// 所以先建一张 `行内容 → 编号` 的表：同一行文本永远拿到同一个编号，
// 之后整个算法只做整数比较。**这不改变任何结果**，纯粹是常数优化
// —— 因为它是"内容 → 编号"的双射（编号只在同一份输入里唯一），
// 相等的行编号必相等，不相等的行编号必不等。
class LineIds
{
public:
    LineIds(int expectedLines, int maxCharsPerLine)
        : m_maxChars(maxCharsPerLine)
    {
        m_table.reserve(expectedLines);
    }

    // 把整个行列表编码成编号序列。
    QList<int> encode(const QStringList &lines)
    {
        QList<int> ids;
        ids.reserve(lines.size());
        for (const QString &line : lines) {
            ids.append(idFor(line));
        }
        return ids;
    }

private:
    // 超长行只在哈希阶段截断：目的是"别为一行哈希几十万字符"。
    // 代价见头文件降级表最后一行（降级方向是"少报差异"，不会崩）。
    int idFor(const QString &line)
    {
        const QString key = (line.size() > m_maxChars) ? line.left(m_maxChars) : line;

        const auto it = m_table.constFind(key);
        if (it != m_table.constEnd()) {
            return it.value();
        }
        const int id = m_table.size();  // 0,1,2,… 与插入顺序一一对应
        m_table.insert(key, id);
        return id;
    }

    QHash<QString, int> m_table;
    int m_maxChars;
};

// ---------------------------------------------------------------------------
// 阶段 2｜动态规划 LCS（默认路径）
// ---------------------------------------------------------------------------
//
// 为什么这一版要先用 DP：它的正确性**一眼可见**，而且结果可以直接和
// "暴力法"对拍（见 tests/test_linediff.cpp 的最小性用例）。
// 用后缀式定义，回溯时能从前往后走，不必递归：
//
//     dp[i][j] = a[i..) 与 b[j..) 的最长公共子序列长度
//     dp[n][j] = 0, dp[i][m] = 0
//     dp[i][j] = (a[i] == b[j]) ? dp[i+1][j+1] + 1
//                               : max(dp[i+1][j], dp[i][j+1])
//
// 内存：这里**没有**用两行滚动数组。原因很直接 —— 滚动数组能算出"LCS 有多长"，
// 但回溯编辑脚本需要整张表。想同时拿到"O(min(N,M)) 内存 + 可回溯"要上
// Hirschberg 的分治，那是另一个量级的工作量。而这条路径的入口条件已经把
// 规模钉死在 kDpLimit（总行数 2000 → 表最多 1000×1000），
// 表用 uint16 存（LCS 长度 <= 1000）只要 2MB，不值得为它引入分治。
// 大文件走阶段 3 的 Myers，那条路径的内存是 O(N + D² 的路径记录)。
QList<Match> lcsMatchesDp(const QList<int> &a, const QList<int> &b)
{
    const int n = a.size();
    const int m = b.size();

    // 行数差太大时连表都不用建（调用方通常已经拦掉了，这里是兜底）
    const int width = m + 1;
    std::vector<quint16> dp(static_cast<size_t>(n + 1) * static_cast<size_t>(width), 0);

    auto at = [&dp, width](int i, int j) -> quint16 & {
        return dp[static_cast<size_t>(i) * static_cast<size_t>(width) + static_cast<size_t>(j)];
    };

    for (int i = n - 1; i >= 0; --i) {
        for (int j = m - 1; j >= 0; --j) {
            if (a.at(i) == b.at(j)) {
                at(i, j) = static_cast<quint16>(at(i + 1, j + 1) + 1);
            } else {
                at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
            }
        }
    }

    // 从前往后回溯，产出的匹配序列天然递增。
    QList<Match> matches;
    int i = 0;
    int j = 0;
    while (i < n && j < m) {
        if (a.at(i) == b.at(j)) {
            matches.append(Match(i, j));
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            ++i;  // 这一行 a[i] 归入 Delete
        } else {
            ++j;  // 这一行 b[j] 归入 Insert
        }
    }
    return matches;
}

// ---------------------------------------------------------------------------
// 阶段 3｜Myers O(ND)（大文件）
// ---------------------------------------------------------------------------
//
// Myers 的核心是"沿对角线扩张"：d = 编辑距离，第 d 轮只关心"用了 d 步之后，
// 每条对角线 k = x - y 上最远能到哪个 x"。
//
//     V[k] = 该对角线上当前能到达的最远 x
//     每轮：k 从 -d 到 d 隔 2 取（因为一步只会让 k 变化 ±1）
//           x 取 max(V[k+1], V[k-1] + 1)  —— 前者是"从上面下来"（Insert），
//                                            后者是"从左边过来"（Delete）
//           然后沿对角线一路吃掉相等行（"蛇"，snake）
//           x >= N && y >= M 就说明找到了最短编辑脚本
//
// 两个必须在注释里说清的取舍（面试会问）：
//   * **复杂度 O((N+M)·D)**，D 是编辑距离。两份文件"几乎相同"时 D 很小 → 飞快；
//     "两份文件完全不同"时 D ≈ N+M → 退化成平方级。这正是 kMaxEditDistance
//     这个闸门存在的理由：过了阈值就降级成整段替换，宁可少报差异也不能卡死。
//   * **回溯需要每轮的 V**，所以空间是 O(D²)（每轮只存 [-d, d] 那一段，
//     Σ(2d+1) ≈ D²）。省内存的正规做法是"分治 + 中线"（linear space refinement），
//     先把能跑通的版本落地、阈值兜住最坏情况，是这里的取舍。
QList<Match> myersMatches(const QList<int> &a,
                          const QList<int> &b,
                          int maxEditDistance,
                          bool *budgetExceeded)
{
    *budgetExceeded = false;

    const int n = a.size();
    const int m = b.size();
    const int maxD = n + m;  // 编辑距离的理论上界（全删 + 全插）

    // V 用 offset 映射负数下标：V[k + offset]。
    std::vector<int> v(static_cast<size_t>(2 * maxD + 1), 0);
    const int offset = maxD;

    // 每轮只快照 [-d, d] 这一段（内存 O(D²) 而不是 O(D·N)）。
    // trace[d][k + d] = 第 d 轮对角线 k 上的 V 值。
    std::vector<std::vector<int>> trace;
    trace.reserve(static_cast<size_t>(std::min(maxEditDistance, 64)) + 1);

    int foundD = -1;

    for (int d = 0; d <= maxD; ++d) {
        // ★ 闸门：编辑距离超过预算就放弃对齐，交给调用方降级。
        if (d > maxEditDistance) {
            *budgetExceeded = true;
            return QList<Match>();
        }

        std::vector<int> row(static_cast<size_t>(2 * d + 1), 0);

        for (int k = -d; k <= d; k += 2) {
            int x = 0;
            if (k == -d || (k != d && v[static_cast<size_t>(k - 1 + offset)]
                                          < v[static_cast<size_t>(k + 1 + offset)])) {
                x = v[static_cast<size_t>(k + 1 + offset)];  // 从上面下来 = Insert
            } else {
                x = v[static_cast<size_t>(k - 1 + offset)] + 1;  // 从左边过来 = Delete
            }

            int y = x - k;
            while (x < n && y < m && a.at(x) == b.at(y)) {  // 沿对角线扩张（吃相等行）
                ++x;
                ++y;
            }

            v[static_cast<size_t>(k + offset)] = x;
            row[static_cast<size_t>(k + d)] = x;

            if (x >= n && y >= m) {
                foundD = d;
                break;
            }
        }

        trace.push_back(std::move(row));

        if (foundD >= 0) {
            break;
        }
    }

    if (foundD < 0) {
        // 理论上到不了这里（d = maxD 时必然可达），保守起见按预算超限处理
        *budgetExceeded = true;
        return QList<Match>();
    }

    // ---- 回溯：从 (n, m) 倒着走回 (0, 0)，路上走的每一步"对角线"都是匹配 ----
    QList<Match> reversed;
    int x = n;
    int y = m;

    for (int d = foundD; d > 0; --d) {
        const std::vector<int> &prev = trace[static_cast<size_t>(d - 1)];
        const int k = x - y;

        int prevK = 0;
        if (k == -d || (k != d && prev[static_cast<size_t>(k - 1 + (d - 1))]
                                       < prev[static_cast<size_t>(k + 1 + (d - 1))])) {
            prevK = k + 1;
        } else {
            prevK = k - 1;
        }

        const int prevX = prev[static_cast<size_t>(prevK + (d - 1))];
        const int prevY = prevX - prevK;

        // 先把这一段"蛇"记下来（不含起点，起点属于上一轮的终点）
        while (x > prevX && y > prevY) {
            --x;
            --y;
            reversed.append(Match(x, y));
        }

        // 再退到"施加那一步编辑之前"的位置
        x = prevX;
        y = prevY;
    }

    // d == 0 的那条对角线：从 (0,0) 起的一段公共前缀
    while (x > 0 && y > 0) {
        --x;
        --y;
        reversed.append(Match(x, y));
    }

    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

// ---------------------------------------------------------------------------
// 匹配序列 → 逐行操作 → 合并成 Hunk
// ---------------------------------------------------------------------------

QList<LineOp> buildOps(const QList<Match> &matches, int oldCount, int newCount)
{
    QList<LineOp> ops;
    ops.reserve(oldCount + newCount);

    int oi = 0;
    int ni = 0;
    for (const Match &match : matches) {
        for (int t = oi; t < match.first; ++t) {
            ops.append(LineOp{LineDiff::Kind::Delete, t, -1});
        }
        for (int t = ni; t < match.second; ++t) {
            ops.append(LineOp{LineDiff::Kind::Insert, -1, t});
        }
        ops.append(LineOp{LineDiff::Kind::Equal, match.first, match.second});
        oi = match.first + 1;
        ni = match.second + 1;
    }
    for (int t = oi; t < oldCount; ++t) {
        ops.append(LineOp{LineDiff::Kind::Delete, t, -1});
    }
    for (int t = ni; t < newCount; ++t) {
        ops.append(LineOp{LineDiff::Kind::Insert, -1, t});
    }
    return ops;
}

// 把逐行操作压成块。行号在**这里**从 0 起算换成 1 起算（全项目唯一的换算点）。
//
// 顺序约定：同一个"缺口"里 Delete 排在 Insert 前面（标准 diff 的写法）。
// 这也让 Delete 块的 newStart 等于紧随其后的 Insert 块的 newStart —— 两个都指"插入点"。
void buildHunks(const QList<LineOp> &ops, LineDiff::Result *result)
{
    int consumedOld = 0;
    int consumedNew = 0;

    for (const LineOp &op : ops) {
        LineDiff::Hunk *last = result->hunks.isEmpty() ? nullptr : &result->hunks.last();

        switch (op.kind) {
        case LineDiff::Kind::Equal:
            if (last != nullptr && last->kind == LineDiff::Kind::Equal) {
                ++last->oldCount;
                ++last->newCount;
            } else {
                result->hunks.append(
                    LineDiff::Hunk{LineDiff::Kind::Equal, consumedOld + 1, consumedNew + 1, 1, 1});
            }
            ++consumedOld;
            ++consumedNew;
            break;

        case LineDiff::Kind::Delete:
            if (last != nullptr && last->kind == LineDiff::Kind::Delete) {
                ++last->oldCount;
            } else {
                result->hunks.append(
                    LineDiff::Hunk{LineDiff::Kind::Delete, consumedOld + 1, consumedNew + 1, 1, 0});
            }
            ++consumedOld;
            ++result->deletedLines;
            break;

        case LineDiff::Kind::Insert:
            if (last != nullptr && last->kind == LineDiff::Kind::Insert) {
                ++last->newCount;
            } else {
                // oldStart 记的是"插入点"：已经消费掉的旧行数 + 1
                result->hunks.append(
                    LineDiff::Hunk{LineDiff::Kind::Insert, consumedOld + 1, consumedNew + 1, 0, 1});
            }
            ++consumedNew;
            ++result->insertedLines;
            break;
        }
    }
}

// 降级：不尝试对齐，整段替换。保证"最坏情况有上界"。
LineDiff::Result degradedResult(int oldCount, int newCount)
{
    LineDiff::Result result;
    result.degraded = true;
    if (oldCount > 0) {
        result.hunks.append(LineDiff::Hunk{LineDiff::Kind::Delete, 1, 1, oldCount, 0});
        result.deletedLines = oldCount;
    }
    if (newCount > 0) {
        result.hunks.append(LineDiff::Hunk{LineDiff::Kind::Insert,
                                           oldCount + 1,
                                           1,
                                           0,
                                           newCount});
        result.insertedLines = newCount;
    }
    return result;
}

QString formatRange(int start, int count)
{
    // git 的写法：数量为 0 时只写起始行号（且此时 start 是"前一行"的序号）
    if (count == 1) {
        return QString::number(start);
    }
    return QStringLiteral("%1,%2").arg(start).arg(count);
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

QStringList LineDiff::splitLines(const QString &text)
{
    QStringList lines = text.split(QLatin1Char('\n'));

    // 以换行结尾的文本，split 会多出一个末尾空元素 —— 去掉它，
    // 这样 "a\nb" 与 "a\nb\n" 切出同样的两行（见头文件的取舍说明）。
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }

    // 剥掉行尾的 \r（CRLF 输入）。
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
    }
    return lines;
}

LineDiff::Result LineDiff::compute(const QStringList &oldLines, const QStringList &newLines)
{
    // 恒等是最常见的情况（历史里点相邻两个版本，很多时候内容相同），
    // QStringList 的 == 会先比 size 再逐个比，比走算法快得多。
    if (oldLines == newLines) {
        Result result;
        if (!oldLines.isEmpty()) {
            result.hunks.append(Hunk{Kind::Equal, 1, 1, static_cast<int>(oldLines.size()),
                                     static_cast<int>(newLines.size())});
        }
        return result;
    }

    const int n = oldLines.size();
    const int m = newLines.size();
    const int longest = std::max(n, m);

    // ---- 降级闸门 1：规模 / 行数差（两个阈值都是对文档规模说的，理由见文件头）----
    if (longest > kFallbackThreshold || std::abs(n - m) > kLineCountDeltaThreshold) {
        return degradedResult(n, m);
    }

    // ---- 阶段 1：行哈希预去重 ----
    LineIds ids(n + m, kMaxHashChars);
    const QList<int> a = ids.encode(oldLines);
    const QList<int> b = ids.encode(newLines);

    // ---- 阶段 2 / 3：按规模选算法 ----
    QList<Match> matches;
    if (longest <= kDpLimit) {
        matches = lcsMatchesDp(a, b);
    } else {
        bool budgetExceeded = false;
        matches = myersMatches(a, b, kMaxEditDistance, &budgetExceeded);
        if (budgetExceeded) {
            // ---- 降级闸门 2：编辑距离超预算（Myers 会退化成平方级）----
            return degradedResult(n, m);
        }
    }

    Result result;
    buildHunks(buildOps(matches, n, m), &result);
    return result;
}

QString LineDiff::toUnifiedText(const Result &result,
                                const QStringList &oldLines,
                                const QStringList &newLines,
                                int contextLines)
{
    // 先把块摊平成一串"逐行操作"，再按"改动点前后各留 contextLines 行"裁切。
    // 这样比直接遍历块简单，而且天然能处理"两个改动靠得很近要合成一个 @@ 段"。
    QList<LineOp> ops;
    for (const Hunk &hunk : result.hunks) {
        switch (hunk.kind) {
        case Kind::Equal:
            for (int i = 0; i < hunk.oldCount; ++i) {
                ops.append(LineOp{Kind::Equal, hunk.oldStart - 1 + i, hunk.newStart - 1 + i});
            }
            break;
        case Kind::Delete:
            for (int i = 0; i < hunk.oldCount; ++i) {
                ops.append(LineOp{Kind::Delete, hunk.oldStart - 1 + i, -1});
            }
            break;
        case Kind::Insert:
            for (int i = 0; i < hunk.newCount; ++i) {
                ops.append(LineOp{Kind::Insert, -1, hunk.newStart - 1 + i});
            }
            break;
        }
    }

    if (ops.isEmpty()) {
        return QString();
    }

    // 找出所有改动点，并按"间隔 <= 2 * contextLines 就并入同一段"分组
    QList<int> changeIndices;
    for (int i = 0; i < ops.size(); ++i) {
        if (ops.at(i).kind != Kind::Equal) {
            changeIndices.append(i);
        }
    }

    QString out;
    out += QStringLiteral("--- old\n+++ new\n");

    if (!changeIndices.isEmpty()) {
        int groupStart = changeIndices.first();
        int groupEnd = changeIndices.first();

        auto flush = [&](int start, int end) {
            const int opCount = static_cast<int>(ops.size());
            const int from = std::max(0, start - contextLines);
            const int to = std::min(opCount - 1, end + contextLines);

            int oldConsumed = 0;
            int newConsumed = 0;
            for (int i = 0; i < from; ++i) {
                if (ops.at(i).kind != Kind::Insert) {
                    ++oldConsumed;
                }
                if (ops.at(i).kind != Kind::Delete) {
                    ++newConsumed;
                }
            }

            int oldInRange = 0;
            int newInRange = 0;
            for (int i = from; i <= to; ++i) {
                if (ops.at(i).kind != Kind::Insert) {
                    ++oldInRange;
                }
                if (ops.at(i).kind != Kind::Delete) {
                    ++newInRange;
                }
            }

            const int oldStart = (oldInRange == 0) ? oldConsumed : oldConsumed + 1;
            const int newStart = (newInRange == 0) ? newConsumed : newConsumed + 1;

            out += QStringLiteral("@@ -%1 +%2 @@\n")
                       .arg(formatRange(oldStart, oldInRange), formatRange(newStart, newInRange));

            for (int i = from; i <= to; ++i) {
                const LineOp &op = ops.at(i);
                switch (op.kind) {
                case Kind::Equal:
                    out += QStringLiteral(" ") + oldLines.value(op.oldIndex) + QStringLiteral("\n");
                    break;
                case Kind::Delete:
                    out += QStringLiteral("-") + oldLines.value(op.oldIndex) + QStringLiteral("\n");
                    break;
                case Kind::Insert:
                    out += QStringLiteral("+") + newLines.value(op.newIndex) + QStringLiteral("\n");
                    break;
                }
            }
        };

        for (int i = 1; i < changeIndices.size(); ++i) {
            if (changeIndices.at(i) - groupEnd <= 2 * contextLines) {
                groupEnd = changeIndices.at(i);
            } else {
                flush(groupStart, groupEnd);
                groupStart = changeIndices.at(i);
                groupEnd = changeIndices.at(i);
            }
        }
        flush(groupStart, groupEnd);
    }

    return out;
}

}  // namespace markdown_editor::core::document
