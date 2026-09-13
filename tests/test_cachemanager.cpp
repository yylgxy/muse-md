// CacheManager（4.2.3 缓存服务）的契约测试。
//
// 重点不是"能存能取"，而是三件容易写错、错了又不容易发现的事：
//   1. **淘汰顺序真的是 LRU**（按访问刷新），不是按插入顺序 —— 用一个"访问旧记录再插新记录"的
//      场景验出来：如果被淘汰的是刚访问过的那条，说明实现的其实是 FIFO。
//      （这不是吹毛求疵：Qt 的 QCache 文档写得含糊，我先用探针实测过它确实是 LRU。）
//   2. **过期判断**：磁盘上的文件被别的程序改过时，必须返回 Stale 让调用方重新读盘。
//      少了这条，缓存就会让用户拿旧内容覆盖别人的新改动。
//   3. **两条容量约束**：条数上限（LRU 淘汰）+ 单条大小上限（大文件直接不进缓存）。
//
// 测试文件写在系统临时目录下的 md-editor-cachemanager-test/，不污染源码树。
//
// 跑法：ctest -C Debug --output-on-failure

#include "cachemanager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>

using markdown_editor::core::storage::CacheManager;
using markdown_editor::core::storage::Encoding;

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

// 按磁盘上文件的当前状态造一条缓存记录（正常流程里这就是 FileManager 干的事）
CacheManager::Entry entryFor(const QString &path, const QString &text)
{
    const QFileInfo info(path);
    CacheManager::Entry entry;
    entry.text = text;
    entry.encoding = Encoding::Utf8;
    entry.size = info.size();
    entry.lastModified = info.lastModified();
    return entry;
}

