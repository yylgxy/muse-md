// DraftRecovery（A3 崩溃恢复）的契约测试。
//
// 全是纯逻辑，**不用起 GUI、不拉 Chromium** —— 这正是"把可靠性逻辑抽成不依赖界面的类"
// 的回报（本项目的既有惯例）。
//
// 覆盖路线图 A3 Step 4 列的 7 条，其中第 5、6 条是重点：
// **崩溃恢复模块自己的容错才是它存在的意义** —— 一个自己会因为坏数据而崩的恢复模块
// 比没有恢复模块更糟（用户丢掉的是"以为已经被救回来的东西"）。
//
// 跑法：ctest -C Debug -R draftrecovery --output-on-failure

#include "draftrecovery.h"
#include "fileutils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>

using markdown_editor::core::storage::DraftRecovery;
// FileUtils 在全局命名空间（项目里唯一没有命名空间的类，属于历史遗留）

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

DraftRecovery::Draft makeDraft(const QString &path, const QString &name, int line, int column)
{
    DraftRecovery::Draft draft;
    draft.documentPath = path;
    draft.displayName = name;
    draft.savedAt = QDateTime::currentDateTime();
    draft.cursorLine = line;
    draft.cursorColumn = column;
    return draft;
}

QString readAll(const QString &path)
{
    QString text;
    QString error;
    if (!FileUtils::readFile(path, text, &error)) {
        return QString();
    }
    return text;
}

}  // namespace

