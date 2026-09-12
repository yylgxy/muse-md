#include "database.h"

#include "logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

namespace {

// 具名连接：不用 Qt 的默认连接（默认连接会被别处 addDatabase 悄悄覆盖）
const char *const kConnectionName = "markdown_editor_db";

}  // namespace

DataBase &DataBase::getInstance()
{
    // C++11 起，函数内静态局部变量的初始化是线程安全的
    static DataBase instance;
    return instance;
}

DataBase::DataBase() = default;

bool DataBase::openDB(const QString &dbPath)
{
    if (m_isOpen) {
        return true;
    }

    // 先给一句人话：缺少 SQLite 驱动插件时，open() 只会报 "Driver not loaded"，
    // 那句提示很难懂（真正的原因是运行目录下没有 plugins/sqldrivers/qsqlite.dll）
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        LOG_ERROR("数据库驱动 QSQLITE 不可用（发布时记得带上 plugins/sqldrivers/qsqlite.dll）");
        return false;
    }

    // SQLite 会自己创建数据库文件，但**不会创建父目录**，所以要先把目录建出来
    const QString dir = QFileInfo(dbPath).absolutePath();
    if (!dir.isEmpty() && !QDir().mkpath(dir)) {
        LOG_ERROR("无法创建数据库目录: %1", dir);
        return false;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                     QString::fromLatin1(kConnectionName));
    m_db.setDatabaseName(dbPath);

    if (!m_db.open()) {
        LOG_ERROR("数据库打开失败: %1 (%2)", dbPath, m_db.lastError().text());
        closeDB();
        return false;
    }
    m_isOpen = true;

    // 建表失败不能"半开"：必须回滚成关闭状态。
    // 否则调用方拿到 false 却以为连接还在，后续每条 SQL 都会以奇怪的方式失败。
    if (!initTable()) {
        LOG_ERROR("建表失败, 数据库已关闭: %1", dbPath);
        closeDB();
        return false;
    }

    LOG_INFO("数据库已打开: %1", dbPath);
    return true;
}

void DataBase::closeDB()
{
    const QString connectionName = m_db.connectionName();

    if (m_db.isOpen()) {
        m_db.close();
    }
    m_isOpen = false;

    // 顺序很重要：必须先把 QSqlDatabase 副本清空，再 removeDatabase。
    // 否则 Qt 会警告 "connection is still in use"，而且连接不会被真正释放
    // （下次 addDatabase 同名连接时旧对象会失效）。
    m_db = QSqlDatabase();
    if (!connectionName.isEmpty()) {
        QSqlDatabase::removeDatabase(connectionName);
    }
}

bool DataBase::execSQL(const QString &sql)
{
    if (!m_isOpen) {
        LOG_WARN("数据库未打开, 忽略了这条 SQL: %1", sql);
        return false;
    }

    QSqlQuery query(m_db);
    if (!query.exec(sql)) {
        LOG_ERROR("SQL 执行失败: %1 | SQL: %2", query.lastError().text(), sql);
        return false;
    }
    return true;
}

QList<QVariantMap> DataBase::querySQL(const QString &sql)
{
    QList<QVariantMap> result;

    if (!m_isOpen) {
        LOG_WARN("数据库未打开, 忽略了这次查询: %1", sql);
        return result;
    }

    QSqlQuery query(m_db);
    if (!query.exec(sql)) {
        LOG_ERROR("查询失败: %1 | SQL: %2", query.lastError().text(), sql);
        return result;
    }

    // record() 对每一行都一样，提到循环外面取一次就够
    const QSqlRecord record = query.record();
    const int columnCount = record.count();

    while (query.next()) {
        QVariantMap row;
        for (int i = 0; i < columnCount; ++i) {
            row.insert(record.fieldName(i), query.value(i));
        }
        result.append(row);
    }
    return result;
}

bool DataBase::initTable()
{
    // filePath 设 UNIQUE：同一个文件在表里只能有一条记录
    return execSQL(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS note_meta (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            filePath   TEXT UNIQUE NOT NULL,
            title      TEXT,
            tags       TEXT,
            createTime INTEGER,
            modifyTime INTEGER
        )
    )"));
}