void writeText(const QString &path, const QString &text)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    file.write(text.toUtf8());
    file.close();
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString work = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-cachemanager-test"));
    QDir(work).removeRecursively();
    if (!QDir().mkpath(work)) {
        std::printf("cannot create temp dir: %s\n", work.toUtf8().constData());
        return 2;
    }

    const QString pathA = work + QStringLiteral("/a.md");
    const QString pathB = work + QStringLiteral("/b.md");
    const QString pathC = work + QStringLiteral("/c.md");
    writeText(pathA, QStringLiteral("甲文件内容\n"));
    writeText(pathB, QStringLiteral("乙文件内容\n"));
    writeText(pathC, QStringLiteral("丙文件内容\n"));

    // ============================ 基本存取 ============================
    {
        CacheManager cache;
        check(cache.size() == 0, QStringLiteral("新建: 缓存为空"));
        check(cache.maxEntries() == CacheManager::kDefaultMaxEntries,
              QStringLiteral("新建: 默认条数上限"),
              QString::number(cache.maxEntries()));

        check(cache.insert(pathA, entryFor(pathA, QStringLiteral("甲文件内容\n"))),
              QStringLiteral("insert: 存入成功"));
        check(cache.size() == 1 && cache.contains(pathA), QStringLiteral("insert: 条数与 contains 都对"));
        check(cache.keys().size() == 1, QStringLiteral("keys: 列出缓存里的路径"));

        CacheManager::Entry got;
        check(cache.lookup(pathA, QFileInfo(pathA), &got) == CacheManager::LookupResult::Hit,
              QStringLiteral("lookup: 文件没变 → Hit"));
        check(got.text == QStringLiteral("甲文件内容\n") && got.encoding == Encoding::Utf8,
              QStringLiteral("lookup: 内容和编码都取回来了"));
        check(cache.hits() == 1 && cache.misses() == 0, QStringLiteral("统计: 命中 1 次"));

        got.text.clear();
        check(cache.lookup(pathB, QFileInfo(pathB), &got) == CacheManager::LookupResult::Miss,
              QStringLiteral("lookup: 没缓存过的路径 → Miss"));
        check(cache.misses() == 1, QStringLiteral("统计: 未命中 1 次"));
    }

    // ============================ LRU 淘汰（核心）============================
    {
        CacheManager cache(2);  // 最多两条

        check(cache.insert(pathA, entryFor(pathA, QStringLiteral("A"))), QStringLiteral("LRU: 存入 A"));
        check(cache.insert(pathB, entryFor(pathB, QStringLiteral("B"))), QStringLiteral("LRU: 存入 B"));

        // 访问 A —— 这一步把 A 变成"最近使用"。
        // 如果实现是 FIFO（按插入顺序淘汰），下面插入 C 时会先淘汰 A。
        CacheManager::Entry got;
        cache.lookup(pathA, QFileInfo(pathA), &got);

        check(cache.insert(pathC, entryFor(pathC, QStringLiteral("C"))), QStringLiteral("LRU: 再存入 C（超出上限）"));
        check(cache.size() == 2, QStringLiteral("LRU: 条数仍然等于上限"));
        check(cache.contains(pathA), QStringLiteral("LRU: 刚访问过的 A 还在（★证明是 LRU 不是 FIFO）"));
        check(!cache.contains(pathB), QStringLiteral("LRU: 最久未使用的 B 被淘汰"));
        check(cache.evictions() >= 1, QStringLiteral("统计: 记了一次淘汰"));
    }

    // ============================ 过期判断 ============================
    {
        CacheManager cache;
        const QString path = work + QStringLiteral("/stale.md");
        writeText(path, QStringLiteral("第一版内容\n"));
        cache.insert(path, entryFor(path, QStringLiteral("第一版内容\n")));

        CacheManager::Entry got;
        check(cache.lookup(path, QFileInfo(path), &got) == CacheManager::LookupResult::Hit,
              QStringLiteral("过期: 文件没动 → Hit"));

        // 别的程序改了文件（大小也变了）
        writeText(path, QStringLiteral("第二版内容，被别的程序改长了\n"));
        got.text = QStringLiteral("调用方原有的值");
        check(cache.lookup(path, QFileInfo(path), &got) == CacheManager::LookupResult::Stale,
              QStringLiteral("过期: 文件被改过 → Stale（必须重新读盘）"));
        check(got.text == QStringLiteral("调用方原有的值"),
              QStringLiteral("过期: 返回 Stale 时不动调用方传进来的 entry"));
        check(cache.staleCount() == 1, QStringLiteral("统计: 记了一次过期"));
        check(cache.size() == 1, QStringLiteral("过期: 记录先留着，等调用方重新读盘覆盖它"));

        // 覆盖成新的之后又能命中
        cache.insert(path, entryFor(path, QStringLiteral("第二版内容，被别的程序改长了\n")));
        check(cache.lookup(path, QFileInfo(path), &got) == CacheManager::LookupResult::Hit,
              QStringLiteral("过期: 覆盖成新内容后恢复命中"));
        check(got.text == QStringLiteral("第二版内容，被别的程序改长了\n"),
              QStringLiteral("过期: 命中时拿到的是新内容"));
    }

    // ============================ 两条容量约束 ============================
    {
        CacheManager cache(0);  // 关掉缓存
        check(!cache.insert(pathA, entryFor(pathA, QStringLiteral("x"))),
              QStringLiteral("上限 0: 插入被拒（等于关掉缓存）"));
        check(cache.size() == 0 && cache.rejections() == 1, QStringLiteral("上限 0: 条数保持 0 并记一次拒绝"));

        cache.setMaxEntries(5);
        check(cache.insert(pathA, entryFor(pathA, QStringLiteral("x"))), QStringLiteral("上限 0: 调大之后能存了"));

        // 单条大小上限（用伪造的 size 来测，不用真的造大文件）
        cache.setMaxEntryBytes(10);
        CacheManager::Entry big = entryFor(pathA, QStringLiteral("x"));
        big.size = 100;
        check(!cache.insert(pathB, big), QStringLiteral("单条上限: 超过上限的大文件被拒"));
        check(!cache.contains(pathB), QStringLiteral("单条上限: 被拒的没进缓存"));
        check(cache.rejections() >= 2, QStringLiteral("统计: 拒绝次数累加"));

        big.size = 5;
        check(cache.insert(pathB, big), QStringLiteral("单条上限: 没超上限的正常进缓存"));

        cache.setMaxEntryBytes(0);
        big.size = 1024 * 1024 * 100;  // 100MB
        check(cache.insert(pathC, big), QStringLiteral("单条上限: 设为 0 表示不限制单条大小"));
    }

    // ============================ 删除 / 清空 / 覆盖 ============================
    {
        CacheManager cache;
        cache.insert(pathA, entryFor(pathA, QStringLiteral("旧")));
        cache.insert(pathA, entryFor(pathA, QStringLiteral("新内容")));
        check(cache.size() == 1, QStringLiteral("覆盖: 同一路径再存不会变成两条"));

        CacheManager::Entry got;
        cache.lookup(pathA, QFileInfo(pathA), &got);
        check(got.text == QStringLiteral("新内容"), QStringLiteral("覆盖: 取到的是最新那份"));

        cache.remove(pathA);
        check(!cache.contains(pathA) && cache.size() == 0, QStringLiteral("remove: 删掉之后查不到"));

        cache.insert(pathA, entryFor(pathA, QStringLiteral("x")));
        cache.insert(pathB, entryFor(pathB, QStringLiteral("y")));
        cache.clear();
        check(cache.size() == 0, QStringLiteral("clear: 一次清空"));
    }

    // ============================ key 规范化 ============================
    {
        check(CacheManager::normalizeKey(QStringLiteral("D:\\notes\\A.md"))
                  == CacheManager::normalizeKey(QStringLiteral("d:/notes/a.md")),
              QStringLiteral("normalizeKey: 大小写与正反斜杠差异归一到同一把 key"));
        check(CacheManager::normalizeKey(QStringLiteral("D:/notes/a.md"))
                  != CacheManager::normalizeKey(QStringLiteral("D:/notes/b.md")),
              QStringLiteral("normalizeKey: 不同文件不同 key"));

        // 通过 API 验证：用反斜杠写法存，用正斜杠写法也能查到（同一个文件必须命中同一份缓存）
        CacheManager cache;
        cache.insert(QStringLiteral("D:\\notes\\A.md"), entryFor(pathA, QStringLiteral("内容")));
        CacheManager::Entry got;
        check(cache.lookup(QStringLiteral("d:/notes/a.md"), QFileInfo(pathA), &got)
                  == CacheManager::LookupResult::Hit,
              QStringLiteral("API: 两种写法命中同一份缓存"));
    }

    // ============================ 拷贝语义与统计文本 ============================
    {
        CacheManager cache;
        cache.insert(pathA, entryFor(pathA, QStringLiteral("原始内容")));

        CacheManager::Entry got;
        cache.lookup(pathA, QFileInfo(pathA), &got);
        got.text = QStringLiteral("我改了调用方这份拷贝");

        CacheManager::Entry again;
        cache.lookup(pathA, QFileInfo(pathA), &again);
        check(again.text == QStringLiteral("原始内容"),
              QStringLiteral("拷贝语义: 调用方改自己的副本，动不到缓存里的数据"));

        const QString stats = cache.statisticsText();
        check(stats.contains(QStringLiteral("命中")) && stats.contains(QStringLiteral("条")),
              QStringLiteral("statisticsText: 是可读的一行统计"), stats);

        cache.resetStatistics();
        check(cache.hits() == 0 && cache.misses() == 0 && cache.staleCount() == 0,
              QStringLiteral("统计归零: 计数清空"));
        check(cache.size() == 1, QStringLiteral("统计归零: 缓存内容不受影响"));
    }

    // ============================ 耗时参考（只打印，不断言）============================
    // 4.2.3 的验收是"第二次从缓存读更快"。这里把数字量出来，但**不做断言** ——
    // 时间会随机器负载、磁盘缓存、杀毒软件抖动，拿它当断言就成了不稳定的测试。
    // 分两个量级各测一遍：小文件省下的是"打开+读+解码"的调用开销，大文件才体现真正的读盘成本。
    {
        struct Bench
        {
            const char *label;
            QString path;
            int iterations;
        };

        const QString smallPath = work + QStringLiteral("/bench-small.md");
        const QString bigPath = work + QStringLiteral("/bench-big.md");
        writeText(smallPath, QStringLiteral("# 小文件\n\n正文\n"));
        writeText(bigPath, QString(2 * 1024 * 1024, QLatin1Char('A')));  // 约 2 MB

        const Bench benches[] = {
            {"小文件(约 20 字节)", smallPath, 2000},
            {"大文件(约 2 MB)", bigPath, 200},
        };

        for (const Bench &bench : benches) {
            QFile source(bench.path);
            source.open(QIODevice::ReadOnly);
            const QString content = QString::fromUtf8(source.readAll());
            source.close();

            CacheManager cache;
            cache.insert(bench.path, entryFor(bench.path, content));

            QElapsedTimer timer;

            // ① 每次真的读盘 + 解码
            timer.start();
            for (int i = 0; i < bench.iterations; ++i) {
                QFile file(bench.path);
                file.open(QIODevice::ReadOnly);
                const QString text = QString::fromUtf8(file.readAll());
                if (text.isEmpty()) {
                    break;  // 防止编译器把整个循环优化掉
                }
            }
            const double diskUs = double(timer.nsecsElapsed()) / 1000.0 / bench.iterations;

            // ② 每次走缓存（含把内容拷贝出来）
            CacheManager::Entry got;
            timer.start();
            for (int i = 0; i < bench.iterations; ++i) {
                cache.lookup(bench.path, QFileInfo(bench.path), &got);
            }
            const double cacheUs = double(timer.nsecsElapsed()) / 1000.0 / bench.iterations;

            std::printf("%-58s %s\n",
                        "耗时参考（不是断言）",
                        QStringLiteral("%1：读盘 %2 µs/次 → 缓存 %3 µs/次（快 %4 倍）")
                            .arg(QString::fromUtf8(bench.label))
                            .arg(diskUs, 0, 'f', 1)
                            .arg(cacheUs, 0, 'f', 1)
                            .arg(diskUs / (cacheUs > 0 ? cacheUs : 1), 0, 'f', 1)
                            .toUtf8()
                            .constData());
        }
    }

    QDir(work).removeRecursively();

    if (g_fail == 0) {
        std::printf("\n=== CacheManager 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== CacheManager 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
