// LineDiff（B1 自研行级 diff）的契约测试。
//
// 纯算法，**零界面依赖**：不用 QApplication、不拉 Chromium，毫秒级跑完。
// 这正是把 diff 放进 core/document 的回报（本项目的既有惯例：需要自动验证的逻辑
// 都不依赖界面 —— 对照 SyncBridge::buildLineMap() 和 MarkdownOutline::extract()）。
//
// 覆盖路线图 B1.4 的 9 条，其中第 4、5、7 条是重点：
//   * **对称性**：compute(a,b).inserted 必须等于 compute(b,a).deleted。
//     这一条几乎一定会暴露第一次写错的 bug —— 它把"增"和"删"记反的时候，
//     单看一个方向完全看不出来（数字都"合理"）。
//   * **最小性**：拿 O(N·M) 暴力 LCS 当参照物对拍。diff 的价值就是"最少改动"，
//     少了这条断言，"能跑通"和"diff 是对的"就分不开了。
//   * **结构不变式**：把每个 hunk 的行号串起来必须**恰好覆盖**旧文本和新文本。
//     这类"结构不变式"最容易漏测，而一旦错了（行号错位），界面上的高亮会整体偏移。
//
// 另外加了三条路线图没写、但这套接口必须有测试的：
//   * **还原不变式**：按 hunks 从旧文本重建，必须逐行等于新文本 —— 上面那条
//     "结构自洽"只证明行号连得上，这条才证明"内容对得上"。
//   * `splitLines` 的 CRLF / 末尾换行（"只改了换行符被判成全文都变了"是经典假阳性）。
//   * 长行截断的**已声明代价**（钉住它，而不是假装它不存在）。
//
// 跑法：ctest -C Debug -R linediff --output-on-failure

#include "linediff.h"

#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <vector>

using markdown_editor::core::document::LineDiff;

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-66s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// ---------------------------------------------------------------------------
// 参照物与不变量检查
// ---------------------------------------------------------------------------

// 暴力 LCS 长度（O(N·M)）。它是"最小性"的参照物：
// 任何行级 diff 的 inserted + deleted 都必然等于 (n - L) + (m - L)。
int bruteForceLcs(const QStringList &a, const QStringList &b)
{
    const int n = a.size();
    const int m = b.size();
    std::vector<quint16> dp(static_cast<size_t>(n + 1) * static_cast<size_t>(m + 1), 0);
    const size_t width = static_cast<size_t>(m + 1);
    for (int i = n - 1; i >= 0; --i) {
        for (int j = m - 1; j >= 0; --j) {
            quint16 &cell = dp[static_cast<size_t>(i) * width + static_cast<size_t>(j)];
            if (a.at(i) == b.at(j)) {
                cell = static_cast<quint16>(dp[static_cast<size_t>(i + 1) * width
                                              + static_cast<size_t>(j + 1)]
                                            + 1);
            } else {
                cell = std::max(dp[static_cast<size_t>(i + 1) * width + static_cast<size_t>(j)],
                                dp[static_cast<size_t>(i) * width + static_cast<size_t>(j + 1)]);
            }
        }
    }
    return dp[0];
}

// 还原不变式：按 hunks 从**旧文本**重建出新文本。
// 注意 Equal 段落取的是 oldLines（不取 newLines）—— 这样才真的在检查行号对不对。
QStringList rebuildFromHunks(const LineDiff::Result &result, const QStringList &oldLines)
{
    QStringList out;
    for (const LineDiff::Hunk &hunk : result.hunks) {
        switch (hunk.kind) {
        case LineDiff::Kind::Equal:
            for (int i = 0; i < hunk.oldCount; ++i) {
                out.append(oldLines.value(hunk.oldStart - 1 + i));
            }
            break;
        case LineDiff::Kind::Delete:
            break;
        case LineDiff::Kind::Insert:
            // Insert 段落没有内容来源（重建时得靠 newLines），这里用占位符，
            // 由调用方按 newStart-1 从新文本里取 —— 见 rebuildOk()。
            for (int i = 0; i < hunk.newCount; ++i) {
                out.append(QStringLiteral("\x01INS\x02%1").arg(hunk.newStart - 1 + i));
            }
            break;
        }
    }
    return out;
}

