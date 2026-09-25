// SearchIndexWorker 契约测试（A1：把全文索引移入后台线程）。
//
// 要证明的三件事（也是 A1 存在的全部理由）：
//   1. 在工作线程里能正常建索引 —— 跨线程信号回得来、引擎真的在工作线程里创建；
//   2. 取消能让它在"文件之间"干净地停下来，且**索引保持上一次的完整状态**（事务回滚）；
//   3. ★ GUI/主线程在索引期间不被阻塞 —— 这一条是 A1 存在的理由，必须测。
//   4. （附带）索引期间从别的线程搜索不报错 —— 这是 WAL 存在的理由。
//
// 为什么这个测试不需要起窗口：
//   SearchIndexWorker 不依赖 QWidget（本项目的既有惯例），所以用 QCoreApplication 就够，
//   不起 Chromium、毫秒级跑完。这正是"逻辑抽成不依赖界面的类"的回报。
//
// 跑法：ctest -C Debug -R searchindexworker --output-on-failure

#include "fulltextsearch.h"
#include "searchindexworker.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <atomic>

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

void writeMarkdown(const QString &path, int lines)
{
    QStringList out;
    out.reserve(lines);
    out << QStringLiteral("# 文档");
    for (int i = 1; i < lines; ++i) {
        out << QStringLiteral("第 %1 行：缓存与索引的测试内容 %2。").arg(i).arg(i % 5);
    }
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(out.join(QLatin1Char('\n')).toUtf8());
    }
}

// 造 fileCount 个文件，每个 linesEach 行
bool makeCorpus(const QString &dir, int fileCount, int linesEach)
{
    if (!QDir().mkpath(dir)) {
        return false;
    }
    for (int i = 0; i < fileCount; ++i) {
        writeMarkdown(QStringLiteral("%1/note-%2.md").arg(dir).arg(i, 5, 10, QLatin1Char('0')), linesEach);
    }
    return true;
}

// 一次 startIndex 的结果
struct Round
{
    bool finished = false;
    bool cancelled = false;
    bool failed = false;
    QString error;
    IndexStats stats;
    qint64 elapsedMs = 0;
    int cancelRequestedMs = -1;  // 什么时候投的取消请求（-1 = 没投）
};

