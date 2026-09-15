#ifndef FULLTEXTSEARCH_H
#define FULLTEXTSEARCH_H

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

// 全文搜索（5.5）：基于 SQLite FTS5 的文件内容索引。
//
// 索引的粒度是**行**：一个文件的每一行是一条记录。为什么按行而不是按文件：
//   * 需求就是"列出匹配的文件和行号，点击跳到那一行" —— 行级索引让行号是天然的，
//     不需要在 C++ 里再算一遍"这个词出现在第几行"
//   * 一行一条记录也让"改了一个文件"只需重建这个文件的行，粒度小、代价低
//
// ★ 分词器为什么必须是 trigram（这一条是实测出来的，不是拍的）：
//   FTS5 默认的 unicode61 把**一整串汉字当成一个词**，所以搜"缓存"匹配不到
//   "……关键词缓存服务"（探针实测：0 行）。trigram 按 3 个字符一组切，中文子串就能搜到；
//   但它有个硬限制：**查询短于 3 个字符时 MATCH 一定没结果**（实测 2 个汉字 = 0 行）。
//   而"缓存""搜索""索引"这种二字词恰恰是中文里最常见的查询，所以：
//     - 查询 >= 3 个字符 → 走 MATCH（用 FTS5 索引，快）
//     - 查询  < 3 个字符 → 走 LIKE '%…%'（没法用索引，但结果是正确的）
//   万一这台机器的 SQLite 不带 trigram，就退回 unicode61 并**一律走 LIKE**：
//   正确性不受影响（LIKE 不依赖分词），只是慢一点。
//
// 用户输入永远当"数据"，不当"语法"：整段输入被包成一个 FTS5 短语（引号里的
// 双引号翻倍转义），所以输入 `AND` / `*` / `"` / `(` 都不会让 SQL 语法坏掉，
// 也不会被当成搜索操作符 —— 这是最容易出安全/稳定性问题的地方，专门有测试钉住。
//
// 为什么不用 infrastructure 的 DataBase 单例：DataBase 是"笔记元数据"的单例
// （一个进程一个连接，而且换库会被它忽略）。索引需要的是"每个实例一条自己的连接、
// 能指向任意库文件"（测试要对着临时库跑，同一进程里还要能开好几个）。
// 两者混在一起还会让"索引库坏了"牵连到元数据。所以这里自己开具名连接。
//
// 线程注记：QSqlDatabase 与线程绑定，本类的连接**只在创建它的线程里用**。
// 以后要把扫描放到后台线程，请在那个线程里另建一个 FullTextSearch 实例。

// 一条搜索结果 = "某个文件的某一行里有匹配"
struct SearchHit
{
    QString filePath;     // 文件绝对路径
    int line = 0;         // 行号，**1 起算**（和编辑器、状态栏的行号一致）
    QString text;         // 那一行的完整内容（太长的话由界面去省略显示）
    int matchStart = -1;  // 关键词在 text 里的起始下标（0 起算；-1 = 没能定位，比如大小写差异）
    int matchLength = 0;  // 关键词长度（给界面做高亮用）
};

// 一次索引的统计结果（给界面显示"索引了多少、跳过了多少、删掉了多少"）
struct IndexStats
{
    int filesFound = 0;    // 扫描到的可索引文件数
    int filesIndexed = 0;  // 真的重建了索引的文件数
    int filesSkipped = 0;  // 没变过、跳过的文件数
    int filesRemoved = 0;  // 索引里有、磁盘上已经没有了（或不在这次扫描范围内）的文件数
    int linesIndexed = 0;  // 这次写进索引的行数
    qint64 elapsedMs = 0;  // 总耗时（毫秒）
};

class FullTextSearch : public QObject
{
    Q_OBJECT

public:
    // 单个文件的大小上限：超过就不索引（笔记不该有这么大，而索引会明显变胖）。
    static constexpr qint64 kMaxIndexFileBytes = 4 * 1024 * 1024;

    explicit FullTextSearch(QObject *parent = nullptr);
    ~FullTextSearch() override;