bool DataBase::addNoteMeta(const NoteMeta &meta)
{
    if (!m_isOpen) {
        LOG_WARN("数据库未打开, 无法插入笔记: %1", meta.filePath);
        return false;
    }

    // ⚠ 这里必须用参数绑定，不能把值拼进 SQL 字符串。三个理由：
    //   1) 标题/路径里出现单引号（例如 "It's"）会把 SQL 语法直接弄坏
    //   2) QString::arg 会把你值里的 "%1" 当成占位符替换掉（这个坑更隐蔽）
    //   3) 恶意构造的路径/标题可以注入 SQL，例如  a'); DROP TABLE note_meta;--
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT INTO note_meta(filePath, title, tags, createTime, modifyTime) "
                                 "VALUES(:filePath, :title, :tags, :createTime, :modifyTime)"));
    query.bindValue(QStringLiteral(":filePath"), meta.filePath);
    query.bindValue(QStringLiteral(":title"), meta.title);
    query.bindValue(QStringLiteral(":tags"), meta.tags);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    query.bindValue(QStringLiteral(":createTime"), meta.createTime != 0 ? meta.createTime : now);
    query.bindValue(QStringLiteral(":modifyTime"), meta.modifyTime != 0 ? meta.modifyTime : now);

    if (!query.exec()) {
        LOG_ERROR("插入笔记失败: %1 | filePath=%2", query.lastError().text(), meta.filePath);
        return false;
    }
    return true;
}

bool DataBase::updateNoteMeta(const NoteMeta &meta)
{
    if (!m_isOpen) {
        LOG_WARN("数据库未打开, 无法更新笔记: %1", meta.filePath);
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE note_meta SET title = :title, tags = :tags, "
                                 "modifyTime = :modifyTime WHERE filePath = :filePath"));
    query.bindValue(QStringLiteral(":title"), meta.title);
    query.bindValue(QStringLiteral(":tags"), meta.tags);
    query.bindValue(QStringLiteral(":modifyTime"),
                    meta.modifyTime != 0 ? meta.modifyTime : QDateTime::currentMSecsSinceEpoch());
    query.bindValue(QStringLiteral(":filePath"), meta.filePath);

    if (!query.exec()) {
        LOG_ERROR("更新笔记失败: %1 | filePath=%2", query.lastError().text(), meta.filePath);
        return false;
    }

    // 路径不存在时 exec() 也算"成功"（只是影响 0 行），所以要看影响行数，
    // 否则调用方会以为更新成功了
    if (query.numRowsAffected() == 0) {
        LOG_WARN("更新笔记: 表里没有这个路径: %1", meta.filePath);
        return false;
    }
    return true;
}

bool DataBase::deleteNoteByPath(const QString &filePath)
{
    if (!m_isOpen) {
        LOG_WARN("数据库未打开, 无法删除笔记: %1", filePath);
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM note_meta WHERE filePath = :filePath"));
    query.bindValue(QStringLiteral(":filePath"), filePath);

    if (!query.exec()) {
        LOG_ERROR("删除笔记失败: %1 | filePath=%2", query.lastError().text(), filePath);
        return false;
    }

    if (query.numRowsAffected() == 0) {
        LOG_WARN("删除笔记: 表里没有这个路径: %1", filePath);
        return false;
    }
    return true;
}

QList<NoteMeta> DataBase::selectAllNotes()
{
    QList<NoteMeta> list;

    // 明确列出列名（不写 SELECT *）：以后表里加列时，这里的行为不会悄悄变化
    const QList<QVariantMap> rows = querySQL(
        QStringLiteral("SELECT id, filePath, title, tags, createTime, modifyTime "
                       "FROM note_meta ORDER BY modifyTime DESC"));

    for (const QVariantMap &row : rows) {
        NoteMeta item;
        item.id = row.value(QStringLiteral("id")).toInt();
        item.filePath = row.value(QStringLiteral("filePath")).toString();
        item.title = row.value(QStringLiteral("title")).toString();
        item.tags = row.value(QStringLiteral("tags")).toString();
        item.createTime = row.value(QStringLiteral("createTime")).toLongLong();
        item.modifyTime = row.value(QStringLiteral("modifyTime")).toLongLong();
        list.append(item);
    }
    return list;
}
