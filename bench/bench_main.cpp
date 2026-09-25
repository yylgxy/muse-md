// 性能基准（A2）：一个可执行程序，用子命令区分场景。
//
// 用法：
//     md_bench <scenario> [--json] [--save <file>] [--quick]
//       scenarios: large-doc | index | sync-scroll | encoding | export | history | all
//
// ---- 为什么不用第三方基准库（google/benchmark 之类）----
//   1. 这是**指标回归**，不是微基准竞赛 —— 我们关心的是"这一版比上一版慢了吗"，
//      不是纳秒级精度；
//   2. 多加一个第三方依赖，就得处理它在 MSVC 下的编译，得不偿失；
//   3. QElapsedTimer 的精度（微秒级）对这个量级完全够用。
//
// ---- 报数口径 ----
// 每个场景跑 N 次，报 **中位数 + 最差值**：中位数抗抖动（别的进程抢 CPU 时不会失真），
// 最差值暴露卡顿（用户真正会抱怨的那一次）。第一次跑当预热，不计入。
//
// ---- 这个基准为什么能直接量到算法（这是设计上的回报）----
// 项目把"要自动验证的逻辑"都抽成了不依赖界面的纯函数/静态函数：
//   * SyncBridge::buildLineMap()        —— static 纯函数
//   * FileManager::detectEncoding()     —— static 纯函数
//   * Exporter::standaloneHtml()        —— static 纯函数
//   * FullTextSearch::indexDirectory()  —— 不依赖 QWidget，能在测试里直接实例化
// 所以基准**不用起 GUI、不用起 Chromium** 就能量到算法本身的耗时。
//
// ---- 数字属于哪个构建，必须写在数字旁边（A2.5 的坑）----
// Debug 下 STL 迭代器、QString 都有大量额外检查，绝对值和 Release 差好几倍 ——
// 拿 Debug 的绝对值和 Release 的绝对值对比，得出的结论可以是完全错的。
// 所以：
//   1. 归档 JSON 里的 buildType 字段按 NDEBUG **自动推出**，不许写死
//      （曾经写死成 "Debug"，而实际是 Release 构建 —— 归档数字的元信息是假的，
//       比没有元信息更糟）；
//   2. 报告里用**相对对比**（A1 前 vs A1 后）说话，绝对只当量级参考。

#include "editorwidget.h"
#include "exporter.h"
#include "filemanager.h"
#include "fulltextsearch.h"
#include "searchpanel.h"
#include "syncbridge.h"
#include "versioncontrol.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringConverter>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QThread>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {

// 这批数字是哪个构建量出来的。按 NDEBUG 判定，不写死 —— 见文件头的 A2.5 说明。
QString buildTypeName()
{
#ifdef NDEBUG
    return QStringLiteral("Release");
#else
    return QStringLiteral("Debug");
#endif
}

using markdown_editor::core::storage::FileManager;
using markdown_editor::core::storage::VersionControl;
using markdown_editor::core::document::SyncBridge;

// ============================================================================
// 计时与统计
// ============================================================================

struct Sample
{
    QString name;
    QString unit = QStringLiteral("ms");
    double median = 0.0;  // 中位数
    double worst = 0.0;   // 最差值
    int runs = 0;
    QString note;
    // "这一行不是多次测量的统计，就是一个静态量"。
    // 磁盘占用这类指标没有"中位数"可言，硬塞进两列会印出 "412.00 KB / 0.00 KB" 这种
    // 看起来像 bug 的东西。所以给它一个显式标记，最差值那列印「—」——
    // 表的形状不变，但读的人一眼知道这一列对这一行没有意义。
    bool single = false;
};

// 跑 runs 次，返回"中位数 + 最差值"。第一次调用当预热，不计入 ——
// 第一次通常包含页缓存冷启动和 Qt 的一次性初始化，不是稳态数字。
template <typename Fn>
Sample measure(const QString &name, int runs, Fn &&fn, const QString &unit = QStringLiteral("ms"))
{
    QList<double> values;
    fn();  // 预热
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer timer;
        timer.start();
        fn();
        values << double(timer.nsecsElapsed()) / 1'000'000.0;
    }
    std::sort(values.begin(), values.end());

    Sample s;
    s.name = name;
    s.unit = unit;
    s.runs = runs;
    s.median = values.isEmpty() ? 0.0 : values.at(values.size() / 2);
    s.worst = values.isEmpty() ? 0.0 : values.last();
    return s;
}

// ============================================================================
// 「主线程阻塞」探针（B2 的核心指标）
// ============================================================================
//
// 怎么量"索引期间 GUI 线程最坏一次事件循环阻塞"：
//   * 起一个后台心跳线程，每 5ms 往主线程 post 一个**带投递时间戳**的事件；
//   * 主线程不断 processEvents()，事件被处理时算"投递 → 送达"的间隔；
//   * 取整个过程中的最大值。
//
// ★ 为什么事件必须自带投递时间戳：如果只记"两次心跳之间的间隔"，主线程阻塞 5 秒之后
//   积压的心跳会被连续处理完，间隔看起来全是 0 —— 那就完全量不到阻塞了。
//
// ★ 为什么这个指标对 A1 前后都成立：
//   * A1 之前：buildIndex() 在主线程同步跑完全部索引，期间一个心跳都送不进来，
//     最坏延迟 ≈ 整个索引耗时（这就是用户眼里的"卡死"）；
//   * A1 之后：任务投给工作线程，主线程空闲地跑事件循环，最坏延迟是毫秒级。

