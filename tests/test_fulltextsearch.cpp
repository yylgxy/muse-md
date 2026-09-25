// FullTextSearch（5.5 全文搜索）+ SearchPanel 的契约测试。
//
// 需要 QApplication（SearchPanel 是控件）和 Qt6::Sql（索引是真的 SQLite 库）。
//
// 分三层测，三层都能 100% 自动验证：
//   1. **纯函数**：用户输入怎么变成查询、哪些文件该进索引、命中位置怎么算、
//      结果行太长怎么截 —— 这些是"只在特定输入下才出错"的地方，必须钉死。
//   2. **真的建索引 + 搜索**：临时目录里的真 md 文件 → 真 SQLite 库 → 真查询。
//      这里最要紧的是中文：FTS5 的 trigram 分词器对**少于 3 个字符**的查询用 MATCH
//      一定搜不到（实测），而"缓存""索引"这种二字词恰恰最常见，所以短查询必须走 LIKE。
//      这一段就是那条规则的回归测试。
//   3. **面板**：搜索 → 结果列表 → "点开第几条" → resultActivated(路径, 行号)。
//      面板不打开文件（那是主窗口的事），所以它的行为可以脱离 Chromium 完整验证。
//
// 索引库全部落在临时目录里，不碰用户真实的 <AppData>/Dev/MarkdownEditor/search-index.sqlite。
//
// 跑法：ctest -C Debug --output-on-failure

#include "fulltextsearch.h"
#include "searchpanel.h"
#include "searchresultdelegate.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdio>

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

// 等索引在后台结束（或失败/取消）。返回 false = 超时。
//
// ★ 为什么必须等（A1 线程化之后测试侧必须跟着变的地方）：
//   buildIndex() 现在只表示"任务已提交"，索引跑在工作线程里。
//   直接断言状态等于测时序运气 —— 单核 CI 机器上必挂。
//
// ★ 为什么用 isIndexing() 而不是断言界面文案：
//   文案会改（"索引完成：…" 这句话改一个字测试就红），而"在不在索引"是状态，不会改。
//   这是路线图 A1 Step 6 推荐的做法。
bool waitForIndexFinished(SearchPanel &panel, int timeoutMs = 30000)
{
    QElapsedTimer clock;
    clock.start();
    while (panel.isIndexing()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (clock.elapsed() > timeoutMs) {
            return false;
        }
    }
    return true;
}

void writeFile(const QString &path, const QString &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(content.toUtf8());
        file.close();
    }
}

