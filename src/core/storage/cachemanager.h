#ifndef CACHEMANAGER_H
#define CACHEMANAGER_H

#include <QCache>
#include <QDateTime>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include "textencoding.h"

namespace markdown_editor::core::storage {

// 缓存服务（4.2.3 基础版）：把最近打开过的文件内容留在内存里，重复打开就不必再读盘。
//
// ---- 关于 QCache，两个实测出来的事实（别照文档猜）----
//   1. 它**按访问刷新顺序**：object(key) 命中的那一条会被移到"最近使用"的一端，
//      所以它确实是 LRU，而不是按插入顺序淘汰。
//      （Qt 文档只说了"淘汰最久未使用的"，实际上有些容器是插入顺序，所以这里专门验证过：
//        容量 2，插入 A、B，访问 A，再插入 C → 被淘汰的是 B，A 活了下来。）
//   2. 它的 maxCost 是"代价预算"，**不是条数**。本类让每条记录的代价恒为 1，
//      于是 setMaxEntries(n) 就等于 setMaxCost(n)，语义变成"最多缓存 n 个文件"。
//      这样做的目的很实在：避免"代价"和"条数"两套计数各说各话、淘汰行为看不懂。
//
// ---- 为什么除了条数还要限制单条大小 ----
// 条数限制挡不住"一个 300MB 的文件进缓存"。所以再加一道 maxEntryBytes：
// 超过这个大小的文件直接不进缓存（记一笔 rejections）。两头都约束了，才算真的"避免内存溢出"。
//
// ---- 过期判断：比 LRU 更要紧的是正确性 ----
// 缓存里存的是"内容快照"，而磁盘上的文件可能被别的程序改过（编辑器、同步盘、脚本……）。
// 所以每条记录都带**缓存时刻的修改时间 + 字节数**，查找时先比这两项：
// 不一致就返回 Stale，调用方必须重新读盘。
// 少了这一步，用户会拿着内存里的旧内容去覆盖别人刚写的新内容 —— 缓存反倒成了丢数据的帮凶。
//
// 分层：本类不认识 MarkdownDocument，也不认识 FileManager，只是"路径 → 文件内容"的缓存。
class CacheManager
{
public:
    // 默认最多缓存多少个文件
    static constexpr int kDefaultMaxEntries = 20;
    // 默认单条上限 1 MiB（Markdown 文档极少超过这个量级）
    static constexpr qint64 kDefaultMaxEntryBytes = 1024 * 1024;

    // 一条缓存记录：内容 + 编码 + 缓存时刻的磁盘状态（后两项用于判断是否过期）
    struct Entry
    {
        QString text;                        // 已解码的文本
        Encoding encoding = Encoding::Utf8;  // 用哪种编码解码的（保存时要按它写回去）
        QDateTime lastModified;              // 缓存时刻磁盘上文件的修改时间
        qint64 size = 0;                     // 缓存时刻磁盘上文件的字节数
    };

    // 查找结果。
    // 把"没有"和"有但过期"分开，是因为调用方后续动作不同，
    // 而且这两件事都值得各记一笔统计（否则永远不知道缓存为什么没起作用）。
    enum class LookupResult { Hit, Stale, Miss };

    explicit CacheManager(int maxEntries = kDefaultMaxEntries);

    // ============================ 容量 ============================

    int maxEntries() const;

    // 最多缓存多少个文件。传 <= 0 表示**关掉缓存**：插入一律被拒（记入 rejections）。
    void setMaxEntries(int count);

    qint64 maxEntryBytes() const;

    // 单条内容超过这个字节数就不进缓存。传 <= 0 表示不限制单条大小。
    void setMaxEntryBytes(qint64 bytes);

    // ============================ 查 / 存 / 删 ============================

    // 查找：命中并且**没有过期**才返回 Hit（并填充 entry）。
    // currentFile 由调用方查一次 QFileInfo 得到（调用方本来就知道路径，这里不重复查磁盘）。
    // 返回 Stale 表示缓存里有，但磁盘上的文件已经变了（修改时间或大小对不上）——
    // 此时 entry 不会被填充，调用方应当重新读盘并覆盖缓存。
    LookupResult lookup(const QString &path, const QFileInfo &currentFile, Entry *entry) const;

    // 只看缓存里有没有这个路径，**不判断是否过期**（给调试/测试用，业务逻辑请用 lookup）。
    bool contains(const QString &path) const;

    // 存入（同一路径会覆盖旧记录）。
    // 返回 false 表示没有被缓存：缓存被关掉、或者内容超过 maxEntryBytes。
    // 超出容量时 QCache 会自动淘汰最久未使用的那条。
    bool insert(const QString &path, const Entry &entry);

    void remove(const QString &path);
    void clear();

    // ============================ 状态与统计 ============================

    int size() const;          // 当前缓存了几条
    QStringList keys() const;  // 当前缓存了哪些路径（顺序不保证，调试/测试用）

    // 统计：用来验证"第二次打开真的走了缓存"。
    // lookup() 是 const，但统计本质是观测，所以这些计数器用 mutable —— 不改变"查找不修改缓存内容"的语义。
    qint64 hits() const;
    qint64 misses() const;
    qint64 staleCount() const;
    qint64 insertions() const;
    qint64 rejections() const;  // 因为缓存被关掉 / 单条太大而没进缓存的次数
    qint64 evictions() const;   // 粗略计数：插入后条数没增加（说明挤掉了别人）
    void resetStatistics();

    QString statisticsText() const;  // 一行可读统计，直接进日志

    // 缓存 key 的规范化：Windows 上 D:\a\B.md 与 d:/a/b.md 是**同一个文件**，
    // 必须算同一把 key，否则同一个文件会有两份缓存、命中率莫名地低。
    // 本类的 insert/lookup/contains/remove 内部都会先做这一步，调用方不用自己转。
    static QString normalizeKey(const QString &path);

private:
    // 缓存是否可用（容量 > 0）
    bool cacheEnabled() const;

    QCache<QString, Entry> m_cache;  // 注意：QCache 持有 Entry*，淘汰/清空时会 delete 它们

    qint64 m_maxEntryBytes = kDefaultMaxEntryBytes;

    mutable qint64 m_hits = 0;
    mutable qint64 m_misses = 0;
    mutable qint64 m_stale = 0;
    qint64 m_insertions = 0;
    qint64 m_rejections = 0;
    qint64 m_evictions = 0;
};

}  // namespace markdown_editor::core::storage

#endif // CACHEMANAGER_H
