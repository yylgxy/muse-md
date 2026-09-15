#include "fulltextsearch.h"

#include "filemanager.h"  // 复用它的编码检测/解码/二进制判断：索引和编辑器必须"看到同一份文本"
#include "fileutils.h"
#include "logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace {

// 编码检测/解码/二进制判断都复用 FileManager 的静态函数：
// 索引里的文本必须和用户在编辑器里看到的完全一致，否则"明明看见有这个词却搜不到"
// 会变成一个说不清的问题。所以两条路用同一套判断，而不是各写一份。
using markdown_editor::core::storage::FileManager;

// 每个实例一条具名连接：QSqlDatabase 的连接名在进程内必须唯一，重名会被 Qt 忽略并告警。
// 用自增编号而不是"按库路径命名"：同一个库文件在同一进程里也可能被两个实例先后打开
// （测试就是这么干的），编号能保证名字不撞。
QString nextConnectionName()
{
    static int counter = 0;
    return QStringLiteral("markdown_editor_fts_%1").arg(++counter);
}

}  // namespace

FullTextSearch::FullTextSearch(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<IndexStats>("IndexStats");
}

FullTextSearch::~FullTextSearch()
{
    close();
}

// ============================ 库与表结构 ============================

QString FullTextSearch::defaultIndexPath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        // 极端情况兜底：至少保证索引有地方放（和 ConfigManager 的处理一致）
        base = QCoreApplication::applicationDirPath();
    }
    return base + QStringLiteral("/search-index.sqlite");
}

void FullTextSearch::setIndexPath(const QString &path)
{
    if (m_open) {
        LOG_WARN("索引库已经打开了，setIndexPath() 不会生效（要换库请先 close()）：%1", path);
        return;
    }
    m_indexPath = path;
}

QString FullTextSearch::indexPath() const
{
    return m_indexPath.isEmpty() ? defaultIndexPath() : m_indexPath;
}

bool FullTextSearch::open(QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (m_open) {
        return true;
    }

    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        LOG_ERROR("%1", why);
        return false;
    };

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return fail(QStringLiteral("这台机器的 Qt 没带 SQLite 驱动（QSQLITE），全文搜索用不了"));
    }

    const QString path = indexPath();
    const QString dir = QFileInfo(path).absolutePath();
    if (!dir.isEmpty() && !QDir().mkpath(dir)) {
        return fail(QStringLiteral("建不了索引库所在目录：%1").arg(dir));
    }

    m_connectionName = nextConnectionName();
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        const QString why = m_db.lastError().text();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        return fail(QStringLiteral("打开索引库失败（%1）：%2").arg(path, why));
    }

    m_open = true;
    if (!ensureSchema(error)) {
        close();
        return false;
    }

    LOG_INFO("全文索引库已打开: %1（分词器 %2）", path, m_matchUsable ? QStringLiteral("trigram") : QStringLiteral("unicode61/LIKE"));
    return true;
}

void FullTextSearch::close()
{
    const QString name = m_connectionName;
    m_open = false;
    m_matchUsable = false;

    if (m_db.isOpen()) {
        m_db.close();
    }
    // 必须先把副本丢掉再 removeDatabase，否则 Qt 会警告"连接还在被使用"
    m_db = QSqlDatabase();
    if (!name.isEmpty()) {
        QSqlDatabase::removeDatabase(name);
    }
    m_connectionName.clear();
}

bool FullTextSearch::isOpen() const
{
    return m_open;
}