// 把 rebuildFromHunks 的占位符换成新文本里的实际行，然后和新文本比对。
bool rebuildOk(const LineDiff::Result &result,
               const QStringList &oldLines,
               const QStringList &newLines,
               QString *why)
{
    const QStringList rebuilt = rebuildFromHunks(result, oldLines);
    if (rebuilt.size() != newLines.size()) {
        *why = QStringLiteral("重建后行数 %1 != 新文本 %2").arg(rebuilt.size()).arg(newLines.size());
        return false;
    }
    for (int i = 0; i < rebuilt.size(); ++i) {
        QString actual = rebuilt.at(i);
        if (actual.startsWith(QStringLiteral("\x01INS\x02"))) {
            const int index = actual.mid(5).toInt();
            actual = newLines.value(index);
        }
        if (actual != newLines.at(i)) {
            *why = QStringLiteral("第 %1 行不一致：「%2」vs「%3」")
                       .arg(i + 1)
                       .arg(actual.left(40), newLines.at(i).left(40));
            return false;
        }
    }
    return true;
}

// 结构不变式：hunks 必须首尾相接、恰好覆盖 n / m 行，且相邻同类块已被合并。
bool structureOk(const LineDiff::Result &result, int n, int m, QString *why)
{
    int consumedOld = 0;
    int consumedNew = 0;
    LineDiff::Kind previous = LineDiff::Kind::Equal;
    bool hasPrevious = false;

    for (int i = 0; i < result.hunks.size(); ++i) {
        const LineDiff::Hunk &hunk = result.hunks.at(i);

        if (hunk.oldStart != consumedOld + 1) {
            *why = QStringLiteral("第 %1 块的 oldStart=%2，应为 %3（前面有空洞或重叠）")
                       .arg(i).arg(hunk.oldStart).arg(consumedOld + 1);
            return false;
        }
        if (hunk.newStart != consumedNew + 1) {
            *why = QStringLiteral("第 %1 块的 newStart=%2，应为 %3")
                       .arg(i).arg(hunk.newStart).arg(consumedNew + 1);
            return false;
        }
        if (hunk.oldCount < 0 || hunk.newCount < 0
            || (hunk.oldCount == 0 && hunk.newCount == 0)) {
            *why = QStringLiteral("第 %1 块是空块").arg(i);
            return false;
        }
        if (hunk.kind == LineDiff::Kind::Equal && hunk.oldCount != hunk.newCount) {
            *why = QStringLiteral("Equal 块两侧行数不等（%1 / %2）")
                       .arg(hunk.oldCount).arg(hunk.newCount);
            return false;
        }
        if (hunk.kind == LineDiff::Kind::Delete && hunk.newCount != 0) {
            *why = QStringLiteral("Delete 块的 newCount 应为 0");
            return false;
        }
        if (hunk.kind == LineDiff::Kind::Insert && hunk.oldCount != 0) {
            *why = QStringLiteral("Insert 块的 oldCount 应为 0");
            return false;
        }
        if (hasPrevious && previous == hunk.kind) {
            *why = QStringLiteral("第 %1 块与上一块同类（应已合并）").arg(i);
            return false;
        }

        consumedOld += hunk.oldCount;
        consumedNew += hunk.newCount;
        previous = hunk.kind;
        hasPrevious = true;
    }

    if (consumedOld != n) {
        *why = QStringLiteral("旧文本覆盖 %1 行，实际有 %2 行").arg(consumedOld).arg(n);
        return false;
    }
    if (consumedNew != m) {
        *why = QStringLiteral("新文本覆盖 %1 行，实际有 %2 行").arg(consumedNew).arg(m);
        return false;
    }
    return true;
}

// 一次性把"结构 + 还原"两条不变式都验掉。
bool invariantsOk(const LineDiff::Result &result,
                  const QStringList &oldLines,
                  const QStringList &newLines,
                  QString *why)
{
    if (!structureOk(result, oldLines.size(), newLines.size(), why)) {
        return false;
    }
    return rebuildOk(result, oldLines, newLines, why);
}

// ---------------------------------------------------------------------------
// 造数据
// ---------------------------------------------------------------------------

QStringList fromText(const QString &text)
{
    return LineDiff::splitLines(text);
}