class HeartbeatEvent : public QEvent
{
public:
    explicit HeartbeatEvent(qint64 postedNs) : QEvent(QEvent::User), m_postedNs(postedNs) {}
    qint64 postedNs() const { return m_postedNs; }

private:
    qint64 m_postedNs;
};

class LatencyProbe
{
public:
    void start()
    {
        m_postedNs.store(0);
        m_deliveredNs.store(0);
        m_worstNs.store(0);
        m_sentCount.store(0);
        m_stop.store(false);
        m_clock.start();

        m_thread = QThread::create([this] {
            while (!m_stop.load(std::memory_order_relaxed)) {
                // 上一条还没送达就先不投：避免堆爆事件队列，也避免把"队列深度"量成延迟
                if (m_postedNs.load() == m_deliveredNs.load()) {
                    const qint64 now = m_clock.nsecsElapsed();
                    m_postedNs.store(now);
                    m_sentCount.fetch_add(1);
                    QCoreApplication::postEvent(m_sink, new HeartbeatEvent(now));
                }
                QThread::msleep(5);
            }
        });
        m_thread->start();
    }

    void observe(const QEvent *event)
    {
        const auto *beat = dynamic_cast<const HeartbeatEvent *>(event);
        if (beat == nullptr) {
            return;
        }
        const qint64 latency = m_clock.nsecsElapsed() - beat->postedNs();
        m_deliveredNs.store(beat->postedNs());
        qint64 previous = m_worstNs.load();
        while (latency > previous && !m_worstNs.compare_exchange_weak(previous, latency)) {
        }
    }

    void stop()
    {
        m_stop.store(true);
        if (m_thread != nullptr) {
            m_thread->wait();
            delete m_thread;
            m_thread = nullptr;
        }
    }

    void setSink(QObject *sink) { m_sink = sink; }
    double worstMs() const { return double(m_worstNs.load()) / 1'000'000.0; }
    int sentCount() const { return m_sentCount.load(); }

private:
    QElapsedTimer m_clock;
    QThread *m_thread = nullptr;
    QObject *m_sink = nullptr;
    std::atomic<qint64> m_postedNs{0};
    std::atomic<qint64> m_deliveredNs{0};
    std::atomic<qint64> m_worstNs{0};
    std::atomic<int> m_sentCount{0};
    std::atomic_bool m_stop{false};
};

// 心跳事件的落点：住在主线程，收到就把延迟喂给探针。
// 不声明 Q_OBJECT —— 只重写虚函数、不加信号槽，不需要 moc。
class HeartbeatSink : public QObject
{
public:
    explicit HeartbeatSink(LatencyProbe *probe) : m_probe(probe) {}

    bool event(QEvent *event) override
    {
        if (event != nullptr && event->type() == QEvent::User) {
            m_probe->observe(event);
        }
        return QObject::event(event);
    }

private:
    LatencyProbe *m_probe = nullptr;
};

