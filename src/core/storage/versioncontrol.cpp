#include "versioncontrol.h"

#include "fileutils.h"
#include "logger.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

namespace markdown_editor::core::storage {

namespace {

// git 里"空树"对象的固定哈希（官方常量）。
// 用来给"第一个提交"算 diff：与空树比 = 全部是新增，正好是用户想看的东西。
const char *const kEmptyTreeHash = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

// 所有 git 调用的唯一入口。
//
// 三件事必须一起做对，否则会出各种怪问题：
//   * 用 -C <仓库目录> 而不是 setWorkingDirectory()：路径带空格/中文都稳，
//     而且不会因为进程工作目录被别处改掉而跑错仓库
//   * GIT_TERMINAL_PROMPT=0：万一条命令不小心触发了认证，也不要弹出窗口卡住整个程序
//   * LC_ALL=C：报错固定英文（用户机器上装了中文语言包时，靠字符串判断会失效）
bool runGit(const QString &repoDir,
            const QStringList &args,
            QString *output,
            QString *error,
            int timeoutMs = 20000)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };

    const QString exe = VersionControl::gitExecutable();
    if (exe.isEmpty()) {
        return fail(QStringLiteral("系统里没有找到 git（Windows 上需要先安装 Git，并把它加入 PATH)"));
    }

    QProcess process;
    process.setProgram(exe);
    process.setArguments(QStringList{QStringLiteral("-C"), repoDir} + args);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(env);

    process.start();
    if (!process.waitForStarted(timeoutMs)) {
        return fail(QStringLiteral("启动 git 失败：%1").arg(process.errorString()));
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return fail(QStringLiteral("git 命令超时（%1 毫秒）：git %2").arg(timeoutMs).arg(args.join(QLatin1Char(' '))));
    }

    const QByteArray outBytes = process.readAllStandardOutput();
    const QByteArray errBytes = process.readAllStandardError();

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        // git 的失败原因在 stderr 上；实在没有就把 stdout 或退出码带上，别让用户看到空白原因
        QString why = QString::fromUtf8(errBytes).trimmed();
        if (why.isEmpty()) {
            why = QString::fromUtf8(outBytes).trimmed();
        }
        if (why.isEmpty()) {
            why = QStringLiteral("git 退出码 %1").arg(process.exitCode());
        }
        return fail(why);
    }

    if (output != nullptr) {
        *output = QString::fromUtf8(outBytes);
    }
    if (error != nullptr) {
        error->clear();  // 成功时清空，避免调用方读到上一轮的旧消息
    }
    return true;
}

}  // namespace

QString VersionControl::snapshotFileName()
{
    // 仓库里只有这一个文件，所以 diff 永远等于"这个文档的两个版本之差"
    return QStringLiteral("snapshot.md");
}

VersionControl::VersionControl(QObject *parent) : QObject(parent) {}

QString VersionControl::gitExecutable()
{
    // 查 PATH 有点慢，而且结果不会变，所以缓存一次
    static const QString cached = QStandardPaths::findExecutable(QStringLiteral("git"));
    return cached;
}

bool VersionControl::isGitAvailable()
{
    return !gitExecutable().isEmpty();
}

QString VersionControl::defaultSnapshotMessage(const QDateTime &when)
{
    // 4.2.2 要求"带时间戳备注"：一眼能看出这份快照是什么时候留下的
    return QStringLiteral("保存快照 %1").arg(when.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

QString VersionControl::historyRoot() const
{
    if (!m_historyRoot.isEmpty()) {
        return m_historyRoot;
    }

    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        base = QCoreApplication::applicationDirPath();  // 极端情况兜底，保证一定有个位置
    }
    return base + QStringLiteral("/history");
}

void VersionControl::setHistoryRoot(const QString &root)
{
    m_historyRoot = root;
}

QString VersionControl::repositoryPathFor(const QString &documentPath) const
{
    // 先统一成正斜杠 + 全小写：Windows 路径不区分大小写，D:\a\B.md 和 d:/a/b.md 是**同一个文件**，
    // 必须落到同一个仓库，否则同一个文档会出现两份互不相干的历史。
    const QString normalized = QDir::fromNativeSeparators(QFileInfo(documentPath).absoluteFilePath()).toLower();
    const QFileInfo info(normalized);

    // 目录名带上文档名，方便人一眼认出这是哪个文档的历史。
    // 只保留"字母/数字/点/下划线/短横线"和中文等文字字符，其它一律换成下划线
    //（反斜杠、冒号、空格这些在目录名里要么非法要么容易出问题）。
    QString stem = info.completeBaseName();
    if (stem.isEmpty()) {
        stem = QStringLiteral("document");
    }
    static const QRegularExpression unsafe(QStringLiteral("[^\\p{L}\\p{N}._-]+"));
    stem.replace(unsafe, QStringLiteral("_"));
    if (stem.size() > 40) {
        stem = stem.left(40);
    }

    // 哈希负责区分同名文档（normalized 已经是统一形式，所以大小写/斜杠差异不会产生两个仓库）
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));

    return historyRoot() + QLatin1Char('/') + stem + QLatin1Char('-') + key;
}