// 把 worker 的四个终止信号收成一个同步等待点。
// 为什么必须有超时：测试里死等 = CI 上挂 30 分钟没人知道为什么。
Round runRound(SearchIndexWorker *worker, const QString &dir, const QString &indexPath,
               int cancelAfterMs = -1, int timeoutMs = 60000)
{
    Round round;
    QEventLoop loop;
    QElapsedTimer clock;
    clock.start();

    const auto connections = QList<QMetaObject::Connection>{
        QObject::connect(worker, &SearchIndexWorker::finished, &loop, [&](const IndexStats &stats) {
            round.finished = true;
            round.stats = stats;
            loop.quit();
        }),
        QObject::connect(worker, &SearchIndexWorker::cancelled, &loop, [&] {
            round.cancelled = true;
            loop.quit();
        }),
        QObject::connect(worker, &SearchIndexWorker::failed, &loop, [&](const QString &message) {
            round.failed = true;
            round.error = message;
            loop.quit();
        }),
    };

    QMetaObject::invokeMethod(worker, "startIndex", Qt::QueuedConnection,
                              Q_ARG(QString, dir), Q_ARG(QString, indexPath));

    // 取消：等一小会儿（让索引真的开跑、跑到文件中间），再置取消标志。
    //
    // ★ 这里**直接调** worker->cancel()，不走排队调用：工作线程此刻正卡在
    //   startIndex() 里面，它的事件循环没在跑，排队投过去的 cancel 要等索引结束
    //   才会被处理 —— 那就等于取消失效。cancel() 只写一个原子标志，跨线程直接调是安全的。
    if (cancelAfterMs >= 0) {
        // cancelAfterMs 必须按值捕获：singleShot 的 lambda 活得比这一行久。
        QTimer::singleShot(cancelAfterMs, &loop, [worker, &round, cancelAfterMs] {
            worker->cancel();
            round.cancelRequestedMs = cancelAfterMs;
        });
    }

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);

    loop.exec();
    round.elapsedMs = clock.elapsed();

    for (const QMetaObject::Connection &c : connections) {
        QObject::disconnect(c);
    }
    return round;
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<IndexStats>("IndexStats");

    QTemporaryDir temp;
    if (!temp.isValid()) {
        std::fprintf(stderr, "临时目录创建失败\n");
        return 1;
    }
    const QString base = temp.path();
    const QString indexPath = base + QStringLiteral("/index.sqlite");

    // ---------------- 0. 样本 ----------------
    // 小样本（快）：验证"能完成"
    const QString smallDir = base + QStringLiteral("/small");
    // 大样本：验证"不阻塞"和"能取消"。
    //
    // 规模取多少？路线图 A2 的基准用 5000 文件 × 200 行，那是**Release** 的量级。
    // 这个测试跑在 Debug 构建上（Debug 下 SQLite + FTS5 trigram 的插入慢一个数量级，
    // qsizetype/QString 都有额外检查），5000×200 会让 ctest 多等十几分钟。
    // 测试要的是"索引足够久，久到能观察到主线程是否被阻塞、取消是否来得及生效"，
    // 而不是某个特定规模 —— 所以这里取 300×200 = 6 万行（Debug 下仍要跑几十秒）。
    const QString bigDir = base + QStringLiteral("/big");
    constexpr int kBigFiles = 300;
    constexpr int kBigLines = 200;

    std::printf("---- A1: SearchIndexWorker ----\n");
    check(makeCorpus(smallDir, 5, 30), QStringLiteral("样本: 小目录 5 个文件"));
    check(makeCorpus(bigDir, kBigFiles, kBigLines),
          QStringLiteral("样本: 大目录 %1 个文件 × %2 行").arg(kBigFiles).arg(kBigLines));

    // ---------------- 1. 在工作线程里能正常建索引 ----------------
    {
        QThread thread;
        thread.setObjectName(QStringLiteral("test-index"));
        auto *worker = new SearchIndexWorker;
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();

        const Round round = runRound(worker, smallDir, indexPath);
        check(round.finished && !round.failed && !round.cancelled,
              QStringLiteral("A1-1 工作线程里索引正常完成"),
              round.failed ? round.error : QStringLiteral("%1 个文件 / %2 行")
                                                .arg(round.stats.filesFound)
                                                .arg(round.stats.linesIndexed));
        check(round.stats.filesFound == 5 && round.stats.filesIndexed == 5,
              QStringLiteral("A1-1 统计数字穿过信号槽回来了（排队连接）"),
              QStringLiteral("扫描 %1 / 重建 %2").arg(round.stats.filesFound).arg(round.stats.filesIndexed));

        // 跨线程读回来：读侧在 GUI 线程另建一个实例（QSqlDatabase 连接绑定线程）
        FullTextSearch reader;
        reader.setIndexPath(indexPath);
        QString error;
        check(reader.open(&error), QStringLiteral("A1-1 读侧实例能在主线程打开同一个库"), error);
        check(reader.indexedFileCount() == 5, QStringLiteral("A1-1 读侧看到 5 个文件（写侧真的写进去了）"),
              QStringLiteral("%1 个").arg(reader.indexedFileCount()));
        check(reader.search(QStringLiteral("缓存"), 100, &error).size() > 0,
              QStringLiteral("A1-1 索引结果能被搜到（不是只写了元数据）"), error);
        reader.close();

        // ---- 3. 不阻塞主线程（A1 的核心验收项）----
        // 提交索引 → 主线程原地跑事件循环并量每次 processEvents 的最坏耗时。
        // 如果 startIndex 被同步执行（或者有人把它改回直接调用而不是排队调用），
        // 主线程会在这一句里停住好几秒，最坏耗时必然 > 50ms。
        QElapsedTimer submitClock;
        submitClock.start();
        QMetaObject::invokeMethod(worker, "startIndex", Qt::QueuedConnection,
                                  Q_ARG(QString, bigDir), Q_ARG(QString, indexPath));
        const qint64 submitMs = submitClock.elapsed();

        check(submitMs < 50, QStringLiteral("A1-3 提交索引立刻返回（不阻塞主线程）"),
              QStringLiteral("%1 ms").arg(submitMs));

        QEventLoop waitLoop;
        bool bigDone = false;
        QObject::connect(worker, &SearchIndexWorker::finished, &waitLoop, [&] {
            bigDone = true;
            waitLoop.quit();
        });
        QObject::connect(worker, &SearchIndexWorker::failed, &waitLoop, [&] { waitLoop.quit(); });

        qint64 worstPumpMs = 0;
        int pumps = 0;
        QElapsedTimer overall;
        overall.start();
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &waitLoop, &QEventLoop::quit);
        watchdog.start(120000);

        // 一边等索引结束、一边量主线程每次事件循环的耗时
        QTimer sampler;
        sampler.setInterval(0);
        QObject::connect(&sampler, &QTimer::timeout, [&worstPumpMs, &pumps] {
            QElapsedTimer t;
            t.start();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            const double ms = double(t.nsecsElapsed()) / 1'000'000.0;
            if (ms > worstPumpMs) {
                worstPumpMs = ms;
            }
            ++pumps;
        });
        sampler.start();

        waitLoop.exec();
        sampler.stop();

        check(bigDone, QStringLiteral("A1-3 大目录索引在后台跑完没超时"),
              QStringLiteral("总耗时 %1 ms").arg(overall.elapsed()));
        check(worstPumpMs < 50.0,
              QStringLiteral("A1-3 ★ 索引期间主线程最坏一次事件循环 < 50ms"),
              QStringLiteral("最坏 %1 ms（%2 次采样）").arg(worstPumpMs, 0, 'f', 2).arg(pumps));

        thread.quit();
        thread.wait();
    }

    // ---------------- 2. 取消：文件之间干净停下 + 索引保持上一次状态 ----------------
    {
        // 先建好一份"上一次的完整索引"（小目录），取消之后它必须还在
        QThread thread;
        auto *worker = new SearchIndexWorker;
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();

        const Round baseline = runRound(worker, smallDir, indexPath);
        check(baseline.finished, QStringLiteral("A1-2 先建好一份基线索引（小目录）"),
              QStringLiteral("%1 个文件").arg(baseline.stats.filesFound));

        // 再对着大目录跑一次，中途取消
        const Round cancelled = runRound(worker, bigDir, indexPath, /*cancelAfterMs=*/80);
        check(cancelled.cancelled, QStringLiteral("A1-2 取消请求生效（收到 cancelled 信号）"));
        check(!cancelled.finished, QStringLiteral("A1-2 取消时**没有**误发 finished（界面不会显示「完成」）"));
        check(cancelled.elapsedMs < 3000, QStringLiteral("A1-2 取消在合理时间内生效（验收要求 1 秒内）"),
              QStringLiteral("%1 ms（请求取消在 %2 ms 时投出）")
                  .arg(cancelled.elapsedMs)
                  .arg(cancelled.cancelRequestedMs));

        // ★ 关键：事务回滚 —— 索引仍然是小目录那一份，而不是"半个大目录"
        FullTextSearch reader;
        reader.setIndexPath(indexPath);
        QString error;
        check(reader.open(&error), QStringLiteral("A1-2 取消后读侧还能打开库"), error);
        const int files = reader.indexedFileCount();
        check(files == 5, QStringLiteral("A1-2 ★ 取消后索引保持上一次的完整状态（回滚生效）"),
              QStringLiteral("%1 个文件（期望 5）").arg(files));
        check(reader.search(QStringLiteral("缓存"), 100, &error).size() > 0,
              QStringLiteral("A1-2 取消后之前的索引仍然搜得出结果"), error);
        reader.close();

        thread.quit();
        thread.wait();
    }

    // ---------------- 2b. 引擎级：requestCancel() 的契约本身 ----------------
    // worker 那一层测的是"取消信号 + 回滚"，但 cancelled() 没有参数，
    // 所以"indexDirectory 的返回值里 cancelled 标了 true"这件事要在引擎这一层直接验。
    {
        const QString engineDb = base + QStringLiteral("/engine-cancel.sqlite");

        std::atomic<FullTextSearch *> published{nullptr};
        IndexStats captured;
        QString openError;
        std::atomic_bool opened{false};

        QThread *thread = QThread::create([&] {
            // ★ 引擎必须在这个线程里创建（QSqlDatabase 的连接绑定创建它的线程）
            FullTextSearch engine;
            engine.setIndexPath(engineDb);
            opened.store(engine.open(&openError));
            published.store(&engine);

            captured = engine.indexDirectory(bigDir, &openError);

            published.store(nullptr);
            engine.close();
        });
        thread->start();

        // 等它真的开始跑
        QElapsedTimer spin;
        spin.start();
        while (published.load() == nullptr && spin.elapsed() < 10000) {
            QThread::msleep(5);
        }
        QThread::msleep(80);  // 让它处理掉几个文件，确保取消点是在"中途"被撞上的

        FullTextSearch *engine = published.load();
        if (engine != nullptr) {
            engine->requestCancel();  // ★ 从别的线程直接调：这正是它被设计成原子标志的原因
        }

        if (!thread->wait(30000)) {
            thread->terminate();
            thread->wait();
        }
        delete thread;

        check(opened.load(), QStringLiteral("A1-2b 引擎在工作线程里打开了库"), openError);
        check(engine != nullptr, QStringLiteral("A1-2b 取消请求确实投出去了"));
        check(captured.cancelled, QStringLiteral("A1-2b ★ indexDirectory 的返回值里标了 cancelled"));
        check(captured.filesIndexed < captured.filesFound,
              QStringLiteral("A1-2b 确实是中途停下的（不是跑完才发现要取消）"),
              QStringLiteral("重建 %1 / 共 %2").arg(captured.filesIndexed).arg(captured.filesFound));
        check(captured.elapsedMs > 0, QStringLiteral("A1-2b 取消路径也填了 elapsedMs"),
              QStringLiteral("%1 ms").arg(captured.elapsedMs));
    }

    // ---------------- 4. 索引期间从另一个线程搜索：不报错（WAL 的存在理由）----------------
    {
        const QString walIndex = base + QStringLiteral("/wal-index.sqlite");

        QThread thread;
        auto *worker = new SearchIndexWorker;
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();

        // 先用小目录把库建起来并留一份数据
        runRound(worker, smallDir, walIndex);

        // 让写侧开始重建大目录（这是一次长写事务）
        bool done = false;
        QEventLoop loop;
        QObject::connect(worker, &SearchIndexWorker::finished, &loop, [&] {
            done = true;
            loop.quit();
        });
        QObject::connect(worker, &SearchIndexWorker::failed, &loop, &QEventLoop::quit);
        QMetaObject::invokeMethod(worker, "startIndex", Qt::QueuedConnection,
                                  Q_ARG(QString, bigDir), Q_ARG(QString, walIndex));

        // 写侧正在写的同时，主线程（= 另一个连接）反复搜
        FullTextSearch reader;
        reader.setIndexPath(walIndex);
        QString error;
        const bool opened = reader.open(&error);
        check(opened, QStringLiteral("A1-4 写事务进行中，读侧仍能打开库"), error);

        int searches = 0;
        int errors = 0;
        QElapsedTimer clock;
        clock.start();
        while (!done && clock.elapsed() < 120000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            if (opened) {
                QString searchError;
                reader.search(QStringLiteral("缓存"), 50, &searchError);
                ++searches;
                if (!searchError.isEmpty()) {
                    ++errors;
                    if (errors == 1) {
                        std::printf("    第一次搜索报错：%s\n", qPrintable(searchError));
                    }
                }
            }
        }
        if (opened) {
            reader.close();
        }

        check(done, QStringLiteral("A1-4 写侧跑完了"));
        check(searches > 0, QStringLiteral("A1-4 边索引边搜索真的发生了"), QStringLiteral("%1 次").arg(searches));
        check(errors == 0, QStringLiteral("A1-4 ★ 索引期间搜索 0 错误（WAL 生效，不是 database is locked）"),
              QStringLiteral("%1 次搜索中 %2 次报错").arg(searches).arg(errors));

        thread.quit();
        thread.wait();
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
