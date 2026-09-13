#ifndef VERSIONCONTROL_H
#define VERSIONCONTROL_H

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

namespace markdown_editor::core::storage {

// 版本控制服务：用系统 Git 给文档做"轻量快照"（本地历史）。
//
// 定位：不是要取代真正的版本管理，而是"保存一次 = 留一个可回看的版本"。
// 用户写笔记时不会手动 git commit，这个类替他把"保存"变成有历史的保存。
//
// ---- 仓库放在哪（这是个重要决定，理由写清楚）----
// **不在文档所在目录里 git init**，而是放在应用数据目录下、一个文档一个仓库：
//
//     <AppData>/Dev/MarkdownEditor/history/<文档名>-<路径哈希8位>/
//         ├── .git/
//         └── snapshot.md      ← 每次保存后的完整内容（固定名字）
//
// 三条理由：
//   1. 不能往用户的笔记目录里塞 .git —— 那会很唐突，用户也没同意过；
//   2. 如果那个目录**本来就是个仓库**（很多人整个笔记目录就是一个 git 仓库），
//      我们一提交就会把用户其它没准备好的改动一起带进去 —— 这是真会伤人的错误；
//   3. 放在 AppData 里则完全隔离：不碰用户任何目录，删掉也不影响文档本身。
// 代价是历史不在文档旁边（换个机器就没了）——这符合"轻量快照"的定位。
// 想改成"仓库就在文档目录里"，只需把 repositoryPathFor() 改成返回 absolutePath()，
// 别的地方都不用动（这也是把它做成一个函数而不是散落各处的目的）。
//
// ---- 每次都存成 UTF-8 ----
// 原文件可能是 GBK（记事本存的），但快照统一写成 UTF-8：快照的用途是"看历史、比差异"，
// 统一编码才能让 diff 可读。原文件的编码由 FileManager 负责，两边互不干扰。
//
// ---- 没有 git 怎么办 ----
// 一切操作都返回失败 + 一句人话原因，但**绝不影响保存文件**（FileManager 只写一条日志）。
// 所以本模块可以放心地被自动调用。
//
// ---- 实现要点 ----
//   * 用 QProcess 调系统 git（Windows 上要先装 Git 并加入 PATH）
//   * 所有命令都带 `-C <仓库目录>`：路径里有空格也不会出问题，也不用管进程的工作目录
//   * 环境变量：GIT_TERMINAL_PROMPT=0（绝不弹凭据窗口）、LC_ALL=C（固定英文报错，便于判断）
//   * 输出一律按 UTF-8 解码
//   * 提交者身份在**仓库级**设置（不动用户的全局配置）—— 很多人根本没配全局
//     user.name/user.email，不设的话 git commit 会直接失败
class VersionControl : public QObject
{
    Q_OBJECT

public:
    // 一条历史记录
    struct Commit
    {
        QString hash;       // 完整 40 位哈希
        QString shortHash;  // 前 7 位，给人看
        QDateTime time;     // 提交时间（带时区）
        QString message;    // 提交备注（只有第一行）
    };

    // 快照文件的固定名字。仓库里只有它，所以 diff 永远等于"这个文档的两个版本之差"。
    static QString snapshotFileName();

    explicit VersionControl(QObject *parent = nullptr);

    // ---- git 环境 ----
    // 找到的 git 可执行文件；找不到返回空字符串（结果会缓存，不会每次都查 PATH）
    static QString gitExecutable();
    static bool isGitAvailable();

    // 默认的快照备注：带时间戳，例如 "保存快照 2026-09-13 22:30:00"
    static QString defaultSnapshotMessage(const QDateTime &when = QDateTime::currentDateTime());

    // ---- 仓库位置 ----
    // 快照仓库的根目录。默认 <AppData>/Dev/MarkdownEditor/history；测试里用 setHistoryRoot() 指到临时目录。
    QString historyRoot() const;
    void setHistoryRoot(const QString &root);

    // 某个文档对应的快照仓库目录（纯计算，不创建任何东西）。
    // 目录名 = 文档名 + 路径哈希前 8 位：既能一眼看出是哪个文档，又不会重名冲突。
    QString repositoryPathFor(const QString &documentPath) const;

    // ---- 四个功能 ----
    // 这个目录是不是一个 git 仓库（看 .git 在不在；不启动进程，所以很快）
    bool isRepository(const QString &repoDir) const;

    // 初始化仓库：建目录 + git init + 设置仓库级配置（提交者身份、不转换换行符等）。
    // 成功返回 true；已经是仓库时直接返回 true（幂等，可以随便重复调用）。
    // 失败：false + error；常见原因是没装 git、路径不可写。
    bool initRepository(const QString &repoDir, QString *error = nullptr);

    // 提交一次快照：把 content 写成仓库里的 snapshot.md，然后 add + commit。
    // message 为空时自动用 defaultSnapshotMessage()（带时间戳）。
    //
    // 返回新提交的 40 位哈希。
    // **内容与上一版完全相同时返回空字符串**——这不是错误（error 保持为空），
    // 意思是"没有新版本可记"。调用方据此区分"没变化"和"失败了"：
    //     返回空 + error 为空   → 内容没变，正常
    //     返回空 + error 非空   → 真的失败了
    QString commitSnapshot(const QString &repoDir,
                           const QString &content,
                           const QString &message = QString(),
                           QString *error = nullptr);

    // 历史列表，**最新的在前**。maxCount 是上限（默认 50）。
    // 仓库还没有任何提交时返回空列表且不报错（"空历史"不是错误）。
    QList<Commit> history(const QString &repoDir, int maxCount = 50, QString *error = nullptr) const;

    // 两个版本的差异（unified diff 文本，含 +/- 行）。
    // toRev 为空 = 与仓库工作区里的当前内容比。
    // 只比较快照文件，不会把仓库里其它东西带进来。
    QString diff(const QString &repoDir,
                 const QString &fromRev,
                 const QString &toRev = QString(),
                 QString *error = nullptr) const;

    // 某个提交相对它**父提交**的差异（历史列表里点一条最常用这个）。
    // 第一个提交没有父提交：此时与 git 的"空树"比较，得到"全部是新增"的 diff。
    QString diffWithParent(const QString &repoDir, const QString &rev, QString *error = nullptr) const;

    // 取某个版本里保存的**完整内容**（就是那一版的 snapshot.md 内容），回滚功能用它。
    // 成功：返回内容。**返回空字符串也可能是成功** —— 那一版就是个空文档，
    //       所以判断成败要看 error 是否为空（同 commitSnapshot 的约定）。
    // 失败：返回空 + error（版本号不存在、仓库不存在……）。
    QString contentOf(const QString &repoDir, const QString &rev, QString *error = nullptr) const;

signals:
    // 新建了一个快照
    void snapshotCreated(const QString &repoDir, const QString &hash, const QString &message);

private:
    QString m_historyRoot;  // 空 = 用默认位置（见 historyRoot()）
};

}  // namespace markdown_editor::core::storage

#endif // VERSIONCONTROL_H