// 把命中拼成 "文件名:行号" 的列表，便于一眼看出对不对
QString describe(const QList<SearchHit> &hits)
{
    QStringList parts;
    for (const SearchHit &hit : hits) {
        parts << QStringLiteral("%1:%2").arg(QFileInfo(hit.filePath).fileName()).arg(hit.line);
    }
    return parts.join(QStringLiteral(", "));
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString base = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-fulltext-test"));
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    const QString docs = base + QStringLiteral("/docs");
    const QString aMd = docs + QStringLiteral("/a.md");
    const QString bMd = docs + QStringLiteral("/sub/b.md");
    const QString cTxt = docs + QStringLiteral("/c.txt");
    const QString dMd = docs + QStringLiteral("/empty.md");

    // a.md：3 行非空（第 2 行特意留空，用来验证"行号按原文来，不被空行挤掉"）
    writeFile(aMd,
              QStringLiteral("# 缓存设计\n"
                             "\n"
                             "这一行讲到了缓存服务的 LRU 淘汰策略\n"
                             "Cache manager 的实现\n"));
    writeFile(bMd, QStringLiteral("缓存与索引\n英文 keyword here\n"));
    writeFile(cTxt, QStringLiteral("缓存（这个文件不是 Markdown，不该进索引）\n"));
    writeFile(dMd, QStringLiteral("\n\n"));

    // ============================ A. 纯函数 ============================
    {
        std::printf("---- A. 纯函数 ----\n");

        check(FullTextSearch::normalizeQuery(QStringLiteral("  缓存  ")) == QStringLiteral("缓存"),
              QStringLiteral("normalizeQuery: 去掉首尾空白"));
        check(FullTextSearch::normalizeQuery(QString()).isEmpty(), QStringLiteral("normalizeQuery: 空还是空"));

        // 短于 3 个字符必须走 LIKE —— 这条是中文搜索的生死线
        check(FullTextSearch::needsLikeFallback(QStringLiteral("缓存")),
              QStringLiteral("needsLikeFallback: 中文 2 字 -> 必须走 LIKE"));
        check(FullTextSearch::needsLikeFallback(QStringLiteral("ab")), QStringLiteral("needsLikeFallback: 2 字母 -> LIKE"));
        check(FullTextSearch::needsLikeFallback(QString()), QStringLiteral("needsLikeFallback: 空 -> LIKE（会直接返回空结果）"));
        check(!FullTextSearch::needsLikeFallback(QStringLiteral("缓存服")),
              QStringLiteral("needsLikeFallback: 中文 3 字 -> 可以用 MATCH"));
        check(!FullTextSearch::needsLikeFallback(QStringLiteral("cache")), QStringLiteral("needsLikeFallback: 英文 5 字母 -> MATCH"));

        // 用户输入永远当"数据"：整段包成短语，里面的引号翻倍
        check(FullTextSearch::toMatchExpression(QStringLiteral("缓存")) == QStringLiteral("\"缓存\""),
              QStringLiteral("toMatchExpression: 包成一个短语"));
        check(FullTextSearch::toMatchExpression(QStringLiteral("a\"b")) == QStringLiteral("\"a\"\"b\""),
              QStringLiteral("toMatchExpression: 内部的双引号翻倍（不会把短语提前结束）"),
              FullTextSearch::toMatchExpression(QStringLiteral("a\"b")));
        check(FullTextSearch::toMatchExpression(QStringLiteral("AND")) == QStringLiteral("\"AND\""),
              QStringLiteral("toMatchExpression: AND/OR/NEAR 都只是普通字符"));

        check(FullTextSearch::toLikePattern(QStringLiteral("50%")) == QStringLiteral("%50\\%%"),
              QStringLiteral("toLikePattern: 转义 %"), FullTextSearch::toLikePattern(QStringLiteral("50%")));
        check(FullTextSearch::toLikePattern(QStringLiteral("a_b")) == QStringLiteral("%a\\_b%"),
              QStringLiteral("toLikePattern: 转义 _"));
        check(FullTextSearch::toLikePattern(QStringLiteral("a\\b")) == QStringLiteral("%a\\\\b%"),
              QStringLiteral("toLikePattern: 转义反斜杠（它同时也是 LIKE 的转义符）"));

        check(FullTextSearch::isIndexableFile(QStringLiteral("note.md"))
                  && FullTextSearch::isIndexableFile(QStringLiteral("NOTE.MD"))
                  && FullTextSearch::isIndexableFile(QStringLiteral("b.markdown")),
              QStringLiteral("isIndexableFile: .md / .markdown（大小写不敏感）都索引"));
        check(!FullTextSearch::isIndexableFile(QStringLiteral("a.txt"))
                  && !FullTextSearch::isIndexableFile(QStringLiteral("a.md.bak"))
                  && !FullTextSearch::isIndexableFile(QStringLiteral("noext")),
              QStringLiteral("isIndexableFile: 别的后缀都不索引"));

        check(FullTextSearch::findMatch(QStringLiteral("Cache manager"), QStringLiteral("cache")) == 0,
              QStringLiteral("findMatch: 不区分大小写，位置从 0 起算"));
        // "这一行讲到了缓存服务"：这0 一1 行2 讲3 到4 了5 缓6 —— 命中在 6
        check(FullTextSearch::findMatch(QStringLiteral("这一行讲到了缓存服务"), QStringLiteral("缓存")) == 6,
              QStringLiteral("findMatch: 中文也能定位到下标"),
              QStringLiteral("%1").arg(FullTextSearch::findMatch(QStringLiteral("这一行讲到了缓存服务"), QStringLiteral("缓存"))));
        check(FullTextSearch::findMatch(QStringLiteral("abc"), QStringLiteral("zz")) == -1,
              QStringLiteral("findMatch: 找不到 -> -1"));
        check(FullTextSearch::findMatch(QStringLiteral("abc"), QString()) == -1,
              QStringLiteral("findMatch: 空关键词 -> -1"));

        // 结果行的截断规则（显示用）
        SearchHit shortHit;
        shortHit.text = QStringLiteral("  缓存服务  ");
        shortHit.matchStart = 2;
        shortHit.matchLength = 4;  // C1：命中区间换算要用它（"缓存服务" 4 个字）
        check(SearchPanel::displayTextFor(shortHit) == QStringLiteral("缓存服务"),
              QStringLiteral("displayTextFor: 短行去掉两边空白"));

        SearchHit longHit;
        longHit.text = QString(400, QLatin1Char('x')) + QStringLiteral("缓存服务") + QString(400, QLatin1Char('y'));
        longHit.matchStart = 400;
        longHit.matchLength = 4;
        const QString shown = SearchPanel::displayTextFor(longHit, 60);
        check(shown.startsWith(QStringLiteral("…")) && shown.contains(QStringLiteral("缓存服务")) && shown.endsWith(QStringLiteral("…")),
              QStringLiteral("displayTextFor: 长行围绕命中截断（命中一定看得见）"),
              QStringLiteral("%1 字符").arg(shown.size()));
        check(shown.size() <= 62, QStringLiteral("displayTextFor: 长度受控（最多 maxChars + 两个省略号）"));

        // ---------------- ★ C1：命中区间换算 ----------------
        // 这是 C1 最容易写错的地方：displayTextFor() 会 trim、会截断、会补省略号，
        // 三种变换都会让"原文下标"失效。下面每条对应一种平移。
        {
            // ① trim 平移：前导 2 个空格被去掉，命中下标要从 2 变成 0
            const SearchPanel::DisplaySpan s1 = SearchPanel::displaySpanFor(shortHit);
            check(s1.start == 0 && s1.length == 4,
                  QStringLiteral("displaySpanFor: trim 之后命中下标跟着平移（2 -> 0）"),
                  QStringLiteral("start=%1 len=%2").arg(s1.start).arg(s1.length));

            // ② 截断平移 + ③ 省略号占位：命中原文下标 400，截断从 380 开始，
            //    前面补了一个 "…"，所以显示文本里的起点应该是 400-380+1 = 21
            const SearchPanel::DisplaySpan s2 = SearchPanel::displaySpanFor(longHit, 60);
            const QString shownLong = SearchPanel::displayTextFor(longHit, 60);
            check(s2.start == 21 && s2.length == 4,
                  QStringLiteral("displaySpanFor: 截断+省略号之后命中下标重新算过（400 -> 21）"),
                  QStringLiteral("start=%1 len=%2").arg(s2.start).arg(s2.length));
            // 最硬的一条：按算出来的区间去切显示文本，必须正好是命中词
            check(shownLong.mid(s2.start, s2.length) == QStringLiteral("缓存服务"),
                  QStringLiteral("displaySpanFor: 区间切出来正好是命中词（这是高亮不错位的充要条件）"),
                  QStringLiteral("切出「%1」").arg(shownLong.mid(s2.start, s2.length)));

            // 短行不截断时，区间也必须能切出命中词（这一条没有截断/省略号平移，
            // 只有 trim 平移 —— 用来把"平移算多了"的错误挡掉）
            check(SearchPanel::displayTextFor(shortHit).mid(s1.start, s1.length)
                      == QStringLiteral("缓存服务"),
                  QStringLiteral("displaySpanFor: 短行（不截断）也能切出命中词"),
                  QStringLiteral("切出「%1」")
                      .arg(SearchPanel::displayTextFor(shortHit).mid(s1.start, s1.length)));

            // 没能定位到命中（matchStart == -1）→ 不产生区间，不崩
            SearchHit unknown;
            unknown.text = QStringLiteral("大小写不一致的一行");
            unknown.matchStart = -1;
            const SearchPanel::DisplaySpan s3 = SearchPanel::displaySpanFor(unknown);
            check(s3.length == 0, QStringLiteral("displaySpanFor: 定位不到命中时不给区间（界面不高亮，也不崩）"));

            // 命中正好在截断边界上：不能算出越界区间
            SearchHit edge;
            edge.text = QString(200, QLatin1Char('a')) + QStringLiteral("目标");
            edge.matchStart = 200;
            edge.matchLength = 2;
            const SearchPanel::DisplaySpan s4 = SearchPanel::displaySpanFor(edge, 40);
            const QString shownEdge = SearchPanel::displayTextFor(edge, 40);
            check(s4.start >= 0 && s4.start + s4.length <= shownEdge.size(),
                  QStringLiteral("displaySpanFor: 边界命中不会算出越界区间"),
                  QStringLiteral("start=%1 len=%2 总长=%3").arg(s4.start).arg(s4.length).arg(shownEdge.size()));
        }
    }

    // ============================ B. 建索引 ============================
    const QString dbPath = base + QStringLiteral("/index.sqlite");
    {
        std::printf("---- B. 建索引（真 SQLite + FTS5）----\n");

        FullTextSearch engine;
        engine.setIndexPath(dbPath);
        check(engine.indexPath() == dbPath, QStringLiteral("索引库路径可改（测试不碰用户真库）"));

        QString error;
        check(!engine.isOpen(), QStringLiteral("open 之前：isOpen = false"));
        check(engine.search(QStringLiteral("缓存")).isEmpty(), QStringLiteral("没打开就搜索：返回空结果而不是崩"));

        check(engine.open(&error), QStringLiteral("open: 打开（不存在就创建）"), error);
        check(engine.isOpen(), QStringLiteral("open 之后：isOpen = true"));
        check(QFileInfo::exists(dbPath), QStringLiteral("open: 库文件真的建出来了"));

        error.clear();
        const IndexStats stats = engine.indexDirectory(docs, &error);
        check(error.isEmpty(), QStringLiteral("建索引: 没有报错"), error);
        check(stats.filesFound == 3, QStringLiteral("建索引: 扫到 3 个 md（a.md / sub/b.md / empty.md，c.txt 不算）"),
              QStringLiteral("%1 个").arg(stats.filesFound));
        check(stats.filesIndexed == 3, QStringLiteral("建索引: 3 个都建了"), QStringLiteral("%1 个").arg(stats.filesIndexed));
        check(stats.filesSkipped == 0, QStringLiteral("建索引: 第一次没有跳过的"));
        check(stats.linesIndexed == 5, QStringLiteral("建索引: 5 行非空内容（a.md 3 行 + b.md 2 行 + 空文件 0 行）"),
              QStringLiteral("%1 行").arg(stats.linesIndexed));
        check(engine.indexedFileCount() == 3, QStringLiteral("索引现状: 3 个文件"), QString::number(engine.indexedFileCount()));
        check(engine.indexedLineCount() == 5, QStringLiteral("索引现状: 5 行"), QString::number(engine.indexedLineCount()));
        check(engine.indexedFiles().size() == 3, QStringLiteral("indexedFiles: 列出了 3 个路径"));

        // 反复建索引：内容没变，应该全部跳过（这就是"点两次很快"的原因）
        const IndexStats again = engine.indexDirectory(docs, &error);
        check(again.filesIndexed == 0 && again.filesSkipped == 3,
              QStringLiteral("再建一次: 全部跳过（按修改时间+大小判断没变过）"),
              QStringLiteral("新建 %1 / 跳过 %2").arg(again.filesIndexed).arg(again.filesSkipped));

        error.clear();
        const IndexStats bad = engine.indexDirectory(base + QStringLiteral("/没有这个目录"), &error);
        check(!error.isEmpty() && bad.filesFound == 0, QStringLiteral("建索引: 目录不存在 -> 给出原因"), error);
    }

    // ============================ C. 搜索 ============================
    {
        std::printf("---- C. 搜索 ----\n");

        FullTextSearch engine;
        engine.setIndexPath(dbPath);
        QString error;
        check(engine.open(&error), QStringLiteral("重新打开同一个库（模拟重启程序）"), error);
        check(engine.indexedFileCount() == 3, QStringLiteral("重启后: 索引内容还在（不是内存里的东西）"));

        // ★ 中文 2 字：trigram 的 MATCH 搜不到，必须靠 LIKE 兜住
        const QList<SearchHit> cn2 = engine.search(QStringLiteral("缓存"), 100, &error);
        check(error.isEmpty(), QStringLiteral("搜索 缓存: 没报错"), error);
        check(cn2.size() == 3, QStringLiteral("搜索 缓存（2 字）: 命中 3 条"), describe(cn2));
        if (cn2.size() == 3) {
            check(QFileInfo(cn2.at(0).filePath).fileName() == QStringLiteral("a.md") && cn2.at(0).line == 1,
                  QStringLiteral("搜索 缓存: 第一条是 a.md 第 1 行"),
                  QStringLiteral("%1:%2").arg(QFileInfo(cn2.at(0).filePath).fileName()).arg(cn2.at(0).line));
            check(cn2.at(1).line == 3, QStringLiteral("搜索 缓存: 第二条是 a.md 第 3 行（空行不影响行号）"),
                  QStringLiteral("行 %1").arg(cn2.at(1).line));
            check(QFileInfo(cn2.at(2).filePath).fileName() == QStringLiteral("b.md") && cn2.at(2).line == 1,
                  QStringLiteral("搜索 缓存: 第三条在子目录 sub/b.md（递归扫描）"));
            check(cn2.at(1).matchStart == 6 && cn2.at(1).matchLength == 2,
                  QStringLiteral("搜索 缓存: 命中位置算出来了（给界面高亮用）"),
                  QStringLiteral("start=%1 len=%2").arg(cn2.at(1).matchStart).arg(cn2.at(1).matchLength));
            check(cn2.at(1).text == QStringLiteral("这一行讲到了缓存服务的 LRU 淘汰策略"),
                  QStringLiteral("搜索 缓存: 带回整行原文"));
        }

        // 中文 3 字：走 FTS5 的 MATCH 这条路
        const QList<SearchHit> cn3 = engine.search(QStringLiteral("缓存服"), 100, &error);
        check(cn3.size() == 1 && cn3.at(0).line == 3,
              QStringLiteral("搜索 缓存服（3 字，走 MATCH）: 命中 a.md 第 3 行"), describe(cn3));

        // 英文：大小写不敏感
        const QList<SearchHit> en = engine.search(QStringLiteral("cache"), 100, &error);
        check(en.size() == 1 && en.at(0).line == 4, QStringLiteral("搜索 cache: 命中 a.md 第 4 行"), describe(en));
        check(engine.search(QStringLiteral("CACHE"), 100, &error).size() == 1,
              QStringLiteral("搜索 CACHE: 大小写不敏感，同样命中"));

        check(engine.search(QStringLiteral("keyword"), 100, &error).size() == 1,
              QStringLiteral("搜索 keyword: 子目录里的文件也搜得到"));

        // 搜不到 / 空关键词 / 上限
        check(engine.search(QStringLiteral("根本没有这个词条"), 100, &error).isEmpty(),
              QStringLiteral("搜索 没有的词: 空结果"));
        check(error.isEmpty(), QStringLiteral("搜索 没有的词: 也不算错误"));
        check(engine.search(QString(), 100, &error).isEmpty(), QStringLiteral("搜索 空关键词: 空结果"));
        check(engine.search(QStringLiteral("缓存"), 0, &error).isEmpty(), QStringLiteral("搜索 limit=0: 空结果"));
        check(engine.search(QStringLiteral("缓存"), 1, &error).size() == 1, QStringLiteral("搜索 limit=1: 只给一条"));

        // ★ 用户的输入永远只是数据：这些字符对 FTS5 来说是语法，必须不会炸
        std::printf("---- C2. 脏输入（FTS5 语法字符）----\n");
        const QStringList nasty{QStringLiteral("\""),
                                QStringLiteral("*"),
                                QStringLiteral("AND"),
                                QStringLiteral("(缓存)"),
                                QStringLiteral("缓存 AND 服务"),
                                QStringLiteral("NEAR("),
                                QStringLiteral("^"),
                                QStringLiteral("-缓存"),
                                QStringLiteral("缓存\\")};
        int survived = 0;
        for (const QString &raw : nasty) {
            QString nastyError;
            const QList<SearchHit> hits = engine.search(raw, 10, &nastyError);
            if (nastyError.isEmpty()) {
                ++survived;
            } else {
                std::printf("    %-16s -> 报错: %s\n", raw.toUtf8().constData(), nastyError.toUtf8().constData());
            }
            Q_UNUSED(hits);
        }
        check(survived == nasty.size(), QStringLiteral("脏输入: 9 种 FTS5 语法字符全部不报错（当成普通文本搜）"),
              QStringLiteral("通过 %1/%2").arg(survived).arg(nasty.size()));
        check(engine.search(QStringLiteral("(\"缓存\")"), 10, &error).isEmpty(),
              QStringLiteral("脏输入: 括号引号被当成字面量 -> 没有匹配（而不是查出所有东西）"));
    }

    // ============================ D. 增量更新与快照语义 ============================
    {
        std::printf("---- D. 增量更新 ----\n");

        FullTextSearch engine;
        engine.setIndexPath(dbPath);
        QString error;
        engine.open(&error);

        // 改一个文件：只应该重建这一个（新内容里去掉了"缓存"，加上了"索引"）
        writeFile(aMd, QStringLiteral("# 新的标题\n\n改过了，这里提到了索引\n"));
        const IndexStats stats = engine.indexDirectory(docs, &error);
        check(stats.filesIndexed == 1 && stats.filesSkipped == 2,
              QStringLiteral("改过一个文件: 只重建 1 个、跳过 2 个"),
              QStringLiteral("新建 %1 / 跳过 %2").arg(stats.filesIndexed).arg(stats.filesSkipped));

        const QList<SearchHit> afterEdit = engine.search(QStringLiteral("缓存"), 100, &error);
        check(afterEdit.size() == 1 && QFileInfo(afterEdit.at(0).filePath).fileName() == QStringLiteral("b.md"),
              QStringLiteral("改过之后: a.md 的旧匹配消失了，只剩 b.md"), describe(afterEdit));
        check(engine.search(QStringLiteral("索引"), 100, &error).size() == 2,
              QStringLiteral("改过之后: 新加的内容能搜到（a.md 和 b.md）"),
              describe(engine.search(QStringLiteral("索引"), 100, &error)));

        // 删一个文件：它的索引行也要跟着消失
        check(QFile::remove(bMd), QStringLiteral("准备: 删掉 sub/b.md"));
        const IndexStats afterRemove = engine.indexDirectory(docs, &error);
        check(afterRemove.filesRemoved == 1, QStringLiteral("删过文件: 清理了 1 个文件的旧记录"),
              QStringLiteral("%1 个").arg(afterRemove.filesRemoved));
        check(engine.indexedFileCount() == 2, QStringLiteral("删过之后: 索引里只剩 2 个文件"));
        check(engine.search(QStringLiteral("keyword"), 100, &error).isEmpty(),
              QStringLiteral("删过之后: 那个文件里的词搜不到了"));

        // 索引 = 目录的快照：换成索引另一个目录时，旧目录的记录会被清掉
        const QString other = base + QStringLiteral("/other");
        QDir().mkpath(other);
        writeFile(other + QStringLiteral("/x.md"), QStringLiteral("另一个目录里的内容\n"));
        const IndexStats switched = engine.indexDirectory(other, &error);
        check(switched.filesFound == 1 && switched.filesRemoved == 2,
              QStringLiteral("换目录: 索引新目录的同时清掉了旧目录的 2 个文件"),
              QStringLiteral("清理 %1 个").arg(switched.filesRemoved));
        check(engine.indexedFileCount() == 1, QStringLiteral("换目录之后: 索引里就只有新目录的文件"));
        check(engine.search(QStringLiteral("索引"), 100, &error).isEmpty(),
              QStringLiteral("换目录之后: 旧目录里的词不再命中（不会给出已经不在索引里的路径）"));

        // 清空
        check(engine.clearIndex(&error), QStringLiteral("clearIndex: 成功"), error);
        check(engine.indexedFileCount() == 0 && engine.indexedLineCount() == 0,
              QStringLiteral("clearIndex: 文件和行都归零"));
        check(engine.search(QStringLiteral("另一个目录"), 100, &error).isEmpty(),
              QStringLiteral("clearIndex: 搜不到了"));
    }

    // ============================ E. 搜索面板 ============================
    {
        std::printf("---- E. 搜索面板 ----\n");

        // 重新造一份干净的文档目录给面板用
        const QString panelDocs = base + QStringLiteral("/panel");
        QDir().mkpath(panelDocs);
        writeFile(panelDocs + QStringLiteral("/one.md"),
                  QStringLiteral("第一行\n缓存服务的第二行\n第三行也有缓存\n"));
        writeFile(panelDocs + QStringLiteral("/two.md"), QStringLiteral("这里也有缓存\n"));
        writeFile(panelDocs + QStringLiteral("/skip.txt"), QStringLiteral("缓存（不是 md）\n"));

        const QString panelDb = base + QStringLiteral("/panel-index.sqlite");
        SearchPanel panel;
        panel.setIndexPath(panelDb);
        panel.setDirectory(panelDocs);
        check(panel.directory() == QDir::toNativeSeparators(panelDocs),
              QStringLiteral("面板: 目录用的是 native 分隔符（显示给用户看的）"), panel.directory());

        // ★ A1 之后 buildIndex() 只表示"任务已提交"，索引在工作线程里跑，
        //   所以这里必须等它结束再断言状态和索引内容。
        check(panel.buildIndex(), QStringLiteral("面板: 索引任务被接受"));
        check(panel.isIndexing(), QStringLiteral("面板: 提交之后立刻处于「正在索引」"));
        check(waitForIndexFinished(panel), QStringLiteral("面板: 索引在后台完成（不超时）"));
        check(!panel.isIndexing(), QStringLiteral("面板: 索引结束后不再是「正在索引」"));
        check(panel.statusText().contains(QStringLiteral("索引完成")),
              QStringLiteral("面板: 状态栏报告索引完成"), panel.statusText());
        check(panel.engine()->indexedFileCount() == 2, QStringLiteral("面板: 索引里是 2 个 md"));

        QStringList activated;
        // C1：信号参数从 (路径, 行号) 变成了整条 SearchHit。
        QObject::connect(&panel, &SearchPanel::resultActivated, [&activated](const SearchHit &hit) {
            activated << QStringLiteral("%1:%2").arg(QFileInfo(hit.filePath).fileName()).arg(hit.line);
        });

        const int found = panel.runSearch(QStringLiteral("缓存"));
        check(found == 3, QStringLiteral("面板: 搜到 3 条（one.md 两处 + two.md 一处）"), QStringLiteral("%1 条").arg(found));
        check(panel.hitCount() == 3, QStringLiteral("面板: hitCount = 3"));
        check(panel.fileCount() == 2, QStringLiteral("面板: 涉及 2 个文件"));
        check(panel.resultFiles().size() == 2, QStringLiteral("面板: resultFiles 去掉重复"));
        check(panel.hitAt(0).line == 2 && panel.hitAt(0).matchStart == 0,
              QStringLiteral("面板: 第 1 条是 one.md 第 2 行，命中在行首"),
              QStringLiteral("行 %1").arg(panel.hitAt(0).line));
        check(panel.hitAt(1).line == 3 && panel.hitAt(1).matchStart == 5,
              QStringLiteral("面板: 第 2 条是 one.md 第 3 行，命中位置也对"),
              QStringLiteral("行 %1 / 位置 %2").arg(panel.hitAt(1).line).arg(panel.hitAt(1).matchStart));
        check(panel.statusText().contains(QStringLiteral("找到 3 条")),
              QStringLiteral("面板: 状态栏说了找到几条"), panel.statusText());

        // ---- ★ C1：命中区间真的被写到了结果条目上（delegate 就是从那儿读的）----
        // 这条断言把"算位置"和"存位置"连起来验了：取出来的区间必须**正好切出命中词**。
        // 光测 displaySpanFor 只证明算术对，证明不了面板有没有把它记到条目上。
        auto *resultTree = panel.findChild<QTreeWidget *>();
        check(resultTree != nullptr, QStringLiteral("面板: 结果列表能找到"));
        if (resultTree != nullptr && resultTree->topLevelItemCount() > 0
            && resultTree->topLevelItem(0)->childCount() > 0) {
            QTreeWidgetItem *firstRow = resultTree->topLevelItem(0)->child(0);
            const QPoint span = firstRow->data(1, SearchResultDelegate::kMatchSpanRole).toPoint();
            check(span.y() > 0, QStringLiteral("C1: 结果条目上存了命中区间"),
                  QStringLiteral("start=%1 len=%2").arg(span.x()).arg(span.y()));
            check(firstRow->text(1).mid(span.x(), span.y()) == QStringLiteral("缓存"),
                  QStringLiteral("C1: 区间切出来的正好是命中词（delegate 靠它上色）"),
                  QStringLiteral("切出「%1」").arg(firstRow->text(1).mid(span.x(), span.y())));
        } else {
            check(false, QStringLiteral("C1: 结果列表里应该有分组和命中行"));
        }

        // "点开第 N 条" → 发 resultActivated，主窗口据此打开文件并跳行
        check(panel.activateResult(1), QStringLiteral("面板: 点第 2 条 -> 成功"));
        check(activated.size() == 1 && activated.first() == QStringLiteral("one.md:3"),
              QStringLiteral("面板: 发出的是「哪个文件、哪一行」"), activated.join(QStringLiteral(", ")));
        check(!panel.activateResult(-1) && !panel.activateResult(99),
              QStringLiteral("面板: 越界的下标 -> false（不发信号）"));
        check(activated.size() == 1, QStringLiteral("面板: 越界点击不会误发跳转信号"));

        // 真的"点一下列表里的某一行"：走的是同一个槽（itemClicked → activateResult）
        // QTreeWidgetItem* 不是元类型，先注册，才能按名字调用私有槽
        qRegisterMetaType<QTreeWidgetItem *>("QTreeWidgetItem*");
        QTreeWidget tree;
        auto *group = new QTreeWidgetItem(&tree);
        auto *child = new QTreeWidgetItem(group);
        child->setData(0, Qt::UserRole, 2);
        const bool invoked = QMetaObject::invokeMethod(&panel,
                                                      "onItemClicked",
                                                      Q_ARG(QTreeWidgetItem *, child),
                                                      Q_ARG(int, 0));
        check(invoked, QStringLiteral("面板: 单击槽能被元对象系统调到"));
        check(activated.size() == 2 && activated.at(1) == QStringLiteral("two.md:1"),
              QStringLiteral("面板: 单击结果行 -> 跳转到那条结果"), activated.join(QStringLiteral(", ")));
        QMetaObject::invokeMethod(&panel, "onItemClicked", Q_ARG(QTreeWidgetItem *, group), Q_ARG(int, 0));
        check(activated.size() == 2, QStringLiteral("面板: 点文件分组行不跳转（分组不是结果）"));

        // 搜不到 / 空关键词 / 索引为空
        check(panel.runSearch(QStringLiteral("完全没有的内容")) == 0, QStringLiteral("面板: 搜不到 -> 0 条"));
        check(panel.hitCount() == 0, QStringLiteral("面板: 搜不到时结果列表被清空"));
        check(panel.statusText().contains(QStringLiteral("没有找到")),
              QStringLiteral("面板: 说清是没找到"), panel.statusText());
        check(panel.runSearch(QString()) == 0 && panel.statusText().contains(QStringLiteral("输入关键词")),
              QStringLiteral("面板: 空关键词 -> 提示先输关键词"));

        SearchPanel emptyPanel;
        emptyPanel.setIndexPath(base + QStringLiteral("/empty-index.sqlite"));
        emptyPanel.setDirectory(panelDocs);
        check(emptyPanel.runSearch(QStringLiteral("缓存")) == 0,
              QStringLiteral("面板: 还没建索引就搜 -> 0 条（不崩）"));
        check(emptyPanel.statusText().contains(QStringLiteral("索引是空的")),
              QStringLiteral("面板: 并且提示先建立索引"), emptyPanel.statusText());
        check(emptyPanel.activateResult(0) == false, QStringLiteral("面板: 没有结果时点开 -> false"));
    }

    QDir(base).removeRecursively();

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
