// VersionControl（4.2.2 版本控制服务）的契约测试。
//
// 这个测试会**真的调用系统 git**（通过 QProcess）—— 所以前提是机器上装了 Git、并且能在 PATH 里找到。
// 找不到就打印一行 SKIP 并返回 0：Git 没装不是本项目代码的问题，不该让整个测试套挂掉。
//
// 所有操作都在系统临时目录里进行：仓库根目录用 setHistoryRoot() 指过去，
// 绝不碰 AppData 里那份真实的历史（这和"测试不污染用户数据"是同一条原则）。
//
// 跑法：ctest -C Debug --output-on-failure

#include "versioncontrol.h"
#include "fileutils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cstdio>

using markdown_editor::core::storage::VersionControl;

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

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    if (!VersionControl::isGitAvailable()) {
        std::printf("SKIP: 系统里没找到 git（装好 Git 并加入 PATH 后会正常执行这些检查）\n");
        return 0;
    }

    const QString work = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-versioncontrol-test"));
    QDir(work).removeRecursively();
    if (!QDir().mkpath(work)) {
        std::printf("cannot create temp dir: %s\n", work.toUtf8().constData());
        return 2;
    }

    VersionControl vc;
    vc.setHistoryRoot(work + QStringLiteral("/history"));

    QString err;

    // ---------------- git 环境 ----------------
    check(!VersionControl::gitExecutable().isEmpty(),
          QStringLiteral("gitExecutable: 找得到 git"),
          VersionControl::gitExecutable());

    // ---------------- 默认备注（4.2.2 要求"带时间戳"）----------------
    {
        const QString note = VersionControl::defaultSnapshotMessage();
        check(note.startsWith(QStringLiteral("保存快照 ")), QStringLiteral("默认备注: 以「保存快照 」开头"), note);
        check(note.contains(QRegularExpression(QStringLiteral("\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}"))),
              QStringLiteral("默认备注: 含 yyyy-MM-dd HH:mm:ss 时间戳"), note);
    }

    // ---------------- 仓库位置推导 ----------------
    {
        const QString docPath = work + QStringLiteral("/笔记 note.md");
        FileUtils::writeFile(docPath, QStringLiteral("# 标题\n"));

        const QString repoDir = vc.repositoryPathFor(docPath);
        check(repoDir.startsWith(work + QStringLiteral("/history")),
              QStringLiteral("repositoryPathFor: 落在 historyRoot 下面"), repoDir);
        check(repoDir.contains(QStringLiteral("笔记")),
              QStringLiteral("repositoryPathFor: 目录名带文档名（便于辨认）"));
        check(vc.repositoryPathFor(docPath) == repoDir, QStringLiteral("repositoryPathFor: 同一路径结果稳定"));
        check(vc.repositoryPathFor(docPath.toUpper()) == repoDir,
              QStringLiteral("repositoryPathFor: 大小写不同 = 同一个文件 → 同一仓库"));
        check(vc.repositoryPathFor(work + QStringLiteral("/另一个.md")) != repoDir,
              QStringLiteral("repositoryPathFor: 不同文档 → 不同仓库"));

        // 这个仓库目录现在还不该存在（repositoryPathFor 是纯计算，不创建东西）
        check(!QFileInfo::exists(repoDir), QStringLiteral("repositoryPathFor: 只是算路径，不创建目录"));
    }

    const QString docPath = work + QStringLiteral("/笔记 note.md");
    const QString repoDir = vc.repositoryPathFor(docPath);

    // ---------------- 初始化仓库 ----------------
    check(!vc.isRepository(repoDir), QStringLiteral("初始化之前: isRepository = false"));
    check(vc.initRepository(repoDir, &err), QStringLiteral("initRepository: 成功"), err);
    check(vc.isRepository(repoDir), QStringLiteral("初始化之后: isRepository = true"));
    check(QFileInfo::exists(repoDir + QStringLiteral("/.git")), QStringLiteral("initRepository: .git 真的建出来了"));
    check(vc.initRepository(repoDir, &err), QStringLiteral("initRepository: 重复调用仍然成功（幂等）"));

    {
        QString errEmpty;
        const QList<VersionControl::Commit> none = vc.history(repoDir, 10, &errEmpty);
        check(none.isEmpty() && errEmpty.isEmpty(),
              QStringLiteral("history: 还没有任何提交时返回空列表且不报错"));
    }

    // ---------------- 提交快照 ----------------
    const QString v1 = QStringLiteral("# 标题\n\n第一行\n旧的一行\n");
    const QString hash1 = vc.commitSnapshot(repoDir, v1, QStringLiteral("第一版"), &err);
    check(hash1.size() == 40, QStringLiteral("commitSnapshot: 返回 40 位哈希"), hash1);
    check(hash1.contains(QRegularExpression(QStringLiteral("^[0-9a-f]{40}$"))),
          QStringLiteral("commitSnapshot: 哈希是十六进制"));

    QList<VersionControl::Commit> commits = vc.history(repoDir, 10, &err);
    check(err.isEmpty() && commits.size() == 1, QStringLiteral("history: 1 条记录"),
          QStringLiteral("%1 条").arg(commits.size()));
    if (!commits.isEmpty()) {
        check(commits.first().hash == hash1, QStringLiteral("history: 最新那条就是刚提交的"));
        check(commits.first().shortHash == hash1.left(7), QStringLiteral("history: shortHash 是哈希前 7 位"));
        check(commits.first().message == QStringLiteral("第一版"), QStringLiteral("history: 备注原样保留"));
        check(commits.first().time.isValid(), QStringLiteral("history: 时间是有效的 QDateTime"));
        check(qAbs(commits.first().time.secsTo(QDateTime::currentDateTime())) < 300,
              QStringLiteral("history: 提交时间接近现在"));
    }

    // ---------------- 内容没变 → 不产生新提交 ----------------
    {
        const QString same = vc.commitSnapshot(repoDir, v1, QStringLiteral("内容一样的备注"), &err);
        check(same.isEmpty() && err.isEmpty(),
              QStringLiteral("commitSnapshot: 内容没变 → 返回空且不报错（这不是失败）"));
        check(vc.history(repoDir, 10, &err).size() == 1, QStringLiteral("内容没变时: 历史条数不变"));
    }

    // ---------------- 改内容 → 第二个快照 ----------------
    const QString v2 = QStringLiteral("# 标题\n\n第一行\n中文修改\n");
    const QString hash2 = vc.commitSnapshot(repoDir, v2, QStringLiteral("第二版"), &err);
    check(!hash2.isEmpty() && hash2 != hash1, QStringLiteral("commitSnapshot: 内容变了 → 新哈希"), hash2.left(7));

    commits = vc.history(repoDir, 10, &err);
    check(commits.size() == 2, QStringLiteral("history: 2 条记录"), QStringLiteral("%1 条").arg(commits.size()));
    check(!commits.isEmpty() && commits.first().hash == hash2, QStringLiteral("history: 最新的排在最上面"));
    check(vc.history(repoDir, 1, &err).size() == 1, QStringLiteral("history: maxCount 生效"));

    // ---------------- 差异（4.2.2 的验收点之一）----------------
    {
        const QString diffText = vc.diff(repoDir, hash1, hash2, &err);
        check(err.isEmpty() && !diffText.isEmpty(), QStringLiteral("diff: 两个版本之间能算出差异"),
              QStringLiteral("%1 字符").arg(diffText.size()));
        check(diffText.contains(QStringLiteral("-旧的一行")), QStringLiteral("diff: 有删除行（- 开头）"));
        check(diffText.contains(QStringLiteral("+中文修改")), QStringLiteral("diff: 有新增行（+ 开头），中文可读"));
        check(!diffText.contains(QStringLiteral("-第一行")) && !diffText.contains(QStringLiteral("+第一行")),
              QStringLiteral("diff: 没变的行不出现在差异里"));

        check(vc.diffWithParent(repoDir, hash2, &err) == diffText,
              QStringLiteral("diffWithParent: 等于「上一版 → 这一版」"));

        const QString firstDiff = vc.diffWithParent(repoDir, hash1, &err);
        check(firstDiff.contains(QStringLiteral("+# 标题")) && firstDiff.contains(QStringLiteral("+第一行")),
              QStringLiteral("diffWithParent: 第一个提交与空树比 → 全部算新增"));
    }

    // ---------------- 取某个版本的完整内容（回滚功能的基础）----------------
    {
        const QString content1 = vc.contentOf(repoDir, hash1, &err);
        check(err.isEmpty() && content1 == v1, QStringLiteral("contentOf: 取回第一版的完整内容"));

        const QString content2 = vc.contentOf(repoDir, hash2, &err);
        check(err.isEmpty() && content2 == v2, QStringLiteral("contentOf: 取回第二版的完整内容"));
        check(content1 != content2, QStringLiteral("contentOf: 两版内容确实不同"));

        // 空文档那一版：返回空字符串但**不报错**（空 != 失败，这条约定回滚功能要靠它）
        const QString emptyHash = vc.commitSnapshot(repoDir, QString(), QStringLiteral("空文档"), &err);
        check(!emptyHash.isEmpty(), QStringLiteral("contentOf: 能提交空文档作为一版"), err);
        const QString emptyContent = vc.contentOf(repoDir, emptyHash, &err);
        check(emptyContent.isEmpty() && err.isEmpty(),
              QStringLiteral("contentOf: 那一版是空文档 → 返回空且不报错"));

        check(vc.contentOf(repoDir, QStringLiteral("不存在的版本"), &err).isEmpty() && !err.isEmpty(),
              QStringLiteral("contentOf: 版本号不存在 → 空 + 原因"), err);
        check(vc.contentOf(repoDir, QString(), &err).isEmpty() && !err.isEmpty(),
              QStringLiteral("contentOf: 没给版本 → 空 + 原因"));
    }

    // ---------------- 失败路径（必须给出人能看懂的原因）----------------
    check(vc.diff(repoDir, QStringLiteral("不存在的版本"), hash2, &err).isEmpty() && !err.isEmpty(),
          QStringLiteral("diff: 版本号不存在 → 空 + 原因"), err);
    check(vc.commitSnapshot(work + QStringLiteral("/nope-repo"), v1, QStringLiteral("x"), &err).isEmpty()
              && !err.isEmpty(),
          QStringLiteral("commitSnapshot: 仓库不存在 → 空 + 原因"), err);
    check(!vc.initRepository(QString(), &err) && !err.isEmpty(),
          QStringLiteral("initRepository: 空路径 → false + 原因"), err);
    check(vc.diff(repoDir, QString(), hash2, &err).isEmpty() && !err.isEmpty(),
          QStringLiteral("diff: 没给起始版本 → 空 + 原因"), err);
    check(!vc.initRepository(work + QStringLiteral("/笔记 note.md"), &err) && !err.isEmpty(),
          QStringLiteral("initRepository: 目标是文件 → false + 原因"), err);

    // ---------------- 中文 / emoji：备注和内容都不能乱码 ----------------
    {
        const QString v3 = QStringLiteral("# 标题\n\nemoji 🎉 和中文\n");
        const QString hash3 = vc.commitSnapshot(repoDir, v3, QStringLiteral("中文备注 😀"), &err);
        check(!hash3.isEmpty(), QStringLiteral("commitSnapshot: 中文 + emoji 备注能提交"), err);

        const QList<VersionControl::Commit> latest = vc.history(repoDir, 1, &err);
        check(!latest.isEmpty() && latest.first().message == QStringLiteral("中文备注 😀"),
              QStringLiteral("history: 中文 / emoji 备注读回来一字不差"),
              latest.isEmpty() ? QString() : latest.first().message);

        const QString emojiDiff = vc.diff(repoDir, hash2, hash3, &err);
        check(emojiDiff.contains(QStringLiteral("emoji 🎉")),
              QStringLiteral("diff: 快照里的 emoji 可读（说明按 UTF-8 存/读）"));
    }

    // ---------------- 信号 + 自动备注 ----------------
    {
        // 注意：变量名不能叫 signals —— 那是 Qt 的宏（展开成 public），
        // 写 int signals = 0; 会被编译器读成 int public = 0;
        int signalCount = 0;
        QString lastHash;
        QObject::connect(&vc, &VersionControl::snapshotCreated, [&](const QString &repo, const QString &hash, const QString &) {
            Q_UNUSED(repo);
            ++signalCount;
            lastHash = hash;
        });

        const QString hash4 = vc.commitSnapshot(repoDir, QStringLiteral("# 又改了一次\n"), QString(), &err);
        check(!hash4.isEmpty(), QStringLiteral("commitSnapshot: 备注传空时自动用带时间戳的默认备注"), err);
        check(signalCount == 1 && lastHash == hash4,
              QStringLiteral("snapshotCreated: 提交成功时发信号并带上哈希"));

        const QList<VersionControl::Commit> latest = vc.history(repoDir, 1, &err);
        check(!latest.isEmpty() && latest.first().message.startsWith(QStringLiteral("保存快照 ")),
              QStringLiteral("默认备注: 历史里那条确实带时间戳"),
              latest.isEmpty() ? QString() : latest.first().message);
    }

    QDir(work).removeRecursively();

    if (g_fail == 0) {
        std::printf("\n=== VersionControl 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== VersionControl 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
