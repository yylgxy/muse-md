#include "draftrecovery.h"

#include "fileutils.h"
#include "logger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStandardPaths>

namespace markdown_editor::core::storage {

namespace {

// manifest.json 里每条记录用的键名。写成常量是为了"读到旧版本文件时缺键不崩"这件事好核对。
const char *kKeyDocument = "document";
const char *kKeyDisplayName = "displayName";
const char *kKeySavedAt = "savedAt";
const char *kKeyCursorLine = "cursorLine";
const char *kKeyCursorColumn = "cursorColumn";
const char *kKeyContent = "content";

// 路径 → 文件名的哈希。为什么用它而不是"把路径转义一下当文件名"：
//   路径里可能有中文、空格、冒号、超过 260 字符 —— 直接当文件名在 Windows 上会失败。
//   sha1 十六进制固定 40 个字符，一定合法。哈希碰撞在这里不是安全问题
//   （同一台机器的草稿目录，且 manifest 里还存着原路径可以对账）。
QString hashKey(const QString &key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
}

}  // namespace

// ============================ Draft ============================

QString DraftRecovery::Draft::key() const
{
    // 有路径就用路径，没有（新建未保存的文档）就用显示名。
    // 空字符串是唯一的"无效"情况：既没路径也没名字，那这条草稿无从归属。
    return documentPath.isEmpty() ? displayName : documentPath;
}

// ============================ 构造与目录 ============================

DraftRecovery::DraftRecovery(const QString &rootDir)
    : m_rootDir(rootDir.isEmpty() ? defaultRootDir() : rootDir)
{
}

QString DraftRecovery::defaultRootDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        // 极端情况兜底：至少保证草稿有地方放（和 FullTextSearch / ConfigManager 的处理一致）
        base = QDir::tempPath();
    }
    return base + QStringLiteral("/drafts");
}

QString DraftRecovery::rootDir() const
{
    return m_rootDir;
}

QString DraftRecovery::contentPathFor(const QString &key) const
{
    return m_rootDir + QLatin1Char('/') + hashKey(key) + QStringLiteral(".draft");
}

// ============================ manifest 读写 ============================

// manifest 的一条记录。和公开的 Draft 分开是因为"持久化格式"和"对外接口"该能各自演进 ——
// 比如以后 manifest 里想多存一个"编辑器是第几个标签"，不必改 Draft。
struct DraftRecovery::Entry
{
    QString key;  // documentPath 或 displayName
    Draft draft;
};

QList<DraftRecovery::Entry> DraftRecovery::readManifest(bool *ok) const
{
    QList<Entry> entries;
    const QString manifestPath = m_rootDir + QStringLiteral("/manifest.json");

    if (ok != nullptr) {
        *ok = true;
    }
    if (!QFile::exists(manifestPath)) {
        return entries;  // 还没有草稿：不是错误
    }

    QString text;
    QString error;
    if (!FileUtils::readFile(manifestPath, text, &error)) {
        // ★ 容错（A3 的核心要求）：manifest 读不出来就当作"没有草稿"，绝不让程序起不来。
        LOG_WARN("草稿 manifest 读不出来（当作没有草稿）：%1", error);
        if (ok != nullptr) {
            *ok = false;
        }
        return entries;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // ★ 容错：坏 JSON → 空列表且不崩（测试第 5 条钉的就是这里）
        LOG_WARN("草稿 manifest 是坏 JSON（当作没有草稿）：%1", parseError.errorString());
        if (ok != nullptr) {
            *ok = false;
        }
        return entries;
    }

    const QJsonArray array = doc.object().value(QStringLiteral("drafts")).toArray();
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;  // 单条坏掉只丢这一条，不牵连整份 manifest
        }
        const QJsonObject o = value.toObject();

        Entry entry;
        entry.key = o.value(QLatin1String(kKeyDocument)).toString();
        entry.draft.documentPath = entry.key;
        entry.draft.displayName = o.value(QLatin1String(kKeyDisplayName)).toString();
        if (entry.key.isEmpty()) {
            // 未命名文档：键就是显示名
            entry.key = entry.draft.displayName;
        }
        if (entry.key.isEmpty()) {
            continue;  // 既没路径也没名字：这条无从恢复，跳过
        }

        entry.draft.savedAt = QDateTime::fromString(
            o.value(QLatin1String(kKeySavedAt)).toString(), Qt::ISODate);
        // 缺失的键一律取默认值（和 SessionState 的容错原则一致）
        entry.draft.cursorLine = o.value(QLatin1String(kKeyCursorLine)).toInt(1);
        entry.draft.cursorColumn = o.value(QLatin1String(kKeyCursorColumn)).toInt(1);
        entry.draft.contentPath = o.value(QLatin1String(kKeyContent)).toString();
        if (entry.draft.contentPath.isEmpty()) {
            entry.draft.contentPath = contentPathFor(entry.key);
        }
        entries << entry;
    }
    return entries;
}