// 主线程空转 durationMs：只有 processEvents 一直在跑，心跳事件才有机会被送达。
void pumpEventsFor(int durationMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < durationMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

// ============================================================================
// 样本生成
// ============================================================================

// 一段像真笔记的 Markdown（标题 / 列表 / 代码块 / 正文），不是"全是同一行" ——
// 否则哈希、分词都会命中缓存，把数字量得过于乐观。
QString makeMarkdownParagraph(int index)
{
    static const char *kFillers[] = {
        "这一节记录实现细节与取舍，重点是**为什么**这么做，而不只是做了什么。",
        "缓存命中率、索引重建耗时、滚动同步的复杂度，都要有可复现的数字支撑。",
        "分层的好处是依赖方向明确：business 不认识 ui，ui 也不反过来依赖 core 的实现。",
        "如果一个判断只能靠肉眼观察，那它迟早会退化，所以关键路径都要有契约测试。",
        "QPlainTextEdit 的 blockNumber() 是 0 起算，而本项目的行号统一 1 起算。",
    };
    QStringList block;
    block << QStringLiteral("## 小节 %1").arg(index);
    block << QString();
    block << QStringLiteral("- %1").arg(QString::fromUtf8(kFillers[index % 5]));
    block << QStringLiteral("- %1").arg(QString::fromUtf8(kFillers[(index + 1) % 5]));
    block << QString();
    block << QStringLiteral("```cpp");
    block << QStringLiteral("int value%1 = compute(%1);").arg(index);
    block << QStringLiteral("```");
    block << QString();
    block << QString::fromUtf8(kFillers[(index + 2) % 5]);
    return block.join(QLatin1Char('\n'));
}

QString makeLargeMarkdown(int targetChars)
{
    QString text;
    text.reserve(targetChars + 4096);
    int i = 0;
    while (text.size() < targetChars) {
        text += makeMarkdownParagraph(++i);
        text += QLatin1Char('\n');
    }
    return text;
}

QString makeMarkdownWithLines(int lines)
{
    QStringList out;
    out.reserve(lines);
    for (int i = 0; i < lines; ++i) {
        if (i % 10 == 0) {
            out << QStringLiteral("# 标题 %1").arg(i / 10);
        } else if (i % 3 == 0) {
            out << QStringLiteral("- 列表项 %1 的内容").arg(i);
        } else {
            out << QStringLiteral("第 %1 行：一段普通正文，用来撑出行数。").arg(i);
        }
    }
    return out.join(QLatin1Char('\n'));
}

// B2 的样本：N 个文件 × M 行。落到临时目录并复用 ——
// 造 5000 个文件本身要几秒，那是"造样本"的代价，不该混进"索引"的数字里。
QString prepareIndexFixture(int fileCount, int linesPerFile, QString *note)
{
    const QString dir = QDir::tempPath()
                        + QStringLiteral("/muse-md-bench-index-%1x%2").arg(fileCount).arg(linesPerFile);
    QDir().mkpath(dir);

    const QFileInfoList existing = QDir(dir).entryInfoList({QStringLiteral("*.md")}, QDir::Files);
    if (existing.size() == fileCount) {
        if (note != nullptr) {
            *note = QStringLiteral("复用已有样本：%1").arg(QDir::toNativeSeparators(dir));
        }
        return dir;
    }

    for (const QFileInfo &info : existing) {
        QFile::remove(info.absoluteFilePath());  // 上次被中断，样本不完整
    }

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < fileCount; ++i) {
        // 每个文件内容都不同：否则某些优化会被"内容重复"喂出虚高的数字
        QStringList lines;
        lines.reserve(linesPerFile);
        lines << QStringLiteral("# 文档 %1").arg(i);
        for (int n = 1; n < linesPerFile; ++n) {
            lines << QStringLiteral("第 %1 行：这是第 %2 号文件的一段内容，关键词 %3。")
                         .arg(n)
                         .arg(i)
                         .arg(n % 7 == 0 ? QStringLiteral("缓存") : QStringLiteral("索引"));
        }
        QFile file(QStringLiteral("%1/note-%2.md").arg(dir).arg(i, 5, 10, QLatin1Char('0')));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::fprintf(stderr, "无法写入样本文件：%s\n", qPrintable(file.fileName()));
            return QString();
        }
        file.write(lines.join(QLatin1Char('\n')).toUtf8());
        file.close();
    }

    if (note != nullptr) {
        *note = QStringLiteral("%1 个文件（造样本耗时 %2 ms）").arg(fileCount).arg(timer.elapsed());
    }
    return dir;
}

struct EncodingFixture
{
    QString label;
    QByteArray bytes;
};

QList<EncodingFixture> makeEncodingFixtures(int targetChars)
{
    QString text;
    text.reserve(targetChars);
    while (text.size() < targetChars) {
        text += QStringLiteral("中文字符测试 ABC def 123 编码检测要在这一串上保持正确。\n");
    }
    text = text.trimmed();

    QList<EncodingFixture> out;
    out << EncodingFixture{QStringLiteral("UTF-8"), text.toUtf8()};
    out << EncodingFixture{QStringLiteral("UTF-8-BOM"), QByteArray("\xEF\xBB\xBF") + text.toUtf8()};
    {
        QByteArray gbk = QStringEncoder(QStringConverter::Encoding::System)(text);
        out << EncodingFixture{QStringLiteral("GBK(系统编码)"), gbk};
    }
    {
        QByteArray utf16 = QStringEncoder(QStringConverter::Encoding::Utf16LE)(text);
        out << EncodingFixture{QStringLiteral("UTF-16LE(带BOM)"), QByteArray("\xFF\xFE") + utf16};
    }
    return out;
}

// ============================================================================
// 场景
// ============================================================================