bool FullTextSearch::ensureSchema(QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        LOG_ERROR("%1", why);
        return false;
    };

    QSqlQuery query(m_db);

    // 元数据表：索引里有哪些文件、它们当时的磁盘状态（用来判断"这个文件变过没有"）
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS indexed_files ("
                                   "path TEXT PRIMARY KEY, "
                                   "mtime INTEGER NOT NULL, "
                                   "size INTEGER NOT NULL, "
                                   "lines INTEGER NOT NULL)"))) {
        return fail(QStringLiteral("建 indexed_files 表失败：%1").arg(query.lastError().text()));
    }

    // 正文表：FTS5 虚表，一行一条记录（见头文件里"为什么按行"的说明）。
    // path / line_no 特意标成 UNINDEXED：它们只随行返回，不参与匹配 ——
    // 否则"搜某个词"会把文件路径里的字也算成命中。
    const QString trigramSql = QStringLiteral("CREATE VIRTUAL TABLE IF NOT EXISTS doc_lines USING fts5("
                                              "path UNINDEXED, line_no UNINDEXED, text, "
                                              "tokenize = 'trigram')");
    if (!query.exec(trigramSql)) {
        // 老 SQLite 没有 trigram（3.34 才有）。退回 unicode61 并**一律走 LIKE**：
        // 中文子串搜索依然是对的（LIKE 不依赖分词），只是没法用 FTS 索引加速。
        LOG_WARN("FTS5 trigram 分词器不可用（%1），退回 unicode61，搜索将一律走 LIKE",
                 query.lastError().text());
        const QString fallbackSql = QStringLiteral("CREATE VIRTUAL TABLE IF NOT EXISTS doc_lines USING fts5("
                                                   "path UNINDEXED, line_no UNINDEXED, text, "
                                                   "tokenize = 'unicode61')");
        if (!query.exec(fallbackSql)) {
            return fail(QStringLiteral("建 FTS5 全文索引表失败（这台机器的 SQLite 可能没编译 FTS5）：%1")
                            .arg(query.lastError().text()));
        }
    }

    // 表到底建在哪个分词器上，以**库里的实际定义**为准（已存在的库也要能正确判断）
    QSqlQuery probe(m_db);
    if (probe.exec(QStringLiteral("SELECT sql FROM sqlite_master WHERE name = 'doc_lines'")) && probe.next()) {
        m_matchUsable = probe.value(0).toString().contains(QLatin1String("trigram"), Qt::CaseInsensitive);
    } else {
        m_matchUsable = false;
    }
    return true;
}

// ============================ 建立索引 ============================

