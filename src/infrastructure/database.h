#ifndef DATABASE_H
#define DATABASE_H

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariant>

// 笔记元数据（后面业务层做索引/最近文件列表用）。
// 时间统一用**毫秒**时间戳：QDateTime::currentMSecsSinceEpoch()
struct NoteMeta
{
    int id = 0;
    QString filePath;       // md 文件路径；表里是 UNIQUE，相当于一条笔记的身份
    QString title;          // 笔记标题
    QString tags;           // 标签，逗号分隔
    qint64 createTime = 0;  // 创建时间（毫秒）
    qint64 modifyTime = 0;  // 修改时间（毫秒）
};

// SQLite 数据库封装（单例）。
//
// 三条设计说明：
//   1. 单例：一个进程一个连接就够。用 C++11 的"函数内静态局部变量"实现，
//      初始化天然线程安全（不需要自己加锁）。
//   2. 使用**具名连接**，不占用 Qt 的默认连接 —— 默认连接会被别人 addDatabase
//      时悄悄覆盖，是多人协作时的经典坑。
//   3. QSqlDatabase 与线程绑定：**连接只能在创建它的那个线程里使用**。
//      以后要在后台线程扫描目录，请在那个线程里另开一个连接，不要共用这个单例。
//
// 典型用法：
//     auto &db = DataBase::getInstance();
//     if (!db.openDB(路径)) { /* 失败原因已写进日志 */ }
//     ...
//     db.closeDB();     // 退出前关闭
class DataBase
{
public:
    // 取单例
    static DataBase &getInstance();

    // 打开数据库文件（SQLite）。
    // 会：① 检查 SQLite 驱动是否可用 ② 自动创建不存在的父目录 ③ 建表。
    // 成功：true；失败：false，并写 LOG_ERROR 说明原因。已经打开时直接返回 true。
    bool openDB(const QString &dbPath);

    // 关闭连接，并把它从 Qt 的连接池里移除（之后可以重新 openDB）。
    // 注意：调用前不要持有未销毁的 QSqlQuery / QSqlDatabase 副本。
    void closeDB();

    bool isOpen() const { return m_isOpen; }

    // 执行没有结果集的 SQL（建表 / 插入 / 更新 / 删除）。
    // 失败：false 并写 LOG_ERROR（含 SQL 和数据库给的原因）。
    // ⚠ 不要把用户数据用字符串拼进 SQL：值里出现单引号（例如 "It's"）会让 SQL 语法坏掉，
    //   而且有注入风险。带参数一律用下面的 addNoteMeta/updateNoteMeta，
    //   或者自己 QSqlQuery::prepare() + bindValue()。
    bool execSQL(const QString &sql);

    // 查询，返回每一行（列名 -> 值）。
    // 失败或数据库未打开：返回空列表（并写日志，别把"空结果"和"失败"混起来用）。
    QList<QVariantMap> querySQL(const QString &sql);

    // 建表（openDB 会自动调用，一般不用自己调）
    bool initTable();

    // ---------------- 笔记元数据增删改查 ----------------
    // 插入一条。filePath 是 UNIQUE，所以同一路径重复插入会失败（false）。
    // meta.createTime / modifyTime 传 0 时，会自动填当前时间（毫秒）。
    bool addNoteMeta(const NoteMeta &meta);

    // 按 filePath 更新 title / tags / modifyTime。
    // 路径不存在时返回 false（不会静默地"当作成功"）。
    bool updateNoteMeta(const NoteMeta &meta);

    // 按 filePath 删除。路径不存在时返回 false。
    bool deleteNoteByPath(const QString &filePath);

    // 查出全部笔记元数据，按 modifyTime 从新到旧排列（"最近在改的排前面"）
    QList<NoteMeta> selectAllNotes();

private:
    // 私有构造 + 禁止拷贝/移动：单例不该被复制
    DataBase();
    DataBase(const DataBase &) = delete;
    DataBase &operator=(const DataBase &) = delete;
    DataBase(DataBase &&) = delete;
    DataBase &operator=(DataBase &&) = delete;

    QSqlDatabase m_db;
    bool m_isOpen = false;
};

#endif // DATABASE_H