    // 索引库路径。默认 <AppData>/Dev/MarkdownEditor/search-index.sqlite。
    // setIndexPath() 要在 open() 之前调（测试用它指向临时库）。
    static QString defaultIndexPath();
    void setIndexPath(const QString &path);
    QString indexPath() const;

    // 打开（不存在就创建）索引库，并建好表结构。
    // 失败：false + *error，并写一条 LOG_ERROR。
    bool open(QString *error = nullptr);
    void close();
    bool isOpen() const;

    // 扫描目录下所有 Markdown 文件并建立/更新索引（递归）。
    // 没变过的文件（修改时间 + 大小都没变）会跳过，所以反复点"建立索引"很快。
    // 语义是"目录的快照"：索引里那些**不在本次扫描结果里**的文件会被清掉
    //   （包括被删掉的、被改名的、以及不属于这个目录的）。
    IndexStats indexDirectory(const QString &dir, QString *error = nullptr);

    // 搜索。结果按「文件 → 行号」排序（同一文件里的匹配聚在一起，界面直接分组显示）。
    // limit 是结果条数上限（0 或负数 = 空结果）；超限时只返回前 limit 条。
    QList<SearchHit> search(const QString &query, int limit = 500, QString *error = nullptr) const;

    // 索引现状（界面显示"已索引 N 个文件 / M 行"）
    int indexedFileCount() const;
    int indexedLineCount() const;
    QStringList indexedFiles() const;

    // 清空索引（文件本身一个字节都不动，只是忘掉索引）
    bool clearIndex(QString *error = nullptr);

    // ============================ 纯函数（能脱离数据库和界面单独测）============================

    // 用户输入 → 真正用来搜的关键词：去掉首尾空白。
    // 空字符串 = "不搜"（调用方据此直接返回空结果，不要拿空串去查库）。
    static QString normalizeQuery(const QString &raw);

    // 这个关键词是不是必须退回 LIKE（< 3 个字符，trigram 的 MATCH 对它无能为力）
    static bool needsLikeFallback(const QString &query);

    // 关键词 → FTS5 短语表达式（整段当一个子串搜；内部的双引号翻倍转义）。
    static QString toMatchExpression(const QString &query);

    // 关键词 → LIKE 模式（%…%，转义 % _ \ 三个特殊字符）。
    static QString toLikePattern(const QString &query);

    // 这个文件名该不该进索引（.md / .markdown，大小写不敏感）
    static bool isIndexableFile(const QString &fileName);

    // 在某一行的文本里找关键词，返回起始下标（不区分大小写），找不到返回 -1。
    // 界面用它做高亮；数据库那边只负责"这一行里有匹配"。
    static int findMatch(const QString &line, const QString &query);

signals:
    // 索引进度：done / total（按文件数）。扫描大目录时界面靠它显示进度。
    void indexProgress(int done, int total);
    // 一次索引结束（参数就是 indexDirectory 的返回值）
    void indexFinished(const IndexStats &stats);
    // 出错了（原因已经写进日志）。界面可以拿它去更新状态栏。
    void errorOccurred(const QString &message);

private:
    // 确保表结构存在，并判断这台机器的 FTS5 有没有 trigram（没有就一律走 LIKE）。
    bool ensureSchema(QString *error);

    // 索引里记着的文件状态：修改时间 + 大小（和 CacheManager 的"新鲜度"是同一个思路）
    struct KnownFile
    {
        qint64 mtime = 0;
        qint64 size = 0;
    };

    QSqlDatabase m_db;
    QString m_indexPath;
    QString m_connectionName;

    bool m_open = false;
    // FTS5 的 MATCH 能不能用来搜（= 表是按 trigram 建的）。false 时一律走 LIKE。
    bool m_matchUsable = false;
};

// 让 IndexStats 能安全地穿过信号槽（直接连接其实不需要，但排队连接/未来放别的线程就需要）
Q_DECLARE_METATYPE(IndexStats)

#endif // FULLTEXTSEARCH_H