IndexStats FullTextSearch::indexDirectory(const QString &dir, QString *error)
{
    IndexStats stats;
    if (error != nullptr) {
        error->clear();
    }

    QElapsedTimer timer;
    timer.start();

    const auto fail = [this, error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        LOG_ERROR("%1", why);
        emit errorOccurred(why);
    };

    if (!m_open) {
        fail(QStringLiteral("索引库还没打开，没法建立索引"));
        return stats;
    }
    if (dir.isEmpty() || !QDir(dir).exists()) {
        fail(QStringLiteral("要索引的目录不存在：%1").arg(dir));
        return stats;
    }

    // ---- 1. 找出这个目录下所有 Markdown 文件（递归）----
    QStringList found;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (isIndexableFile(QFileInfo(path).fileName())) {
            found << path;
        }
    }
    found.sort(Qt::CaseInsensitive);  // 顺序固定：日志、统计、测试都好看
    stats.filesFound = found.size();

    // ---- 2. 读一遍索引里已有的文件状态（一次查完，别在循环里逐条查）----
    QHash<QString, KnownFile> known;
    {
        QSqlQuery query(m_db);
        if (query.exec(QStringLiteral("SELECT path, mtime, size FROM indexed_files"))) {
            while (query.next()) {
                KnownFile entry;
                entry.mtime = query.value(1).toLongLong();
                entry.size = query.value(2).toLongLong();
                known.insert(query.value(0).toString(), entry);
            }
        } else {
            LOG_WARN("读 indexed_files 失败（权当索引是空的）：%1", query.lastError().text());
        }
    }

    // ---- 3. 一个事务里逐个文件处理：一次事务比"每行一次提交"快一个数量级 ----
    const bool inTransaction = m_db.transaction();
    if (!inTransaction) {
        LOG_WARN("开启事务失败（会退化成逐条提交，慢一些）：%1", m_db.lastError().text());
    }

    QSqlQuery dropLines(m_db);
    QSqlQuery insertLine(m_db);
    QSqlQuery dropMeta(m_db);
    QSqlQuery insertMeta(m_db);
    dropLines.prepare(QStringLiteral("DELETE FROM doc_lines WHERE path = ?"));
    insertLine.prepare(QStringLiteral("INSERT INTO doc_lines(path, line_no, text) VALUES (?, ?, ?)"));
    dropMeta.prepare(QStringLiteral("DELETE FROM indexed_files WHERE path = ?"));
    insertMeta.prepare(QStringLiteral("INSERT INTO indexed_files(path, mtime, size, lines) VALUES (?, ?, ?, ?)"));

    int done = 0;
    for (const QString &path : found) {
        const QFileInfo info(path);
        const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
        const qint64 size = info.size();

        // 这个文件"跳过"时的统一处理：记一条日志、算进统计、并把它从"待删除"名单里摘掉
        //（它还在磁盘上，只是这次不重建索引）
        const auto skipFile = [&](const QString &reason) {
            LOG_WARN("索引时跳过 %1（%2）", path, reason);
            ++stats.filesSkipped;
            known.remove(path);
            emit indexProgress(++done, stats.filesFound);
        };

        const auto knownIt = known.find(path);
        if (knownIt != known.end() && knownIt->mtime == mtime && knownIt->size == size) {
            ++stats.filesSkipped;
            known.erase(knownIt);  // 它还在，别当成"已经被删掉"
            emit indexProgress(++done, stats.filesFound);
            continue;
        }

        if (size > kMaxIndexFileBytes) {
            skipFile(QStringLiteral("超过单文件上限 %1 字节").arg(kMaxIndexFileBytes));
            continue;
        }

        QByteArray raw;
        QString readError;
        if (!FileUtils::readFileBytes(path, raw, &readError)) {
            skipFile(readError);
            continue;
        }

        // 用和编辑器**完全相同**的编码检测 + 解码：索引里的文本必须和用户在编辑器里
        // 看到的一致，否则"明明看见有这个词却搜不到"就会变成一个说不清的问题。
        const QString content = FileManager::decode(raw, FileManager::detectEncoding(raw));
        if (FileManager::looksBinary(content)) {
            skipFile(QStringLiteral("内容看起来是二进制（尽管扩展名像 Markdown）"));
            continue;
        }

        // 重建这个文件的所有行（先删后插：改名/删行/换行都能自然处理）
        dropLines.addBindValue(path);
        if (!dropLines.exec()) {
            LOG_WARN("清掉旧索引行失败：%1", dropLines.lastError().text());
        }

        const QStringList lines = content.split(QLatin1Char('\n'));
        int indexedLines = 0;
        for (int i = 0; i < lines.size(); ++i) {
            QString line = lines.at(i);
            if (line.endsWith(QLatin1Char('\r'))) {
                line.chop(1);  // CRLF 文档：行尾的 \r 不该进索引（也不该影响高亮）
            }
            if (line.trimmed().isEmpty()) {
                continue;  // 空行不占索引：省地方，而且"搜空行"本来也没意义
            }

            insertLine.addBindValue(path);
            insertLine.addBindValue(i + 1);  // 行号 **1 起算**，和编辑器/状态栏一致
            insertLine.addBindValue(line);
            if (!insertLine.exec()) {
                LOG_WARN("写索引行失败（这个文件后面的行不再索引）：%1", insertLine.lastError().text());
                break;
            }
            ++indexedLines;
        }

        dropMeta.addBindValue(path);
        dropMeta.exec();
        insertMeta.addBindValue(path);
        insertMeta.addBindValue(mtime);
        insertMeta.addBindValue(size);
        insertMeta.addBindValue(indexedLines);
        if (!insertMeta.exec()) {
            LOG_WARN("写 indexed_files 失败：%1", insertMeta.lastError().text());
        }

        stats.linesIndexed += indexedLines;
        ++stats.filesIndexed;
        known.remove(path);
        emit indexProgress(++done, stats.filesFound);
    }

    // ---- 4. 清掉"索引里有、这次没扫到"的文件 ----
    // 三种情况都会落到这里：文件被删了、被改名了、或者换成索引另一个目录了。
    // 语义是"索引 = 这个目录的快照"，所以不做例外。
    for (auto it = known.constBegin(); it != known.constEnd(); ++it) {
        dropLines.addBindValue(it.key());
        dropLines.exec();
        dropMeta.addBindValue(it.key());
        dropMeta.exec();
        ++stats.filesRemoved;
    }

    if (inTransaction && !m_db.commit()) {
        fail(QStringLiteral("提交索引事务失败（这次索引不算数）：%1").arg(m_db.lastError().text()));
        stats.elapsedMs = timer.elapsed();
        return stats;
    }

    stats.elapsedMs = timer.elapsed();
    LOG_INFO("索引完成: 扫描 %1 个文件 / 重建 %2 / 跳过 %3 / 清理 %4 / 写入 %5 行，耗时 %6 ms",
             stats.filesFound,
             stats.filesIndexed,
             stats.filesSkipped,
             stats.filesRemoved,
             stats.linesIndexed,
             stats.elapsedMs);
    emit indexFinished(stats);
    return stats;
}

// ============================ 搜索 ============================