// 用**固定种子**造随机行：失败时能一字不差地复现（随机测试不复现等于没测）。
// 词表故意很小（5 个词）——让重复行大量出现。重复行是 LCS diff 最容易写错的场景。
QStringList randomLines(QRandomGenerator &rng, int count)
{
    static const QStringList vocabulary = {QStringLiteral("alpha"), QStringLiteral("beta"),
                                          QStringLiteral("gamma"), QStringLiteral("delta"),
                                          QStringLiteral("epsilon")};
    QStringList lines;
    lines.reserve(count);
    for (int i = 0; i < count; ++i) {
        lines.append(vocabulary.at(
            rng.bounded(static_cast<int>(vocabulary.size()))));
    }
    return lines;
}

// 和对拍的 git 命令要用的两个临时文件
QString writeTempFile(const QString &dir, const QString &name, const QStringList &lines)
{
    const QString path = dir + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    for (const QString &line : lines) {
        stream << line << '\n';
    }
    return path;
}

// 直接调 `git diff --no-index --numstat` 拿"增了几行、删了几行"，和自研结果对拍。
// 只比**行数**，不比文本（理由见 linediff.h 里 toUnifiedText 的注释）。
// git 不在就直接跳过（这个项目本来就允许多种环境，不把 git 当硬依赖）。
bool gitNumStat(const QString &oldPath,
                const QString &newPath,
                int *added,
                int *deleted,
                QString *why)
{
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        return false;
    }

    QProcess process;
    process.start(git, QStringList{QStringLiteral("-c"), QStringLiteral("core.quotepath=false"),
                                   QStringLiteral("diff"), QStringLiteral("--no-index"),
                                   QStringLiteral("--numstat"), QStringLiteral("--"),
                                   oldPath, newPath});
    if (!process.waitForFinished(30000)) {
        *why = QStringLiteral("git diff 超时");
        return false;
    }

    const QString out = QString::fromUtf8(process.readAllStandardOutput());
    const QStringList fields = out.split(QLatin1Char('\t'));
    if (fields.size() < 2) {
        *why = QStringLiteral("git diff 输出看不懂：%1").arg(out.left(80));
        return false;
    }
    *added = fields.at(0).toInt();
    *deleted = fields.at(1).toInt();
    return true;
}

// ---------------------------------------------------------------------------

void testSplitLines()
{
    std::printf("\n---- splitLines（换行符取舍）----\n");

    check(fromText(QStringLiteral("a\nb\nc"))
              == QStringList({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}),
          "LF：三行");

    // ★ 经典假阳性：整篇只换了换行符，不该被判成"每行都变了"
    check(fromText(QStringLiteral("a\r\nb\r\nc\r\n"))
              == QStringList({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}),
          "CRLF：剥掉行尾 \\r，和 LF 切出同样的行");

    // ★ 末尾换行符的取舍：按"行内容"比较，不体现差异
    check(fromText(QStringLiteral("a\nb")) == fromText(QStringLiteral("a\nb\n")),
          "末尾有没有换行符：不体现（两个结果相同）");

    check(fromText(QString()).isEmpty(), "空文本：0 行");
    check(fromText(QStringLiteral("\n")).size() == 1, "只有一个换行：1 个空行");
    check(fromText(QStringLiteral("a\n\nb")).size() == 3, "中间空行保留：a / 空 / b");

    // 中间空行 vs 结尾多一个空行，必须区分开
    check(fromText(QStringLiteral("a\nb\n\n")) != fromText(QStringLiteral("a\nb")),
          "结尾多一个空行：和没有空行是两份文本");
}

void testIdenticalAndEmpty()
{
    std::printf("\n---- 1/2. 恒等与空 ----\n");

    const QStringList a = fromText(QStringLiteral("x\ny\nz"));
    const LineDiff::Result same = LineDiff::compute(a, a);
    check(same.identical(), "恒等：identical() == true");
    check(same.hunks.size() == 1 && same.hunks.first().kind == LineDiff::Kind::Equal,
          "恒等：hunks 只有一个 Equal 块");
    check(same.hunks.first().oldCount == 3 && same.hunks.first().newCount == 3,
          "恒等：那一个 Equal 块覆盖全部 3 行");

    const LineDiff::Result both = LineDiff::compute(QStringList(), QStringList());
    check(both.identical() && both.hunks.isEmpty(), "空对空：无差异、无块");

    const LineDiff::Result add = LineDiff::compute(QStringList(), a);
    check(add.insertedLines == 3 && add.deletedLines == 0, "空对非空：3 增 0 删");
    check(!add.degraded, "空对非空：不走降级");
    QString why;
    check(invariantsOk(add, QStringList(), a, &why), "空对非空：结构 + 还原", why);

    const LineDiff::Result del = LineDiff::compute(a, QStringList());
    check(del.insertedLines == 0 && del.deletedLines == 3, "非空对空：0 增 3 删");
    check(invariantsOk(del, a, QStringList(), &why), "非空对空：结构 + 还原", why);
}