bool VersionControl::isRepository(const QString &repoDir) const
{
    if (repoDir.isEmpty()) {
        return false;
    }
    // 注意是查文件系统而不是启动 git 进程：这个判断在每次保存时都会走一遍，
    // 起一次进程（Windows 上几十毫秒）不值得。.git 可能是目录（普通仓库）
    // 也可能是文件（worktree / submodule），QFileInfo::exists 两种情况都覆盖。
    return QFileInfo::exists(repoDir + QStringLiteral("/.git"));
}

bool VersionControl::initRepository(const QString &repoDir, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    if (repoDir.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("快照仓库目录是空的");
        }
        return false;
    }

    if (isRepository(repoDir)) {
        return true;  // 幂等：已经是仓库了就直接成功，调用方可以每次保存都无脑调
    }

    if (!QDir().mkpath(repoDir)) {
        if (error != nullptr) {
            *error = QStringLiteral("创建不了快照仓库目录：%1").arg(repoDir);
        }
        return false;
    }

    QString out;
    if (!runGit(repoDir, QStringList{QStringLiteral("init")}, &out, error)) {
        return false;
    }

    // 下面这些都写在**仓库级**配置里，绝不碰用户的全局配置。
    // 第一组不是"讲究"而是必须：这台机器上就没配全局 user.name/user.email，
    // 不设的话 git commit 会直接以 "Please tell me who you are" 失败。
    QString ignored;  // 配置失败不致命，原因不值得覆盖 error
    runGit(repoDir, QStringList{QStringLiteral("config"), QStringLiteral("user.name"),
                                QStringLiteral("Markdown Editor")}, &out, &ignored);
    runGit(repoDir, QStringList{QStringLiteral("config"), QStringLiteral("user.email"),
                                QStringLiteral("markdown-editor@localhost")}, &out, &ignored);
    // 快照要保证"仓库里的字节 = 我们写进去的字节"，所以关掉换行符转换。
    // 否则 diff 出来的内容会和真实文件不一致（还可能出现只有 ^M 的假差异）。
    runGit(repoDir, QStringList{QStringLiteral("config"), QStringLiteral("core.autocrlf"),
                                QStringLiteral("false")}, &out, &ignored);
    // 关掉路径转义：出现中文文件名时 diff 里直接显示中文，而不是 \344\275\240 这种
    runGit(repoDir, QStringList{QStringLiteral("config"), QStringLiteral("core.quotepath"),
                                QStringLiteral("false")}, &out, &ignored);
    // 保证 git 输出的是 UTF-8，和 QProcess 那边的解码方式对上
    runGit(repoDir, QStringList{QStringLiteral("config"), QStringLiteral("i18n.logOutputEncoding"),
                                QStringLiteral("UTF-8")}, &out, &ignored);

    LOG_INFO("已初始化快照仓库: %1", repoDir);
    return true;
}