bool DraftRecovery::writeManifest(const QList<Entry> &entries, QString *error) const
{
    QJsonArray array;
    for (const Entry &entry : entries) {
        QJsonObject o;
        if (!entry.draft.documentPath.isEmpty()) {
            // 未命名文档不写 document 键（写了空串反而让读取端要做"空串算不算有路径"的判断）
            o.insert(QLatin1String(kKeyDocument), entry.draft.documentPath);
        }
        o.insert(QLatin1String(kKeyDisplayName), entry.draft.displayName);
        o.insert(QLatin1String(kKeySavedAt), entry.draft.savedAt.toString(Qt::ISODate));
        o.insert(QLatin1String(kKeyCursorLine), entry.draft.cursorLine);
        o.insert(QLatin1String(kKeyCursorColumn), entry.draft.cursorColumn);
        o.insert(QLatin1String(kKeyContent), entry.draft.contentPath);
        array.append(o);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("drafts"), array);

    // ★ 必须走 FileUtils::writeFile（QSaveFile 原子替换）：
    //   manifest 写一半崩了，会变成坏 JSON —— 那样**所有**草稿都失联了（见 readManifest 的容错）。
    return FileUtils::writeFile(m_rootDir + QStringLiteral("/manifest.json"),
                                QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)),
                                error);
}

// ============================ 存 ============================

bool DraftRecovery::store(const Draft &draft, const QString &content, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    const QString key = draft.key();
    if (key.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("这条草稿既没有文档路径也没有名字，无从归属");
        }
        return false;
    }

    if (!QDir().mkpath(m_rootDir)) {
        if (error != nullptr) {
            *error = QStringLiteral("建不了草稿目录：%1").arg(m_rootDir);
        }
        LOG_WARN("建不了草稿目录：%1", m_rootDir);
        return false;
    }

    Draft stored = draft;
    stored.contentPath = contentPathFor(key);

    // ---- 1. 先落正文（原子写）----
    QString writeError;
    if (!FileUtils::writeFile(stored.contentPath, content, &writeError)) {
        if (error != nullptr) {
            *error = writeError;
        }
        LOG_WARN("草稿正文写失败（不影响正常保存）：%1", writeError);
        return false;
    }

    // ---- 2. 再更新 manifest（同一份文档只保留一条：按 key 替换）----
    QList<Entry> entries = readManifest();
    bool replaced = false;
    for (Entry &entry : entries) {
        if (entry.key == key) {
            entry.draft = stored;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        Entry entry;
        entry.key = key;
        entry.draft = stored;
        entries << entry;
    }

    QString manifestError;
    if (!writeManifest(entries, &manifestError)) {
        // ★ 正文写了但 manifest 没更新 → 这条正文是孤儿。
        //   删掉它再返回 false：否则 pending() 看不到它、prune 又会把它当"已失效文件"清掉，
        //   两条路径对同一个文件的判断不一致，逻辑就不自洽了。
        QFile::remove(stored.contentPath);
        if (error != nullptr) {
            *error = manifestError;
        }
        LOG_WARN("草稿 manifest 写失败，已删掉刚写的孤儿正文：%1", manifestError);
        return false;
    }

    return true;
}