QList<SearchHit> FullTextSearch::search(const QString &query, int limit, QString *error) const
{
    QList<SearchHit> hits;
    if (error != nullptr) {
        error->clear();
    }

    const QString keyword = normalizeQuery(query);
    if (keyword.isEmpty() || limit <= 0) {
        return hits;  // 空关键词/无效上限：直接给空结果，不要拿空串去查库
    }
    if (!m_open) {
        if (error != nullptr) {
            *error = QStringLiteral("索引库还没打开，搜不了");
        }
        return hits;
    }

    // >= 3 个字符走 FTS5 的 MATCH（有索引，快）；< 3 个字符只能 LIKE（trigram 的硬限制）。
    // 表不是按 trigram 建的（老 SQLite）时，也一律走 LIKE。
    const bool useLike = !m_matchUsable || needsLikeFallback(keyword);

    QSqlQuery statement(m_db);
    const QString sql = useLike
                            ? QStringLiteral("SELECT path, line_no, text FROM doc_lines "
                                             "WHERE text LIKE ? ESCAPE '\\' ORDER BY path, line_no LIMIT ?")
                            : QStringLiteral("SELECT path, line_no, text FROM doc_lines "
                                             "WHERE text MATCH ? ORDER BY path, line_no LIMIT ?");
    if (!statement.prepare(sql)) {
        if (error != nullptr) {
            *error = QStringLiteral("搜索语句准备失败：%1").arg(statement.lastError().text());
        }
        LOG_ERROR("搜索语句准备失败: %1", statement.lastError().text());
        return hits;
    }

    statement.addBindValue(useLike ? toLikePattern(keyword) : toMatchExpression(keyword));
    statement.addBindValue(limit);

    if (!statement.exec()) {
        if (error != nullptr) {
            *error = QStringLiteral("搜索失败：%1").arg(statement.lastError().text());
        }
        LOG_ERROR("搜索失败（关键词 %1）: %2", keyword, statement.lastError().text());
        return hits;
    }

    while (statement.next()) {
        SearchHit hit;
        hit.filePath = statement.value(0).toString();
        hit.line = statement.value(1).toInt();
        hit.text = statement.value(2).toString();
        hit.matchStart = findMatch(hit.text, keyword);
        hit.matchLength = (hit.matchStart >= 0) ? keyword.size() : 0;
        hits.append(hit);
    }

    LOG_INFO("搜索 %1: 命中 %2 条（%3）", keyword, hits.size(), useLike ? QStringLiteral("LIKE") : QStringLiteral("MATCH"));
    return hits;
}

// ============================ 索引现状 ============================

int FullTextSearch::indexedFileCount() const
{
    if (!m_open) {
        return 0;
    }
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM indexed_files")) && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

int FullTextSearch::indexedLineCount() const
{
    if (!m_open) {
        return 0;
    }
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM doc_lines")) && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

QStringList FullTextSearch::indexedFiles() const
{
    QStringList files;
    if (!m_open) {
        return files;
    }
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT path FROM indexed_files ORDER BY path"))) {
        while (query.next()) {
            files << query.value(0).toString();
        }
    }
    return files;
}

bool FullTextSearch::clearIndex(QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_open) {
        if (error != nullptr) {
            *error = QStringLiteral("索引库还没打开");
        }
        return false;
    }

    QSqlQuery query(m_db);
    const bool ok = query.exec(QStringLiteral("DELETE FROM doc_lines")) && query.exec(QStringLiteral("DELETE FROM indexed_files"));
    if (!ok) {
        if (error != nullptr) {
            *error = QStringLiteral("清空索引失败：%1").arg(query.lastError().text());
        }
        LOG_ERROR("清空索引失败: %1", query.lastError().text());
        return false;
    }
    LOG_INFO("索引已清空");
    return true;
}

// ============================ 纯函数 ============================

QString FullTextSearch::normalizeQuery(const QString &raw)
{
    return raw.trimmed();
}

bool FullTextSearch::needsLikeFallback(const QString &query)
{
    // trigram 按 3 个字符一组切，所以**短于 3 个字符的查询用 MATCH 一定搜不到**
    // （实测：中文"缓存"= 0 行，"缓存服"= 1 行）。这种情况必须退回 LIKE。
    return normalizeQuery(query).size() < 3;
}

QString FullTextSearch::toMatchExpression(const QString &query)
{
    QString escaped = normalizeQuery(query);
    // 整段输入被包成一个 FTS5 短语：这样用户输的 AND / OR / * / ( / NEAR 都只是普通字符，
    // 不会变成搜索语法（既避免语法错误，也避免"输入被当成命令执行"这类问题）。
    // 短语内部的双引号要翻倍，否则会把短语提前结束掉。
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString FullTextSearch::toLikePattern(const QString &query)
{
    QString escaped = normalizeQuery(query);
    // LIKE 的三个特殊字符：转义符自己、% 和 _（都按 "\" 当转义符，SQL 里写了 ESCAPE '\'）
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return QLatin1Char('%') + escaped + QLatin1Char('%');
}

bool FullTextSearch::isIndexableFile(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    return suffix == QLatin1String("md") || suffix == QLatin1String("markdown");
}

int FullTextSearch::findMatch(const QString &line, const QString &query)
{
    const QString keyword = normalizeQuery(query);
    if (keyword.isEmpty()) {
        return -1;
    }
    return line.indexOf(keyword, 0, Qt::CaseInsensitive);
}