QString VersionControl::commitSnapshot(const QString &repoDir,
                                       const QString &content,
                                       const QString &message,
                                       QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    if (!isRepository(repoDir)) {
        if (error != nullptr) {
            *error = QStringLiteral("还不是快照仓库（先调用 initRepository）：%1").arg(repoDir);
        }
        return QString();
    }

    // 快照统一存 UTF-8：原文件可能是 GBK，但快照的用途是"看历史/比差异"，统一编码才可读
    if (!FileUtils::writeFileBytes(repoDir + QLatin1Char('/') + snapshotFileName(), content.toUtf8(), error)) {
        return QString();
    }

    QString out;
    if (!runGit(repoDir, QStringList{QStringLiteral("add"), QStringLiteral("--"), snapshotFileName()}, &out, error)) {
        return QString();
    }

    // 先看有没有东西可提交：内容与上一版一样时不要留下空提交。
    // （判断放在 commit 之前，比"让 commit 失败再看错误文本"更可靠。）
    QString status;
    if (!runGit(repoDir, QStringList{QStringLiteral("status"), QStringLiteral("--porcelain")}, &status, error)) {
        return QString();
    }
    if (status.trimmed().isEmpty()) {
        return QString();  // 没有变化：不是错误，只是没有新版本可记
    }

    const QString note = message.isEmpty() ? defaultSnapshotMessage() : message;
    if (!runGit(repoDir, QStringList{QStringLiteral("commit"), QStringLiteral("--quiet"),
                                     QStringLiteral("-m"), note}, &out, error)) {
        return QString();
    }

    QString head;
    if (!runGit(repoDir, QStringList{QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &head, error)) {
        return QString();
    }

    const QString hash = head.trimmed();
    LOG_INFO("快照已提交: %1 (%2) %3", hash.left(7), repoDir, note);
    emit snapshotCreated(repoDir, hash, note);
    return hash;
}

QList<VersionControl::Commit> VersionControl::history(const QString &repoDir, int maxCount, QString *error) const
{
    QList<Commit> commits;

    if (error != nullptr) {
        error->clear();
    }
    if (!isRepository(repoDir)) {
        if (error != nullptr) {
            *error = QStringLiteral("还不是快照仓库：%1").arg(repoDir);
        }
        return commits;
    }

    if (maxCount <= 0) {
        maxCount = 50;
    }

    // 用 %x1f（单元分隔符）分隔字段：提交备注里的空格、引号、冒号都不会干扰解析。
    // %cI = 严格的 ISO 8601 时间（带时区），QDateTime::fromString(Qt::ISODate) 直接能解。
    const QString pretty = QStringLiteral("--pretty=format:%H%x1f%h%x1f%cI%x1f%s");
    QString out;
    if (!runGit(repoDir,
                QStringList{QStringLiteral("log"), QStringLiteral("-n"), QString::number(maxCount), pretty},
                &out,
                error)) {
        // 仓库刚 init、一个提交都还没有时 git log 会失败 —— 这不是错误，是"空历史"
        if (error != nullptr) {
            error->clear();
        }
        return commits;
    }

    const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList parts = line.split(QChar(0x1f));
        if (parts.size() < 4) {
            continue;  // 格式对不上就跳过这一行，不要让整个列表失败
        }

        Commit commit;
        commit.hash = parts.at(0);
        commit.shortHash = parts.at(1);
        commit.time = QDateTime::fromString(parts.at(2), Qt::ISODate);
        commit.message = parts.at(3);
        commits.append(commit);
    }

    return commits;
}

QString VersionControl::diff(const QString &repoDir,
                             const QString &fromRev,
                             const QString &toRev,
                             QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!isRepository(repoDir)) {
        if (error != nullptr) {
            *error = QStringLiteral("还不是快照仓库：%1").arg(repoDir);
        }
        return QString();
    }
    if (fromRev.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("没有指定要对比的版本");
        }
        return QString();
    }

    QStringList args{QStringLiteral("diff"),
                     QStringLiteral("--no-color"),
                     QStringLiteral("--unified=3"),
                     fromRev};
    if (!toRev.isEmpty()) {
        args << toRev;
    } else {
        // toRev 为空：与仓库工作区里的当前内容比（也就是 snapshot.md 现在的样子）
        LOG_INFO("对比: %1 → 工作区当前内容", fromRev.left(7));
    }
    // 只看快照文件，别把仓库里其它东西带进来
    args << QStringLiteral("--") << snapshotFileName();

    QString out;
    if (!runGit(repoDir, args, &out, error)) {
        return QString();
    }
    return out;
}

QString VersionControl::diffWithParent(const QString &repoDir, const QString &rev, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (rev.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("没有指定版本");
        }
        return QString();
    }

    // 先问 git 这个提交有没有父提交：rev^ 在第一个提交上会直接失败，
    // 所以那种情况改用"空树"，得到"全部是新增"的 diff。
    QString parents;
    if (!runGit(repoDir,
                QStringList{QStringLiteral("rev-list"), QStringLiteral("--parents"), QStringLiteral("-n"),
                            QStringLiteral("1"), rev},
                &parents,
                error)) {
        return QString();
    }

    const QStringList fields = parents.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString base = (fields.size() >= 2) ? fields.at(1) : QString::fromLatin1(kEmptyTreeHash);
    return diff(repoDir, base, rev, error);
}

QString VersionControl::contentOf(const QString &repoDir, const QString &rev, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!isRepository(repoDir)) {
        if (error != nullptr) {
            *error = QStringLiteral("还不是快照仓库：%1").arg(repoDir);
        }
        return QString();
    }
    if (rev.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("没有指定版本");
        }
        return QString();
    }

    // <rev>:<路径> 是 git 的"对象说明"语法：直接取那个提交里这个文件的内容。
    // 注意不要用 "git show <rev>"（那会连提交信息、diff 一起打出来）。
    const QString objectRef = QStringLiteral("%1:%2").arg(rev, snapshotFileName());

    QString out;
    if (!runGit(repoDir, QStringList{QStringLiteral("show"), objectRef}, &out, error)) {
        return QString();
    }
    return out;
}

}  // namespace markdown_editor::core::storage