// B1 大文档编辑：量 kFastModeThresholdChars = 300000 这个阈值本身的依据。
QList<Sample> scenarioLargeDoc(bool quick)
{
    const int runs = quick ? 2 : 5;
    const int threshold = EditorWidget::kFastModeThresholdChars;

    QList<Sample> samples;
    QString text;
    samples << measure(
        QStringLiteral("B1 造 %1 字符文档").arg(threshold), runs, [&text, threshold] { text = makeLargeMarkdown(threshold); });
    samples.last().note = QStringLiteral("实际 %1 字符").arg(text.size());

    EditorWidget editor;
    samples << measure(QStringLiteral("B1 setPlainText(%1 字符)").arg(threshold), runs,
                       [&editor, &text] { editor.setPlainText(text); });
    samples.last().note = QStringLiteral("编辑器读到 %1 字符，快速模式=%2")
                              .arg(editor.characterCount())
                              .arg(EditorWidget::prefersFastMode(editor.characterCount()) ? QStringLiteral("开")
                                                                                          : QStringLiteral("关"));

    // 阈值上下各判一次，证明"300000"不是拍的：刚好在阈值上就进快速模式
    samples << measure(
        QStringLiteral("B1 阈值判定 prefersFastMode()"), quick ? 20 : 100,
        [threshold] {
            Q_UNUSED(EditorWidget::prefersFastMode(threshold - 1));
            Q_UNUSED(EditorWidget::prefersFastMode(threshold));
        });
    samples.last().runs = quick ? 20 : 100;
    samples.last().note = QStringLiteral("阈值 %1：%2→%3，%4→%5")
                              .arg(threshold)
                              .arg(threshold - 1)
                              .arg(EditorWidget::prefersFastMode(threshold - 1) ? QStringLiteral("快速") : QStringLiteral("常规"))
                              .arg(threshold)
                              .arg(EditorWidget::prefersFastMode(threshold) ? QStringLiteral("快速") : QStringLiteral("常规"));

    // 打字延迟的直接代理指标：在 30 万字符的文档末尾连续插入字符，量合计耗时。
    // 不开窗口 —— 真实帧延迟要用 FrameProbe 在 GUI 会话里量（见 README 的说明）。
    const int keyCount = quick ? 50 : 200;
    editor.setPlainText(text);
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    samples << measure(
        QStringLiteral("B1 大文档连续插入 %1 字符（合计）").arg(keyCount), runs,
        [&cursor, keyCount] {
            for (int i = 0; i < keyCount; ++i) {
                cursor.insertText(QStringLiteral("x"));
            }
        });
    samples.last().note = QStringLiteral("%1 构建；单次约 %2 ms")
                              .arg(buildTypeName())
                              .arg(samples.last().median / keyCount, 0, 'f', 4);
    return samples;
}

// B2 索引：A1 的核心指标。
QList<Sample> scenarioIndex(bool quick, LatencyProbe *probe)
{
    const int runs = quick ? 1 : 3;
    const int files = quick ? 800 : 5000;
    const int linesPerFile = 200;

    QList<Sample> samples;
    QString fixtureNote;
    const QString dir = prepareIndexFixture(files, linesPerFile, &fixtureNote);
    if (dir.isEmpty()) {
        std::fprintf(stderr, "索引样本准备失败\n");
        return samples;
    }

    const QString indexDb = QDir::tempPath() + QStringLiteral("/muse-md-bench-index.sqlite");
    const auto clearDb = [](const QString &path) {
        QFile::remove(path);
        QFile::remove(path + QStringLiteral("-wal"));  // 开了 WAL 之后会多这两个文件
        QFile::remove(path + QStringLiteral("-shm"));
    };

    // ---- B2a 首次索引（冷索引：每次先把库删干净）----
    samples << measure(
        QStringLiteral("B2 首次索引 %1 文件×%2 行").arg(files).arg(linesPerFile), runs,
        [&dir, &indexDb, &clearDb] {
            clearDb(indexDb);
            FullTextSearch engine;
            engine.setIndexPath(indexDb);
            QString error;
            if (!engine.open(&error)) {
                std::fprintf(stderr, "索引库打开失败：%s\n", qPrintable(error));
                return;
            }
            engine.indexDirectory(dir, &error);
            engine.close();
        });
    samples.last().note = fixtureNote;

    // ---- B2b 二次索引（全部命中"没变过"，应该很快）----
    samples << measure(
        QStringLiteral("B2 二次索引（全跳过）"), quick ? 2 : 5,
        [&dir, &indexDb] {
            FullTextSearch engine;
            engine.setIndexPath(indexDb);
            QString error;
            if (!engine.open(&error)) {
                return;
            }
            engine.indexDirectory(dir, &error);
            engine.close();
        });

    // ---- B2c 界面级：点「建立索引」期间主线程被占用的时长 ★★ A1 的核心指标 ----
    //
    // 为什么**先看这一个**、而不是先看探针的"事件投递延迟"：
    //   旧实现里 buildIndex() 是同步跑的，途中靠 onIndexProgress 里的
    //   `processEvents(QEventLoop::ExcludeUserInputEvents)` 让界面重绘。
    //   那个 flags 的字面意思就是"**不处理用户输入**" —— 所以：
    //     * 用"心跳事件（postEvent 的队列事件）"量出来的延迟很小（几十毫秒），
    //       因为队列事件确实被那次 processEvents 投递了；
    //     * 但用户此刻**点不动、打不了字**，一直到 buildIndex() 返回为止。
    //   也就是说：探针在旧实现上会**系统性低估**卡顿。
    //   "buildIndex() 占住主线程多久"才是用户真正感知的那段（点下去到界面能动）。
    //
    // A1 之后这两个指标同时成立：buildIndex() 立刻返回（<1 ms），
    // 索引跑在工作线程里，探针量到的也就是真实的事件循环延迟了。
    if (probe != nullptr) {
        const QString panelDb = QDir::tempPath() + QStringLiteral("/muse-md-bench-panel.sqlite");
        clearDb(panelDb);

        SearchPanel panel;
        panel.setIndexPath(panelDb);
        panel.setDirectory(dir);

        probe->start();
        QElapsedTimer clock;
        clock.start();
        panel.buildIndex();  // ★ 要量的就是这一句
        const qint64 callMs = clock.elapsed();
        // 固定泵 3 秒，让两边可比：
        //   * 旧实现：buildIndex() 里已经泵了整个索引过程（15 秒），这里再多泵 3 秒；
        //   * A1 之后：buildIndex() 立刻返回，这 3 秒覆盖的正是"索引在工作线程里跑、
        //     同时主线程要处理事件"的那段最拥挤的时刻 —— 这才是界面流畅度的真实工况。
        // 两边窗口长度不同（这一点在结果文档里写明了），但都在量同一件事：
        // "事件积压到送达的最坏值"。
        pumpEventsFor(3000);
        probe->stop();

        // ★ 主指标：用户从点下按钮到界面能重新响应输入的那段时间
        Sample call;
        call.name = QStringLiteral("B2 界面级：buildIndex() 占住主线程的时长");
        call.median = static_cast<double>(callMs);
        call.worst = static_cast<double>(callMs);
        call.runs = 1;
        call.note = QStringLiteral("这一句返回之前，主线程不会处理任何用户输入"
                                   "（旧实现里 processEvents 用的是 ExcludeUserInputEvents）");
        samples << call;

        // 次要指标：事件循环的投递延迟。旧实现下会低估（理由见上面那段注释），
        // A1 之后它才是可信的"界面流畅度"数字。
        Sample idle;
        idle.name = QStringLiteral("B2 界面级：建立索引期间事件投递最坏延迟");
        idle.median = probe->worstMs();
        idle.worst = probe->worstMs();
        idle.runs = probe->sentCount();
        idle.note = QStringLiteral("投出 %1 次心跳；旧实现会低估这一项（见代码注释）")
                        .arg(probe->sentCount());
        samples << idle;
    }

    return samples;
}