// ============================ 查 ============================

QList<DraftRecovery::Draft> DraftRecovery::pending() const
{
    QList<Draft> result;
    const QList<Entry> entries = readManifest();

    QList<Entry> survivors;
    bool manifestDirty = false;
    for (const Entry &entry : entries) {
        // ★ 容错：正文文件被手工删掉了 → 这条不算数，顺手把 manifest 里的残项清掉。
        //   这就是"崩溃恢复模块自己的容错才是它存在的意义"（测试第 6 条）。
        if (!QFile::exists(entry.draft.contentPath)) {
            LOG_WARN("草稿正文不见了，清掉这条记录：%1", entry.draft.contentPath);
            manifestDirty = true;
            continue;
        }
        survivors << entry;
        result << entry.draft;
    }

    if (manifestDirty) {
        QString error;
        if (!writeManifest(survivors, &error)) {
            LOG_WARN("清理失效草稿记录失败（不影响恢复）：%1", error);
        }
    }

    // 新的在前：用户最可能想恢复的是刚丢掉的那一份
    std::sort(result.begin(), result.end(), [](const Draft &a, const Draft &b) {
        return a.savedAt > b.savedAt;
    });
    return result;
}

// ============================ 删 ============================

bool DraftRecovery::discard(const QString &documentPath, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    QList<Entry> entries = readManifest();
    QList<Entry> survivors;
    bool removed = false;
    for (const Entry &entry : entries) {
        if (entry.key == documentPath) {
            QFile::remove(entry.draft.contentPath);  // 正文删掉；删不掉也不阻止 manifest 更新
            removed = true;
            continue;
        }
        survivors << entry;
    }

    if (!removed) {
        return true;  // 本来就没有这份草稿：幂等，算成功
    }
    return writeManifest(survivors, error);
}

bool DraftRecovery::discardAll(QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    QDir dir(m_rootDir);
    if (dir.exists()) {
        const QFileInfoList drafts = dir.entryInfoList({QStringLiteral("*.draft")}, QDir::Files);
        for (const QFileInfo &info : drafts) {
            QFile::remove(info.absoluteFilePath());
        }
    }
    // manifest 也一并重置成空表（不是删掉文件，保持"文件一定存在且是合法 JSON"更利于排查）
    return writeManifest({}, error);
}

// ============================ 清 ============================

int DraftRecovery::pruneOlderThan(int maxAgeDays)
{
    QDir dir(m_rootDir);
    if (!dir.exists()) {
        return 0;
    }

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-qMax(0, maxAgeDays));

    QList<Entry> entries = readManifest();
    QList<Entry> survivors;
    int removed = 0;
    for (const Entry &entry : entries) {
        if (entry.draft.savedAt.isValid() && entry.draft.savedAt < cutoff) {
            QFile::remove(entry.draft.contentPath);
            ++removed;
            continue;
        }
        survivors << entry;
    }

    // 「manifest 里已不存在的正文文件也顺手清掉」：这类文件是介入不掉的残留
    //（比如手工删过 manifest、或者上次 store 的正文写成功但进程立刻被杀）。
    const auto knownPaths = [&survivors] {
        QList<QString> paths;
        for (const Entry &entry : survivors) {
            paths << QDir::cleanPath(entry.draft.contentPath);
        }
        return paths;
    }();
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.draft")}, QDir::Files);
    for (const QFileInfo &info : files) {
        if (!knownPaths.contains(QDir::cleanPath(info.absoluteFilePath()))) {
            if (QFile::remove(info.absoluteFilePath())) {
                ++removed;
                LOG_WARN("清掉没有 manifest 记录的孤儿草稿：%1", info.absoluteFilePath());
            }
        }
    }

    if (removed > 0) {
        QString error;
        if (!writeManifest(survivors, &error)) {
            LOG_WARN("prune 之后更新 manifest 失败：%1", error);
        }
    }
    return removed;
}

}  // namespace markdown_editor::core::storage