void testBasicShapes()
{
    std::printf("\n---- 3. 纯插入 / 纯删除 / 中间替换 / 首尾变化 ----\n");

    QString why;

    // 纯插入（中间）
    {
        const QStringList o = fromText(QStringLiteral("a\nb\nc"));
        const QStringList n = fromText(QStringLiteral("a\nb\nNEW\nc"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.insertedLines == 1 && r.deletedLines == 0, "纯插入：1 增 0 删",
              QStringLiteral("%1/%2").arg(r.insertedLines).arg(r.deletedLines));
        check(invariantsOk(r, o, n, &why), "纯插入：结构 + 还原", why);
    }

    // 纯删除
    {
        const QStringList o = fromText(QStringLiteral("a\nb\nc\nd"));
        const QStringList n = fromText(QStringLiteral("a\nd"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.insertedLines == 0 && r.deletedLines == 2, "纯删除：0 增 2 删");
        check(invariantsOk(r, o, n, &why), "纯删除：结构 + 还原", why);
    }

    // 中间替换（1 换 1，必然是 1 删 + 1 增）
    {
        const QStringList o = fromText(QStringLiteral("a\nb\nc"));
        const QStringList n = fromText(QStringLiteral("a\nX\nc"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.insertedLines == 1 && r.deletedLines == 1, "中间替换：1 增 1 删");
        check(r.hunks.size() == 4, "中间替换：Equal/Delete/Insert/Equal 四块",
              QStringLiteral("实际 %1 块").arg(r.hunks.size()));
        check(invariantsOk(r, o, n, &why), "中间替换：结构 + 还原", why);
    }

    // 首部变化
    {
        const QStringList o = fromText(QStringLiteral("a\nb\nc"));
        const QStringList n = fromText(QStringLiteral("HEAD\nb\nc"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.insertedLines == 1 && r.deletedLines == 1, "首部变化：1 增 1 删");
        check(invariantsOk(r, o, n, &why), "首部变化：结构 + 还原", why);
        check(r.hunks.first().oldStart == 1, "首部变化：第一块从第 1 行开始");
    }

    // 尾部变化
    {
        const QStringList o = fromText(QStringLiteral("a\nb\nc"));
        const QStringList n = fromText(QStringLiteral("a\nb\nTAIL"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.insertedLines == 1 && r.deletedLines == 1, "尾部变化：1 增 1 删");
        check(invariantsOk(r, o, n, &why), "尾部变化：结构 + 还原", why);
    }

    // 只改一个字符，算 1 行改动（不是 0 也不是 2）
    {
        const QStringList o = fromText(QStringLiteral("hello world"));
        const QStringList n = fromText(QStringLiteral("hello worlD"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.insertedLines == 1 && r.deletedLines == 1, "改一个字符：仍算 1 增 1 删");
    }

    // 全是删除 + 全是新增（没有公共行）
    {
        const QStringList o = fromText(QStringLiteral("a\nb"));
        const QStringList n = fromText(QStringLiteral("x\ny"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.insertedLines == 2 && r.deletedLines == 2, "无公共行：2 增 2 删");
        check(invariantsOk(r, o, n, &why), "无公共行：结构 + 还原", why);
    }
}

void testRepeatedLines()
{
    std::printf("\n---- 3b. 重复行（LCS 最容易写错的场景）----\n");

    QString why;

    // 整篇都是同一行，只在中间插一行：
    // 如果实现把"匹配"当成了"第一个能对上的就对上"，行号会整体错位。
    {
        const QStringList o = fromText(QStringLiteral("dup\ndup\ndup\ndup"));
        const QStringList n = fromText(QStringLiteral("dup\ndup\nX\ndup\ndup"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.insertedLines == 1 && r.deletedLines == 0, "全同重复行中间插一行：1 增 0 删",
              QStringLiteral("%1/%2").arg(r.insertedLines).arg(r.deletedLines));
        check(invariantsOk(r, o, n, &why), "全同重复行：结构 + 还原", why);
    }

    // 重复行 + 尾部新增
    {
        const QStringList o = fromText(QStringLiteral("x\ny\nx\ny"));
        const QStringList n = fromText(QStringLiteral("x\ny\nx\ny\nx\ny"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.insertedLines == 2 && r.deletedLines == 0, "重复块整体翻倍：2 增 0 删",
              QStringLiteral("%1/%2").arg(r.insertedLines).arg(r.deletedLines));
        check(invariantsOk(r, o, n, &why), "重复块整体翻倍：结构 + 还原", why);
    }
}

void testSymmetryAndMinimality()
{
    std::printf("\n---- 4/5. 对称性 + 最小性（固定种子随机对拍）----\n");

    QRandomGenerator rng(20260925);  // 固定种子：失败可复现

    int symmetryFailures = 0;
    int minimalityFailures = 0;
    int invariantFailures = 0;
    QString firstSymmetryWhy;
    QString firstMinimalityWhy;
    QString firstInvariantWhy;
    constexpr int kRounds = 40;

    for (int round = 0; round < kRounds; ++round) {
        const QStringList a = randomLines(rng, static_cast<int>(rng.bounded(1, 30)));
        const QStringList b = randomLines(rng, static_cast<int>(rng.bounded(1, 30)));

        const LineDiff::Result forward = LineDiff::compute(a, b);
        const LineDiff::Result backward = LineDiff::compute(b, a);

        // ★ 对称性：正向的"增"必须是反向的"删"
        if (forward.insertedLines != backward.deletedLines
            || forward.deletedLines != backward.insertedLines) {
            ++symmetryFailures;
            if (firstSymmetryWhy.isEmpty()) {
                firstSymmetryWhy = QStringLiteral("第 %1 组：fwd(%2增/%3删) vs bwd(%4增/%5删)")
                                       .arg(round)
                                       .arg(forward.insertedLines)
                                       .arg(forward.deletedLines)
                                       .arg(backward.insertedLines)
                                       .arg(backward.deletedLines);
            }
        }

        // ★ 最小性：和暴力 LCS 对拍
        const int lcs = bruteForceLcs(a, b);
        const int expected = (a.size() - lcs) + (b.size() - lcs);
        if (forward.insertedLines + forward.deletedLines != expected) {
            ++minimalityFailures;
            if (firstMinimalityWhy.isEmpty()) {
                firstMinimalityWhy = QStringLiteral("第 %1 组：得到 %2，最小应为 %3（n=%4 m=%5 L=%6）")
                                         .arg(round)
                                         .arg(forward.insertedLines + forward.deletedLines)
                                         .arg(expected)
                                         .arg(a.size())
                                         .arg(b.size())
                                         .arg(lcs);
            }
        }

        // ★ 结构 + 还原
        QString why;
        if (!invariantsOk(forward, a, b, &why)) {
            ++invariantFailures;
            if (firstInvariantWhy.isEmpty()) {
                firstInvariantWhy = QStringLiteral("第 %1 组：%2").arg(round).arg(why);
            }
        }
    }

    check(symmetryFailures == 0,
          QStringLiteral("对称性：%1 组随机对拍全部满足").arg(kRounds),
          firstSymmetryWhy);
    check(minimalityFailures == 0,
          QStringLiteral("最小性：%1 组随机对拍与暴力 LCS 一致").arg(kRounds),
          firstMinimalityWhy);
    check(invariantFailures == 0,
          QStringLiteral("结构 + 还原：%1 组随机对拍全部成立").arg(kRounds),
          firstInvariantWhy);
}

void testMyersPath()
{
    std::printf("\n---- 11. Myers 路径（超过 DP 上限的输入）----\n");

    // 造一份比 kDpLimit 大的输入，确认它**没有**降级、而且结果仍然最小。
    const int lineCount = LineDiff::kDpLimit + 200;  // 2200 > 2000
    QStringList oldLines;
    oldLines.reserve(lineCount);
    for (int i = 0; i < lineCount; ++i) {
        oldLines.append(QStringLiteral("第 %1 行的内容").arg(i, 6, 10, QLatin1Char('0')));
    }

    QStringList newLines = oldLines;
    newLines[10] = QStringLiteral("这一行被改掉了");
    newLines[1500] = QStringLiteral("这一行也被改掉了");
    newLines.removeAt(2000);
    newLines.insert(500, QStringLiteral("中间插入一行"));

    const LineDiff::Result r = LineDiff::compute(oldLines, newLines);
    check(!r.degraded, QStringLiteral("%1 行输入：不降级（走 Myers）").arg(lineCount));

    // 3 处改动 → 删 3 行（两处替换 + 一处删除）、增 3 行（两处替换 + 一处插入）
    // 实测值比"人眼数的"更可靠：这里直接和暴力 LCS 对拍。
    const int lcs = bruteForceLcs(oldLines, newLines);
    const int expected = (oldLines.size() - lcs) + (newLines.size() - lcs);
    check(r.insertedLines + r.deletedLines == expected,
          QStringLiteral("Myers 结果同样最小（编辑数 %1）").arg(expected),
          QStringLiteral("得到 %1").arg(r.insertedLines + r.deletedLines));

    QString why;
    check(invariantsOk(r, oldLines, newLines, &why), "Myers：结构 + 还原", why);
}

void testUnifiedText()
{
    std::printf("\n---- 10. toUnifiedText（给人看的文本）----\n");

    const QStringList o = fromText(QStringLiteral("keep1\nold\nkeep2\nkeep3\nkeep4\nold2\nkeep5"));
    const QStringList n = fromText(QStringLiteral("keep1\nnew\nkeep2\nkeep3\nkeep4\nnew2\nkeep5"));
    const LineDiff::Result r = LineDiff::compute(o, n);

    const QString text = LineDiff::toUnifiedText(r, o, n, 1);
    int plus = 0;
    int minus = 0;
    const QStringList rows = text.split(QLatin1Char('\n'));
    for (const QString &row : rows) {
        if (row.startsWith(QLatin1Char('+'))) {
            ++plus;
        } else if (row.startsWith(QLatin1Char('-'))) {
            ++minus;
        }
    }
    // 头两行是 "--- old" / "+++ new"，要从计数里扣掉
    check(plus - 1 == r.insertedLines,
          QStringLiteral("unified：`+` 行数（去掉 +++ 头）等于 insertedLines"),
          QStringLiteral("%1 vs %2").arg(plus - 1).arg(r.insertedLines));
    check(minus - 1 == r.deletedLines,
          QStringLiteral("unified：`-` 行数（去掉 --- 头）等于 deletedLines"),
          QStringLiteral("%1 vs %2").arg(minus - 1).arg(r.deletedLines));
    check(text.contains(QStringLiteral("@@")), "unified：带 @@ 段头");
    check(text.contains(QStringLiteral("--- old")) && text.contains(QStringLiteral("+++ new")),
          "unified：带 old/new 头");

    // contextLines = 0 时，只有改动行，没有上下文行
    const QString text0 = LineDiff::toUnifiedText(r, o, n, 0);
    bool hasContextLine = false;
    for (const QString &row : text0.split(QLatin1Char('\n'))) {
        if (row.startsWith(QLatin1Char(' '))) {
            hasContextLine = true;
        }
    }
    check(!hasContextLine, "unified：contextLines=0 时不含上下文行");

    // 恒等 → 空文本（没有改动就没有 @@ 段）
    const LineDiff::Result same = LineDiff::compute(o, o);
    const QString sameText = LineDiff::toUnifiedText(same, o, o, 3);
    check(!sameText.contains(QStringLiteral("@@")), "unified：恒等时不产生 @@ 段");
}

void testAgainstGit()
{
    std::printf("\n---- 6. 与 git 对拍（增删行数）----\n");

    QTemporaryDir temp;
    if (!temp.isValid()) {
        check(false, "临时目录创建失败");
        return;
    }

    // 几个"形态各异"的用例，逐个和 `git diff --no-index --numstat` 对拍
    struct Case
    {
        const char *name;
        QStringList oldLines;
        QStringList newLines;
    };

    QList<Case> cases;
    cases.append({"纯插入", fromText(QStringLiteral("a\nb\nc")),
                  fromText(QStringLiteral("a\nb\nNEW\nc"))});
    cases.append({"纯删除", fromText(QStringLiteral("a\nb\nc\nd")),
                  fromText(QStringLiteral("a\nd"))});
    cases.append({"替换", fromText(QStringLiteral("a\nb\nc")),
                  fromText(QStringLiteral("a\nX\nc"))});
    cases.append({"首尾都改", fromText(QStringLiteral("h1\nm\nh2")),
                  fromText(QStringLiteral("H1\nm\nH2"))});
    cases.append({"重复行", fromText(QStringLiteral("dup\ndup\ndup\ndup")),
                  fromText(QStringLiteral("dup\nX\ndup\ndup"))});
    cases.append({"无公共行", fromText(QStringLiteral("a\nb\nc")),
                  fromText(QStringLiteral("x\ny\nz"))});

    int compared = 0;
    int mismatches = 0;
    bool gitAvailable = true;
    QString firstWhy;

    for (int i = 0; i < cases.size(); ++i) {
        const Case &one = cases.at(i);
        const QString oldPath = writeTempFile(temp.path(), QStringLiteral("old%1.txt").arg(i),
                                              one.oldLines);
        const QString newPath = writeTempFile(temp.path(), QStringLiteral("new%1.txt").arg(i),
                                              one.newLines);
        if (oldPath.isEmpty() || newPath.isEmpty()) {
            check(false, QStringLiteral("临时文件写入失败：%1")
                             .arg(QString::fromLatin1(one.name)));
            continue;
        }

        int gitAdded = 0;
        int gitDeleted = 0;
        QString why;
        if (!gitNumStat(oldPath, newPath, &gitAdded, &gitDeleted, &why)) {
            gitAvailable = false;
            break;
        }

        const LineDiff::Result r = LineDiff::compute(one.oldLines, one.newLines);
        ++compared;
        if (r.insertedLines != gitAdded || r.deletedLines != gitDeleted) {
            ++mismatches;
            if (firstWhy.isEmpty()) {
                firstWhy = QStringLiteral("%1：自研 %2增/%3删，git %4增/%5删")
                               .arg(QString::fromLatin1(one.name))
                               .arg(r.insertedLines)
                               .arg(r.deletedLines)
                               .arg(gitAdded)
                               .arg(gitDeleted);
            }
        }
    }

    if (!gitAvailable) {
        std::printf("  （本机没有 git，跳过对拍 —— 这条不是硬依赖）\n");
        return;
    }

    check(mismatches == 0,
          QStringLiteral("与 git 对拍：%1 个用例的增删行数全部一致").arg(compared),
          firstWhy);
}

void testDegradedPaths()
{
    std::printf("\n---- 9. 降级路径（三个闸门）----\n");

    QString why;

    // 闸门 1a：文档规模超 kFallbackThreshold
    {
        const int lineCount = LineDiff::kFallbackThreshold + 10;
        QStringList o;
        QStringList n;
        o.reserve(lineCount);
        n.reserve(lineCount);
        for (int i = 0; i < lineCount; ++i) {
            o.append(QStringLiteral("旧 %1").arg(i));
            n.append(QStringLiteral("新 %1").arg(i));
        }

        QElapsedTimer clock;
        clock.start();
        const LineDiff::Result r = LineDiff::compute(o, n);
        const qint64 elapsed = clock.elapsed();

        check(r.degraded, "超规模：标记为降级（不是失败）");
        check(r.insertedLines == lineCount && r.deletedLines == lineCount,
              "超规模：整段替换 = 全删 + 全增");
        check(elapsed < 3000, "超规模：立刻返回，不试图对齐",
              QStringLiteral("%1 ms").arg(elapsed));
        check(invariantsOk(r, o, n, &why), "超规模：降级结果仍满足结构 + 还原", why);
    }

    // 闸门 1b：行数差超 kLineCountDeltaThreshold（等价于"编辑距离必然超预算"的提前判定）
    {
        QStringList o;
        QStringList n;
        for (int i = 0; i < 100; ++i) {
            o.append(QStringLiteral("短 %1").arg(i));
        }
        for (int i = 0; i < 100 + LineDiff::kLineCountDeltaThreshold + 50; ++i) {
            n.append(QStringLiteral("长 %1").arg(i));
        }

        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.degraded, "行数差超阈值：标记为降级");
        check(invariantsOk(r, o, n, &why), "行数差超阈值：降级结果仍满足结构 + 还原", why);
    }

    // 闸门 2：Myers 编辑距离超预算（规模没超，但两份文件完全不同）
    {
        constexpr int kLines = 5000;
        QStringList o;
        QStringList n;
        o.reserve(kLines);
        n.reserve(kLines);
        for (int i = 0; i < kLines; ++i) {
            o.append(QStringLiteral("完全不同的 A 侧第 %1 行").arg(i));
            n.append(QStringLiteral("完全不同的 B 侧第 %1 行").arg(i));
        }

        QElapsedTimer clock;
        clock.start();
        const LineDiff::Result r = LineDiff::compute(o, n);
        const qint64 elapsed = clock.elapsed();

        check(r.degraded, "编辑距离超预算：标记为降级（D≈N+M，Myers 会退化成平方级）");
        check(elapsed < 3000, "编辑距离超预算：在预算处刹住，不跑满 O(N·D)",
              QStringLiteral("%1 ms").arg(elapsed));
        check(invariantsOk(r, o, n, &why), "编辑距离超预算：降级结果仍满足结构 + 还原", why);
    }
}

void testPerfSmoke()
{
    std::printf("\n---- 8. 性能冒烟（5 万行只改 3 行）----\n");

    constexpr int kLines = 50000;
    QStringList oldLines;
    oldLines.reserve(kLines);
    for (int i = 0; i < kLines; ++i) {
        oldLines.append(QStringLiteral("第 %1 行：这是一段用来撑规模的正文内容").arg(i, 6, 10,
                                                                              QLatin1Char('0')));
    }

    QStringList newLines = oldLines;
    newLines[12345] = QStringLiteral("改动一");
    newLines[30000] = QStringLiteral("改动二");
    newLines[49999] = QStringLiteral("改动三");

    QElapsedTimer clock;
    clock.start();
    const LineDiff::Result r = LineDiff::compute(oldLines, newLines);
    const qint64 elapsed = clock.elapsed();

    std::printf("  5 万行 / 改 3 行：%lld ms（%d 增 %d 删）\n",
                static_cast<long long>(elapsed), r.insertedLines, r.deletedLines);

    check(!r.degraded, "5 万行：不降级（D 很小，Myers 的优势正在这里）");
    check(r.insertedLines == 3 && r.deletedLines == 3, "5 万行 / 改 3 行：3 增 3 删",
          QStringLiteral("%1/%2").arg(r.insertedLines).arg(r.deletedLines));
    check(elapsed < 1000, "5 万行 / 改 3 行：耗时 < 1s", QStringLiteral("%1 ms").arg(elapsed));

    QString why;
    check(invariantsOk(r, oldLines, newLines, &why), "5 万行：结构 + 还原", why);
}

void testDeclaredLimits()
{
    std::printf("\n---- 12. 已声明的取舍（钉住它，而不是假装它不存在）----\n");

    // 长行截断：两行只在第 4500 个字符处不同，超过 kMaxHashChars(4096) 的部分不参与比较。
    // 结果：被判成"相同"。这是文件头降级表里写明的代价，这里把它钉成测试 ——
    // 与其让它在生产里被当成随机 bug，不如明确地测出来。
    {
        const QString prefix = QString(5000, QLatin1Char('p'));
        const QStringList o{prefix + QStringLiteral("AAA")};
        const QStringList n{prefix + QStringLiteral("BBB")};

        const LineDiff::Result r = LineDiff::compute(o, n);
        std::printf("  （长行截断的已知代价：>%d 字符的部分不参与比较）\n", LineDiff::kMaxHashChars);
        check(r.identical(), "超长行只在 4096 字符之后不同：按已声明的取舍判为相同");
    }

    // 但 4096 字符之内的差异必须照常检出
    {
        const QStringList o{QString(100, QLatin1Char('p')) + QStringLiteral("AAA")};
        const QStringList n{QString(100, QLatin1Char('p')) + QStringLiteral("BBB")};
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(!r.identical(), "4096 字符之内的差异：照常检出");
    }

    // CRLF 与 LF 混用（同一个文件里既有 \r\n 又有 \n）不该让全文变成"全改"
    {
        const QStringList o = fromText(QStringLiteral("a\r\nb\nc\r\n"));
        const QStringList n = fromText(QStringLiteral("a\nb\r\nc\n"));
        const LineDiff::Result r = LineDiff::compute(o, n);
        check(r.identical(), "混用 CRLF/LF：不产生任何差异（假阳性检查）");
    }
}

}  // namespace

int main()
{
    std::printf("LineDiff 契约测试（B1 自研行级 diff）\n");

    testSplitLines();
    testIdenticalAndEmpty();
    testBasicShapes();
    testRepeatedLines();
    testSymmetryAndMinimality();
    testMyersPath();
    testUnifiedText();
    testAgainstGit();
    testDegradedPaths();
    testPerfSmoke();
    testDeclaredLimits();

    std::printf("\n================ %s ================\n",
                g_fail == 0 ? "全部通过" : "有失败");
    if (g_fail != 0) {
        std::printf("失败 %d 项\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