// B3 同步滚动：行号映射的复杂度。
QList<Sample> scenarioSyncScroll(bool quick)
{
    const int runs = quick ? 2 : 5;
    const int lines = quick ? 10000 : 50000;

    QList<Sample> samples;
    QString text;
    samples << measure(QStringLiteral("B3 造 %1 行文档").arg(lines), runs,
                       [&text, lines] { text = makeMarkdownWithLines(lines); });
    samples.last().note = QStringLiteral("实际 %1 字符").arg(text.size());

    QList<int> lineMap;
    samples << measure(QStringLiteral("B3 SyncBridge::buildLineMap(%1 行)").arg(lines), runs,
                       [&text, &lineMap] { lineMap = SyncBridge::buildLineMap(text); });
    samples.last().note = QStringLiteral("映射出 %1 个顶层块").arg(lineMap.size());
    return samples;
}

// B4 编码检测：证明"自写严格 UTF-8 校验"的代价可接受。
QList<Sample> scenarioEncoding(bool quick)
{
    const int runs = quick ? 2 : 5;
    const QList<EncodingFixture> fixtures = makeEncodingFixtures(1024 * 1024 / 3);

    QList<Sample> samples;
    for (const EncodingFixture &fixture : fixtures) {
        const QString label = QStringLiteral("%1, %2 KB").arg(fixture.label).arg(fixture.bytes.size() / 1024);

        Sample detect = measure(
            QStringLiteral("B4 detectEncoding(%1)").arg(label), runs,
            [&fixture] { Q_UNUSED(FileManager::detectEncoding(fixture.bytes)); });
        detect.note = QStringLiteral("判定为 %1")
                          .arg(FileManager::encodingName(FileManager::detectEncoding(fixture.bytes)));
        samples << detect;

        samples << measure(
            QStringLiteral("B4 decode(%1)").arg(fixture.label), runs, [&fixture] {
                Q_UNUSED(FileManager::decode(fixture.bytes, FileManager::detectEncoding(fixture.bytes)));
            });
        samples.last().note = label;
    }
    return samples;
}

// B5 导出：HTML 导出（静态纯函数，不起 Chromium）。
// PDF 是**故意不测**的：它要起整个 Chromium 进程，bench 不该为此背上 WebEngine 运行时依赖。
QList<Sample> scenarioExport(bool quick)
{
    const int runs = quick ? 2 : 5;

    QList<Sample> samples;
    QString text;
    samples << measure(QStringLiteral("B5 造 1MB 文档"), runs,
                       [&text] { text = makeLargeMarkdown(1024 * 1024); });
    samples.last().note = QStringLiteral("%1 字符").arg(text.size());

    samples << measure(QStringLiteral("B5 standaloneHtml(1MB文档)"), runs,
                       [&text] { Q_UNUSED(Exporter::standaloneHtml(text, QStringLiteral("bench"))); });

    QTemporaryDir temp;
    const QString target = temp.path() + QStringLiteral("/bench-export.html");
    samples << measure(QStringLiteral("B5 exportHtml 落盘(1MB文档)"), runs, [&text, &target] {
        Exporter exporter;
        QString error;
        exporter.exportHtml(text, QDir::tempPath(), target, &error);
    });
    return samples;
}

