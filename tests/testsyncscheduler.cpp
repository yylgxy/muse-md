// EditorSyncScheduler 的契约测试（性能优化 P0-1）。
//
// 这个类存在的理由：主窗口原来在**每次按键**里做
//     files->setText(editor->toPlainText())
// 而 toPlainText() 会把整篇文档深拷贝一遍。几十万字符的文档上，这笔钱每敲一个字都要付。
// 改成"停手 150ms 之后统一同步一次"之后，必须保证两件事：
//   1. 连续打字只同步一次（那才是省下来的开销）；
//   2. 保存/关闭/切标签之前 flush 一定能把内容同步进去
//      （否则会保存到 150ms 前的旧内容 —— 那是丢数据）。
// 这个测试把这两条钉住，附带把"编辑器先被销毁了怎么办"也钉住。
//
// 跑法：ctest -C Debug --output-on-failure

#include "editorsyncscheduler.h"
#include "editorwidget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QList>
#include <QString>

#include <cstdio>

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

// 转 ms 毫秒事件循环（真等，因为要验证"定时器到点才同步"）
void spin(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 只关心"同步发生了几次、发生在哪个编辑器上" —— 把指针记下来就够了
    QList<EditorWidget *> syncLog;
    const auto handler = [&syncLog](EditorWidget *editor) { syncLog.append(editor); };

    // ============================ A. 连续打字只同步一次 ============================
    {
        std::printf("---- A. 节流（连续打字只同步一次）----\n");

        EditorSyncScheduler scheduler;
        scheduler.setSyncHandler(handler);
        scheduler.setDelay(60);  // 缩短一点，测试跑得快些

        EditorWidget editor;
        check(scheduler.delay() == 60, QStringLiteral("节流: 延迟可以设置"));
        check(scheduler.pendingCount() == 0, QStringLiteral("节流: 一开始没有欠着的"));

        // 模拟快速打字：10 次 markDirty，每次间隔 10ms（远短于 60ms 的窗口）
        for (int i = 0; i < 10; ++i) {
            scheduler.markDirty(&editor);
            spin(10);
        }
        check(syncLog.isEmpty(),
              QStringLiteral("节流: ★连着敲十次，一次都没同步（定时器一直被往后推）"),
              QStringLiteral("%1 次").arg(syncLog.size()));
        check(scheduler.isPending(&editor), QStringLiteral("节流: 它确实记着「欠着一次」"));
        check(scheduler.pendingCount() == 1,
              QStringLiteral("节流: 同一个编辑器重复标记只算一个（不会堆积）"),
              QStringLiteral("%1 个").arg(scheduler.pendingCount()));

        // 停手之后到点，同步一次
        spin(120);
        check(syncLog.size() == 1, QStringLiteral("节流: 停手之后只同步一次"),
              QStringLiteral("%1 次").arg(syncLog.size()));
        check(syncLog.value(0) == &editor, QStringLiteral("节流: 同步的正是那个编辑器"));
        check(!scheduler.isPending(&editor), QStringLiteral("节流: 同步完就不再欠着了"));

        // 再敲一次还能再同步（不是一次性的）
        scheduler.markDirty(&editor);
        spin(120);
        check(syncLog.size() == 2, QStringLiteral("节流: 再敲一次能再同步一次"),
              QStringLiteral("%1 次").arg(syncLog.size()));
    }

    // ============================ B. flush：保存/关闭前必须立刻同步 ============================
    {
        std::printf("---- B. flush（保存 / 关闭 / 切标签之前）----\n");
        syncLog.clear();

        EditorSyncScheduler scheduler;
        scheduler.setSyncHandler(handler);
        scheduler.setDelay(5000);  // 故意设得极长：不 flush 就绝不会自己发生

        EditorWidget editor;
        editor.setPlainText(QStringLiteral("刚打完的字"));
        scheduler.markDirty(&editor);
        check(syncLog.isEmpty(), QStringLiteral("flush: 时间还没到，当然还没同步"));

        scheduler.flushAll();
        check(syncLog.size() == 1,
              QStringLiteral("flush: ★flush 之后立刻同步（保存/关闭前靠它，不会存到旧内容）"),
              QStringLiteral("%1 次").arg(syncLog.size()));

        scheduler.flushAll();
        check(syncLog.size() == 1, QStringLiteral("flush: 没欠着的时候再 flush 什么都不做"));

        // 两个编辑器都欠着：一次 flush 全同步，且保持标记顺序
        EditorWidget second;
        scheduler.markDirty(&editor);
        scheduler.markDirty(&second);
        check(scheduler.pendingCount() == 2, QStringLiteral("flush: 两个编辑器各欠一次"));
        scheduler.flushAll();
        check(syncLog.size() == 3, QStringLiteral("flush: 一次 flush 把两个都同步了"),
              QStringLiteral("%1 次").arg(syncLog.size()));
        check(syncLog.value(1) == &editor && syncLog.value(2) == &second,
              QStringLiteral("flush: 按标记顺序同步（先欠的先同步）"));
    }

    // ============================ C. forget 与销毁 ============================
    {
        std::printf("---- C. 标签关闭 / 编辑器被销毁 ----\n");
        syncLog.clear();

        EditorSyncScheduler scheduler;
        scheduler.setSyncHandler(handler);
        scheduler.setDelay(60);

        // C-1：forget 之后不再同步（标签被关掉的情形）
        EditorWidget editor;
        scheduler.markDirty(&editor);
        scheduler.forget(&editor);
        check(!scheduler.isPending(&editor), QStringLiteral("forget: 摘掉之后不再欠着"));
        spin(150);
        check(syncLog.isEmpty(), QStringLiteral("forget: 到点也不会同步它（内容已经在管理器里了）"));

        // C-2：定时器还没到点，编辑器就被销毁了 —— 不能崩，也不能回调
        {
            auto *temporary = new EditorWidget();
            scheduler.markDirty(temporary);
            check(scheduler.pendingCount() == 1, QStringLiteral("销毁: 先标记一个"));
            delete temporary;
        }
        spin(150);
        check(syncLog.isEmpty(), QStringLiteral("销毁: ★编辑器先被销毁，到点也不会回调它（不崩）"));
        check(scheduler.pendingCount() == 0, QStringLiteral("销毁: 列表里不留空壳"));
    }

    // ============================ D. 零延迟模式 ============================
    {
        std::printf("---- D. 延迟设为 0 ----\n");
        syncLog.clear();

        EditorSyncScheduler scheduler;
        scheduler.setSyncHandler(handler);
        scheduler.setDelay(0);

        EditorWidget editor;
        scheduler.markDirty(&editor);
        check(syncLog.size() == 1,
              QStringLiteral("零延迟: markDirty 立刻同步（不必转事件循环）"),
              QStringLiteral("%1 次").arg(syncLog.size()));
        check(scheduler.delay() == 0, QStringLiteral("零延迟: delay() 报 0"));
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