int main(int argc, char *argv[])
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    QTemporaryDir temp;
    if (!temp.isValid()) {
        std::fprintf(stderr, "临时目录创建失败\n");
        return 1;
    }
    const QString root = temp.path() + QStringLiteral("/drafts");
    const QString manifestPath = root + QStringLiteral("/manifest.json");

    const QString docA = temp.path() + QStringLiteral("/笔记 A.md");
    const QString docB = temp.path() + QStringLiteral("/笔记 B.md");

    DraftRecovery recovery(root);
    std::printf("---- A3: DraftRecovery ----\n");
    check(recovery.rootDir() == root, QStringLiteral("A3 根目录用的是传进来的那个（测试不碰真实 AppData）"));

    // ---------------- 1. store → pending 能读回来，内容/光标/时间都对 ----------------
    {
        DraftRecovery::Draft draft = makeDraft(docA, QStringLiteral("笔记 A.md"), 7, 3);
        QString error;
        check(recovery.store(draft, QStringLiteral("第一行\n第二行\n第三行"), &error),
              QStringLiteral("A3-1 store 成功"), error);
        check(QFile::exists(manifestPath), QStringLiteral("A3-1 manifest.json 已经落盘"));

        const QList<DraftRecovery::Draft> list = recovery.pending();
        check(list.size() == 1, QStringLiteral("A3-1 pending 里正好 1 条"),
              QStringLiteral("%1 条").arg(list.size()));
        if (list.size() == 1) {
            const DraftRecovery::Draft &back = list.first();
            check(back.documentPath == docA, QStringLiteral("A3-1 路径读回来一致"));
            check(back.cursorLine == 7 && back.cursorColumn == 3,
                  QStringLiteral("A3-1 光标位置读回来一致"),
                  QStringLiteral("行 %1 列 %2").arg(back.cursorLine).arg(back.cursorColumn));
            check(back.savedAt.isValid(), QStringLiteral("A3-1 时间戳有效"));
            check(QFile::exists(back.contentPath), QStringLiteral("A3-1 正文文件存在"));
            check(readAll(back.contentPath) == QStringLiteral("第一行\n第二行\n第三行"),
                  QStringLiteral("A3-1 ★ 正文内容一字不差"));
        }
    }

    // ---------------- 2. 同一文档写两次：只有一份草稿 ----------------
    {
        QString error;
        recovery.store(makeDraft(docA, QStringLiteral("笔记 A.md"), 20, 1), QStringLiteral("改过之后的内容"), &error);
        const QList<DraftRecovery::Draft> list = recovery.pending();
        check(list.size() == 1, QStringLiteral("A3-2 同一文档写两次仍然只有 1 条草稿"),
              QStringLiteral("%1 条").arg(list.size()));
        check(!list.isEmpty() && list.first().cursorLine == 20,
              QStringLiteral("A3-2 存的是后一次的内容（覆盖，不是追加）"));
        check(!list.isEmpty() && readAll(list.first().contentPath) == QStringLiteral("改过之后的内容"),
              QStringLiteral("A3-2 正文也被后一次覆盖"));
    }

    // ---------------- 3. 两个不同文档：各自一份 ----------------
    {
        QString error;
        recovery.store(makeDraft(docB, QStringLiteral("笔记 B.md"), 1, 1), QStringLiteral("B 的内容"), &error);
        check(recovery.pending().size() == 2, QStringLiteral("A3-3 两个文档 → 两份草稿"));
    }

    // ---------------- 4. discard：删掉之后 pending 里没有了 ----------------
    {
        QString error;
        check(recovery.discard(docB, &error), QStringLiteral("A3-4 discard 成功"), error);
        check(recovery.pending().size() == 1, QStringLiteral("A3-4 discard 之后 pending 少了一条"));
        check(recovery.discard(docB, &error),
              QStringLiteral("A3-4 再 discard 一次仍然成功（幂等，不报错）"));
        check(!QDir(root).entryInfoList({QStringLiteral("*.draft")}, QDir::Files).isEmpty(),
              QStringLiteral("A3-4 A 的草稿没被误删"));
    }

    // ---------------- 5. ★ 容错：manifest 是坏 JSON ----------------
    {
        // 直接写坏它（模拟"manifest 写一半崩了"）
        QString error;
        const bool written = FileUtils::writeFile(manifestPath, QStringLiteral("{ 这不是合法 JSON"), &error);
        check(written, QStringLiteral("A3-5 测试自己把 manifest 写坏"), error);

        const QList<DraftRecovery::Draft> list = recovery.pending();
        check(list.isEmpty(), QStringLiteral("A3-5 ★ manifest 坏掉时 pending 返回空列表且不崩"),
              QStringLiteral("%1 条").arg(list.size()));

        // 还能继续写新的（坏 manifest 不该让模块彻底失灵）
        check(recovery.store(makeDraft(docA, QStringLiteral("笔记 A.md"), 5, 1), QStringLiteral("坏 manifest 之后重新写"), &error),
              QStringLiteral("A3-5 ★ manifest 坏了之后还能正常存新草稿"), error);
        check(recovery.pending().size() == 1,
              QStringLiteral("A3-5 新的 manifest 把之前的内容顶掉了（坏数据没有被强行解析）"));
    }

    // ---------------- 6. ★ 容错：正文文件被手工删掉 ----------------
    {
        QString error;
        // 先造两份，然后手工删掉其中一份的正文
        recovery.discardAll(&error);
        recovery.store(makeDraft(docA, QStringLiteral("笔记 A.md"), 1, 1), QStringLiteral("A"), &error);
        recovery.store(makeDraft(docB, QStringLiteral("笔记 B.md"), 1, 1), QStringLiteral("B"), &error);
        check(recovery.pending().size() == 2, QStringLiteral("A3-6 准备了 2 份草稿"));

        // 找到 docB 的正文并删掉它（模拟用户/清理工具手删）
        QString victim;
        for (const DraftRecovery::Draft &d : recovery.pending()) {
            if (d.documentPath == docB) {
                victim = d.contentPath;
            }
        }
        check(!victim.isEmpty(), QStringLiteral("A3-6 找到了要删的正文文件"));
        check(QFile::remove(victim), QStringLiteral("A3-6 手工删掉它"));

        const QList<DraftRecovery::Draft> list = recovery.pending();
        check(list.size() == 1, QStringLiteral("A3-6 ★ 正文不见的那条不返回"),
              QStringLiteral("%1 条").arg(list.size()));
        check(!list.isEmpty() && list.first().documentPath == docA,
              QStringLiteral("A3-6 ★ 留下的是正文还在的那一条"));

        // 而且顺手把 manifest 里的残项清掉了（在 manifest 里不该再出现 docB）
        check(!readAll(manifestPath).contains(QStringLiteral("笔记 B")),
              QStringLiteral("A3-6 ★ manifest 里的残项被顺手清掉了"));
    }

    // ---------------- 7. 路径含中文 / 空格 / 很长都能正常哈希与写读 ----------------
    {
        QString error;
        recovery.discardAll(&error);

        // 路径里的 120 个「长」字用码点构造（QChar 0x957F）。
        // ★ 不能写 QLatin1Char('长')：它只装得下单个 Latin-1 字节，会截成半个 UTF-8 字节，
        //   这条"超长中文路径"就变成了在测 120 个 'é' —— 断言还是绿的，但覆盖的东西没了。
        const QString weird = temp.path()
                              + QStringLiteral("/带 空格 的中文目录/很长的名字")
                                + QString(120, QChar(0x957F))
                              + QStringLiteral(".md");
        recovery.store(makeDraft(weird, QStringLiteral("怪名字"), 3, 4), QStringLiteral("怪路径的内容"), &error);

        const QList<DraftRecovery::Draft> list = recovery.pending();
        check(list.size() == 1, QStringLiteral("A3-7 中文/空格/超长路径能写进去也能读回来"),
              QStringLiteral("%1 条").arg(list.size()));
        check(!list.isEmpty() && QFile::exists(list.first().contentPath),
              QStringLiteral("A3-7 正文文件名是哈希，路径再怪也合法"),
              list.isEmpty() ? QString() : QFileInfo(list.first().contentPath).fileName());
        check(!list.isEmpty() && readAll(list.first().contentPath) == QStringLiteral("怪路径的内容"),
              QStringLiteral("A3-7 内容正确"));
    }

    // ---------------- 8. prune：按天数淘汰 ----------------
    {
        QString error;
        recovery.discardAll(&error);

        // 一条"新"草稿
        recovery.store(makeDraft(docA, QStringLiteral("笔记 A.md"), 1, 1), QStringLiteral("新"), &error);

        // 一条"老"草稿：savedAt 手工往前拨 40 天
        DraftRecovery::Draft old = makeDraft(docB, QStringLiteral("笔记 B.md"), 1, 1);
        old.savedAt = QDateTime::currentDateTime().addDays(-40);
        recovery.store(old, QStringLiteral("旧"), &error);
        check(recovery.pending().size() == 2, QStringLiteral("A3-8 准备了新老各 1 份"));

        check(recovery.pruneOlderThan(30) == 1, QStringLiteral("A3-8 prune(30 天) 清掉 1 条"));
        const QList<DraftRecovery::Draft> left = recovery.pending();
        check(left.size() == 1 && !left.isEmpty() && left.first().documentPath == docA,
              QStringLiteral("A3-8 ★ 留下的是新草稿（旧的那条被清掉）"));

        check(recovery.pruneOlderThan(30) == 0,
              QStringLiteral("A3-8 再 prune 一次没有可清的（幂等）"));
        check(recovery.pruneOlderThan(0) == 1, QStringLiteral("A3-8 prune(0 天) 把剩下的也清掉"));
        check(recovery.pending().isEmpty(), QStringLiteral("A3-8 ★ prune(0) 之后草稿全空"));
    }

    // ---------------- 9. 未命名文档（没有路径）：用显示名当身份 ----------------
    {
        QString error;
        recovery.discardAll(&error);

        DraftRecovery::Draft unnamed = makeDraft(QString(), QStringLiteral("未命名 1"), 2, 2);
        check(recovery.store(unnamed, QStringLiteral("没保存过的内容"), &error),
              QStringLiteral("A3-9 未命名文档也能存（用显示名当键）"), error);

        const QList<DraftRecovery::Draft> list = recovery.pending();
        check(list.size() == 1 && !list.isEmpty() && list.first().documentPath.isEmpty(),
              QStringLiteral("A3-9 路径为空、显示名保留"));
        check(!list.isEmpty() && list.first().displayName == QStringLiteral("未命名 1"),
              QStringLiteral("A3-9 显示名读回来一致"));
        check(!list.isEmpty() && readAll(list.first().contentPath) == QStringLiteral("没保存过的内容"),
              QStringLiteral("A3-9 内容正确"));

        check(recovery.discard(QStringLiteral("未命名 1"), &error),
              QStringLiteral("A3-9 按显示名也丢得掉"), error);
        check(recovery.pending().isEmpty(), QStringLiteral("A3-9 丢完之后没有草稿了"));
    }

    // ---------------- 10. 空 key：既没路径也没名字，拒绝而不是写坏数据 ----------------
    {
        QString error;
        recovery.discardAll(&error);
        check(!recovery.store(makeDraft(QString(), QString(), 1, 1), QStringLiteral("x"), &error),
              QStringLiteral("A3-10 既没路径也没名字 → 拒绝"), error);
        check(!error.isEmpty(), QStringLiteral("A3-10 而且给出了原因"));
        check(recovery.pending().isEmpty(), QStringLiteral("A3-10 没有留下脏数据"));
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