// ============================================================================
// C6 版本历史仓库的膨胀率（"先量后改"）
// ============================================================================
//
// 为什么这个场景存在：路线图 C6 给了两条路 ——
//   (a) 写 pruneHistory() 真的删掉旧提交；
//   (b) 只做 git gc、提交不删，靠 git 自己的增量存储。
// 选哪条**取决于膨胀率有多大**，而"快照文件只有一个、每版差异很小，所以膨胀远小于预期"
// 只是一句直觉。直觉要先变成数字，否则要么白写一个会删数据的危险功能，
// 要么在一个真会膨胀的场景里偷懒。
//
// 所以这里只测三件事：
//   1. 连续 N 次保存之后，.git 一共占多少磁盘、平均每版多少；
//   2. git gc 能收回多少（这是路线 (b) 的关键数字）；
//   3. 顺手验一下 gc 之后**每个版本还读得出来** —— 省了磁盘但读不出内容，
//      那不叫优化，那叫丢数据（路线图 C6 Step 2 的要求）。

// 递归算目录里所有文件的字节数。要 Hidden + System：.git 里的对象文件在 Windows 上
// 带隐藏属性，漏掉它们会把这个数字量成一个笑话。
qint64 directorySize(const QString &path)
{
    qint64 total = 0;
    const QDir::Filters filters = QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    QDirIterator it(path, filters, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

// 模拟一次"保存"对文档做了什么。两种都是真实存在的写法，膨胀率差别很大：
//   appendMode = true  —— 末尾追加一段（写笔记的常态：文档只会越来越长）
//   appendMode = false —— 改掉中间那一行（反复打磨一个地方：文件长度基本不变）
QString mutateDocument(const QString &previous, int step, bool appendMode)
{
    if (appendMode) {
        return previous
               + QStringLiteral("\n## 追加小节 %1\n\n这是第 %1 次保存新增的一段正文，"
                                "用来模拟「笔记越写越长」这种最常见的用法。\n")
                     .arg(step);
    }

    // makeMarkdownWithLines() 里 i == 22 那一行是普通正文（i%10 和 i%3 都不命中），
    // 所以这串字在整个文档里只出现一次 —— 可以安全地用它做"被改的那一行"。
    const QString from = QStringLiteral("第 22 行：一段普通正文，用来撑出行数。");
    const QString to = QStringLiteral("第 22 行：这一行被第 %1 次保存改过了。").arg(step);

    QString text = previous;
    const int at = text.indexOf(from);
    if (at < 0) {
        text += QLatin1Char('\n') + to;
        return text;
    }
    text.replace(at, from.size(), to);
    return text;
}

QList<Sample> scenarioHistory(bool quick)
{
    const int commitTarget = quick ? 60 : 300;
    QList<Sample> samples;

    if (!VersionControl::isGitAvailable()) {
        // 不是错误：这台机器没有 git 时，产品里的"本地历史"也是自动降级的（只记一条日志）。
        std::fprintf(stderr, "C6 跳过：这台机器上没有找到 git\n");
        return samples;
    }

    const struct
    {
        const char *label;
        bool appendMode;
    } patterns[] = {
        {"追加型（文档越来越长）", true},
        {"改写型（长度基本不变）", false},
    };

    for (const auto &pattern : patterns) {
        const QString label = QString::fromUtf8(pattern.label);

        QTemporaryDir temp;
        if (!temp.isValid()) {
            std::fprintf(stderr, "C6 跳过：建不出临时目录\n");
            return samples;
        }

        VersionControl version;
        version.setHistoryRoot(temp.path());

        const QString documentPath = temp.path() + QStringLiteral("/note.md");
        const QString repoDir = version.repositoryPathFor(documentPath);
        QString error;
        if (!version.initRepository(repoDir, &error)) {
            std::fprintf(stderr, "C6 跳过：git init 失败（%s）\n", qPrintable(error));
            return samples;
        }

        // 一本"一屏能看完"的笔记：400 行带标题/列表的 markdown，约 9 KB
        QString content = makeMarkdownWithLines(400);
        const qint64 firstVersionBytes = qint64(content.toUtf8().size());

        QList<double> commitMs;
        commitMs.reserve(commitTarget);
        int committed = 0;
        bool aborted = false;
        for (int step = 1; step <= commitTarget; ++step) {
            content = mutateDocument(content, step, pattern.appendMode);

            QElapsedTimer timer;
            timer.start();
            const QString hash = version.commitSnapshot(repoDir, content, QString(), &error);
            commitMs << double(timer.nsecsElapsed()) / 1'000'000.0;

            if (hash.isEmpty()) {
                std::fprintf(stderr, "C6 第 %d 次提交没有产生版本（%s），中止这个样本\n", step,
                             qPrintable(error));
                aborted = true;
                break;
            }
            ++committed;
        }
        if (aborted || committed == 0) {
            continue;
        }

        const qint64 lastVersionBytes = qint64(content.toUtf8().size());
        const qint64 gitBytes = directorySize(repoDir + QStringLiteral("/.git"));
        const qint64 fullCopyBytes = (firstVersionBytes + lastVersionBytes) / 2 * committed;

        // ---- 单次提交的耗时：这才是用户能感觉到的那部分（Ctrl+S 之后等多久）----
        std::sort(commitMs.begin(), commitMs.end());
        Sample timing;
        timing.name = QStringLiteral("C6 commitSnapshot %1").arg(label);
        timing.runs = int(commitMs.size());
        timing.median = commitMs.at(commitMs.size() / 2);
        timing.worst = commitMs.last();
        timing.note = QStringLiteral("%1 次提交；每次都会起一个 git 进程").arg(commitMs.size());
        samples << timing;

        // ---- 磁盘占用 ----
        Sample gitSize;
        gitSize.name = QStringLiteral("C6 %1 次保存后的 .git").arg(committed);
        gitSize.unit = QStringLiteral("KB");
        gitSize.single = true;
        gitSize.runs = committed;
        gitSize.median = double(gitBytes) / 1024.0;
        gitSize.note = QStringLiteral("%1；每版平均 %2 KB；若每版存一份完整副本要 %3 KB")
                           .arg(label)
                           .arg(double(gitBytes) / 1024.0 / committed, 0, 'f', 2)
                           .arg(double(fullCopyBytes) / 1024.0, 0, 'f', 0);
        samples << gitSize;

        Sample fileSize;
        fileSize.name = QStringLiteral("C6 文档本身的大小（同一时刻）");
        fileSize.unit = QStringLiteral("KB");
        fileSize.single = true;
        fileSize.median = double(lastVersionBytes) / 1024.0;
        fileSize.note = QStringLiteral("第 1 版 %1 KB → 第 %2 版 %3 KB")
                            .arg(double(firstVersionBytes) / 1024.0, 0, 'f', 1)
                            .arg(committed)
                            .arg(double(lastVersionBytes) / 1024.0, 0, 'f', 1);
        samples << fileSize;

        // ---- gc：路线 (b) 到底能收回多少 ----
        // 超时给足：gc 是这一整个场景里最慢的一步（它要重新打包所有对象），
        // 而且它本来就只在"攒够一批提交"之后才跑一次，不是每次保存都跑。
        QProcess gc;
        gc.setProgram(VersionControl::gitExecutable());
        gc.setArguments(QStringList{QStringLiteral("-C"), repoDir, QStringLiteral("gc"),
                                    QStringLiteral("--prune=now"), QStringLiteral("--quiet")});
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
        env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        gc.setProcessEnvironment(env);
        QElapsedTimer gcTimer;
        gcTimer.start();
        gc.start();
        const bool gcOk = gc.waitForStarted(20000) && gc.waitForFinished(180000);
        const double gcMs = double(gcTimer.nsecsElapsed()) / 1'000'000.0;
        const qint64 afterGcBytes = directorySize(repoDir + QStringLiteral("/.git"));

        // gc 之后剩下的版本还读得出来吗？读不出来就是拿数据换磁盘，不能算优化。
        const QList<VersionControl::Commit> remaining = version.history(repoDir, committed + 10, &error);
        int readable = 0;
        for (const VersionControl::Commit &c : remaining) {
            QString readError;
            Q_UNUSED(version.contentOf(repoDir, c.hash, &readError));
            if (readError.isEmpty()) {
                ++readable;
            }
        }

        Sample gcCost;
        gcCost.name = QStringLiteral("C6 git gc 耗时");
        gcCost.runs = 1;
        gcCost.median = gcMs;
        gcCost.worst = gcMs;
        gcCost.note = QStringLiteral("对 %1 个提交做一次；所以它只能「攒够一批再跑」，不能每次保存都跑")
                          .arg(committed);
        samples << gcCost;

        Sample gcSize;
        gcSize.name = QStringLiteral("C6 gc 之后的 .git");
        gcSize.unit = QStringLiteral("KB");
        gcSize.single = true;
        gcSize.median = double(afterGcBytes) / 1024.0;
        gcSize.note = gcOk ? QStringLiteral("收回 %1%；%2 个版本里 %3 个仍可读（提交一个都没删）")
                                 .arg(double(gitBytes - afterGcBytes) / double(gitBytes) * 100.0, 0, 'f', 1)
                                 .arg(remaining.size())
                                 .arg(readable)
                           : QStringLiteral("git gc 没能跑完，这一行不可信");
        samples << gcSize;

        if (readable != int(remaining.size())) {
            // %d 要 int：QList::size() 是 qsizetype（64 位），直接塞进去是未定义行为
            std::fprintf(stderr, "★ C6 警告：gc 之后有 %d/%d 个版本读不出来了\n",
                         int(remaining.size()) - readable, int(remaining.size()));
        }
    }

    return samples;
}

// ============================================================================
// 输出
// ============================================================================

// 中文按字符对齐（printf 的 %-52s 是按字节补齐的，中文会歪）
QString padRight(const QString &text, int width)
{
    QString out = text;
    while (out.size() < width) {
        out += QLatin1Char(' ');
    }
    return out;
}

void printSamples(const QList<Sample> &samples)
{
    const QString header = padRight(QStringLiteral("指标"), 56) + padRight(QStringLiteral("中位数"), 16)
                           + padRight(QStringLiteral("最差值"), 16)                                  + QStringLiteral("说明");
    std::fputs(qPrintable(header), stdout);
    std::fputs("\n", stdout);
    std::fputs(qPrintable(QString(112, QLatin1Char('-'))), stdout);
    std::fputs("\n", stdout);

    for (const Sample &s : samples) {
        const QString median = QStringLiteral("%1 %2").arg(s.median, 0, 'f', 2).arg(s.unit);
        const QString worst =
            s.single ? QStringLiteral("—") : QStringLiteral("%1 %2").arg(s.worst, 0, 'f', 2).arg(s.unit);
        const QString line = padRight(s.name, 56) + padRight(median, 16) + padRight(worst, 16) + s.note;
        std::fputs(qPrintable(line), stdout);
        std::fputs("\n", stdout);
    }
}

QJsonArray samplesToJson(const QList<Sample> &samples)
{
    QJsonArray array;
    for (const Sample &s : samples) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("unit"), s.unit);
        o.insert(QStringLiteral("median"), s.median);
        o.insert(QStringLiteral("worst"), s.worst);
        o.insert(QStringLiteral("runs"), s.runs);
        o.insert(QStringLiteral("note"), s.note);
        array.append(o);
    }
    return array;
}

void printUsage()
{
    std::fprintf(stderr,
                 "用法：md_bench <scenario> [--json] [--save <file>] [--quick]\n"
                 "  scenarios: large-doc | index | sync-scroll | encoding | export | history | all\n");
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);  // 必须是 QApplication：SearchPanel / EditorWidget 都是 QWidget

    QStringList args;
    for (int i = 1; i < argc; ++i) {
        args << QString::fromLocal8Bit(argv[i]);
    }

    bool json = false;
    bool quick = false;
    QString savePath;
    QString scenario;

    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QStringLiteral("--json")) {
            json = true;
        } else if (arg == QStringLiteral("--quick")) {
            quick = true;
        } else if (arg == QStringLiteral("--save") && i + 1 < args.size()) {
            savePath = args.at(++i);
        } else if (arg.startsWith(QStringLiteral("--"))) {
            std::fprintf(stderr, "未知参数：%s\n", qPrintable(arg));
            printUsage();
            return 2;
        } else if (scenario.isEmpty()) {
            scenario = arg;
        }
    }

    if (scenario.isEmpty()) {
        printUsage();
        return 2;
    }

    LatencyProbe probe;
    HeartbeatSink sink(&probe);
    probe.setSink(&sink);

    QList<Sample> samples;
    QJsonObject meta;
    meta.insert(QStringLiteral("generatedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    meta.insert(QStringLiteral("buildType"), buildTypeName());
    meta.insert(QStringLiteral("qt"), QString::fromLatin1(QT_VERSION_STR));
    meta.insert(QStringLiteral("quickMode"), quick);

    const auto runOne = [&](const QString &name) -> bool {
        if (name == QStringLiteral("large-doc")) {
            samples += scenarioLargeDoc(quick);
        } else if (name == QStringLiteral("index")) {
            samples += scenarioIndex(quick, &probe);
        } else if (name == QStringLiteral("sync-scroll")) {
            samples += scenarioSyncScroll(quick);
        } else if (name == QStringLiteral("encoding")) {
            samples += scenarioEncoding(quick);
        } else if (name == QStringLiteral("export")) {
            samples += scenarioExport(quick);
        } else if (name == QStringLiteral("history")) {
            samples += scenarioHistory(quick);
        } else {
            return false;
        }
        return true;
    };

    if (scenario == QStringLiteral("all")) {
        // 顺序有讲究：先跑不含索引的（便宜），最后跑索引（最贵、会落下临时库）
        const QStringList order{QStringLiteral("sync-scroll"),
                                QStringLiteral("encoding"),
                                QStringLiteral("export"),
                                QStringLiteral("history"),
                                QStringLiteral("large-doc"),
                                QStringLiteral("index")};
        for (const QString &name : order) {
            if (!runOne(name)) {
                std::fprintf(stderr, "未知场景：%s\n", qPrintable(name));
                return 2;
            }
        }
    } else if (!runOne(scenario)) {
        std::fprintf(stderr, "未知场景：%s\n", qPrintable(scenario));
        printUsage();
        return 2;
    }

    if (!json) {
        printSamples(samples);
    }

    if (json || !savePath.isEmpty()) {
        QJsonObject root = meta;
        root.insert(QStringLiteral("scenario"), scenario);
        root.insert(QStringLiteral("samples"), samplesToJson(samples));
        const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (json) {
            std::fwrite(payload.constData(), 1, size_t(payload.size()), stdout);
        }
        if (!savePath.isEmpty()) {
            QFile file(savePath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                file.write(payload);
                std::fprintf(stderr, "已写入 %s\n", qPrintable(savePath));
            } else {
                std::fprintf(stderr, "写不进去：%s\n", qPrintable(savePath));
                return 1;
            }
        }
    }

    return 0;
}
