#include "cachemanager.h"

#include <QDir>
#include <QStringList>

namespace markdown_editor::core::storage {

CacheManager::CacheManager(qint64 maxBytes)
{
    // 代价 = 内容字节数（见头文件 #8 说明）：maxCost 直接就是"总字节预算"
    m_cache.setMaxCost(maxBytes > 0 ? maxBytes : 0);
}

// ============================ 容量 ============================

bool CacheManager::cacheEnabled() const
{
    return m_cache.maxCost() > 0;
}

qint64 CacheManager::maxBytes() const
{
    return m_cache.maxCost();
}

void CacheManager::setMaxBytes(qint64 bytes)
{
    // QCache::setMaxCost 调小的时候会**立刻**把多出来的记录淘汰掉，不用我们手工清理
    m_cache.setMaxCost(bytes > 0 ? bytes : 0);
}

qint64 CacheManager::maxEntryBytes() const
{
    return m_maxEntryBytes;
}

void CacheManager::setMaxEntryBytes(qint64 bytes)
{
    m_maxEntryBytes = bytes;
}

qint64 CacheManager::currentBytes() const
{
    // ★ 直接用 QCache::totalCost()：它是只读的（直接返回内部 total 成员），
    //   不会像 object(key) 那样刷新 LRU 访问顺序。
    //   这里绝不能用 object(key) 去遍历累加 —— object() 会 relink（刷新 LRU），
    //   遍历一遍就等于"把所有条目都摸了一遍"，把最久未使用的那条也变成最近使用，
    //   下一次插入的淘汰顺序就全乱了（这是实测踩出来的坑，见 insert 里的注释）。
    return qint64(m_cache.totalCost());
}

// ---- 文件大小分桶（#8）----

CacheManager::Tier CacheManager::tierOf(qint64 bytes)
{
    if (bytes <= kSmallThreshold) {
        return Tier::Small;
    }
    if (bytes <= kMediumThreshold) {
        return Tier::Medium;
    }
    return Tier::Large;
}

qint64 CacheManager::entryBytes(const Entry &entry)
{
    // 用 UTF-8 字节数当代价：既接近真实内存占用（文本是缓存的大头），
    // 又和"文件大小"这一用户脑中的量级一致，淘汰行为好解释。
    return qint64(entry.text.toUtf8().size());
}

// ============================ key 规范 ============================

QString CacheManager::normalizeKey(const QString &path)
{
    // 统一成正斜杠 + 全小写：Windows 路径不区分大小写，D:\a\B.md 和 d:/a/b.md 是同一个文件，
    // 用原样字符串当 key 会让同一个文件命中不了自己的缓存。
    QString key = QDir::fromNativeSeparators(path).toLower();

    // 顺手去掉结尾多余的分隔符（"D:/a/b.md/" 这种不规范写法也当同一个文件）
    while (key.size() > 1 && key.endsWith(QLatin1Char('/'))) {
        key.chop(1);
    }
    return key;
}

// ============================ 查 / 存 / 删 ============================

CacheManager::LookupResult CacheManager::lookup(const QString &path, const QFileInfo &currentFile, Entry *entry) const
{
    // object() 命中时会把这条记录刷新成"最近使用"（QCache 的 LRU 就靠它）——
    // 所以"刚打开过的文件"下一次最不容易被淘汰，这正是我们要的行为。
    const Entry *cached = m_cache.object(normalizeKey(path));

    if (cached == nullptr) {
        ++m_misses;
        return LookupResult::Miss;
    }

    // 过期判断：修改时间 + 字节数两项都得对上。
    // 两个都看是故意的：同步盘/脚本可能改回修改时间，而大小能挡住"时间一样但内容换了"的常见情况。
    if (cached->lastModified != currentFile.lastModified() || cached->size != currentFile.size()) {
        ++m_stale;
        return LookupResult::Stale;
    }

    if (entry != nullptr) {
        *entry = *cached;  // 拷贝一份给调用方：它改自己的副本，动不到缓存里的数据
    }
    ++m_hits;
    return LookupResult::Hit;
}

bool CacheManager::contains(const QString &path) const
{
    return m_cache.contains(normalizeKey(path));
}

bool CacheManager::insert(const QString &path, const Entry &entry)
{
    if (!cacheEnabled()) {
        ++m_rejections;  // 缓存被关掉（maxBytes <= 0）
        return false;
    }

    // 单条大小限制：这里按**文件字节数**判断，而不是 QString 的内存占用。
    // 两者差一个常数倍，但"这个文件 5MB，别缓存"是用户脑子里的量级，用它更好解释。
    if (m_maxEntryBytes > 0 && entry.size > m_maxEntryBytes) {
        ++m_rejections;
        return false;
    }

    const QString key = normalizeKey(path);
    const bool replacing = m_cache.contains(key);
    const int sizeBefore = m_cache.size();
    const qint64 totalBefore = qint64(m_cache.totalCost());  // insert 前的总字节（只读，无副作用）
    const qint64 thisEntryBytes = entryBytes(entry);

    // cost = 内容字节数（#8）：于是 QCache 的代价预算 = 总字节预算，
    // 淘汰真正按内存量走 —— 一个 900KB 大文件会优先被挤出，而不是按条数误伤小文件。
    //
    // 注意：QCache::insert 在"这条记录的代价本身 > maxCost"时会直接返回 false，
    // 这就是为什么还要单条上限（maxEntryBytes）挡在前面 —— 两条约束各管一段。
    if (!m_cache.insert(key, new Entry(entry), int(thisEntryBytes))) {
        ++m_rejections;  // 代价超预算进不来（理论上已被 maxEntryBytes 挡住，留个兜底）
        return false;
    }

    ++m_insertions;
    if (!replacing && m_cache.size() == sizeBefore) {
        // 条数没增加 → 挤掉了最久未使用的那条（QCache 的 trim 在 insert 内部 delete 了它）。
        // 被挤掉那条的字节数拿不到了（已经 delete），所以用 totalCost 的差额反推：
        //   insert 前总字节 + 本条字节 - insert 后总字节 = 被淘汰释放的字节。
        ++m_evictions;
        m_evictedBytes += (totalBefore + thisEntryBytes) - qint64(m_cache.totalCost());
    }
    return true;
}

void CacheManager::remove(const QString &path)
{
    // QCache::remove 会 delete 掉记录，不用手工释放
    m_cache.remove(normalizeKey(path));
}

void CacheManager::clear()
{
    m_cache.clear();
}

// ============================ 状态与统计 ============================

int CacheManager::size() const
{
    return m_cache.size();
}

QStringList CacheManager::keys() const
{
    QStringList list;
    const QList<QString> cached = m_cache.keys();
    list.reserve(cached.size());
    for (const QString &key : cached) {
        list.append(key);
    }
    return list;
}

qint64 CacheManager::hits() const
{
    return m_hits;
}

qint64 CacheManager::misses() const
{
    return m_misses;
}

qint64 CacheManager::staleCount() const
{
    return m_stale;
}

qint64 CacheManager::insertions() const
{
    return m_insertions;
}

qint64 CacheManager::rejections() const
{
    return m_rejections;
}

qint64 CacheManager::evictions() const
{
    return m_evictions;
}

qint64 CacheManager::evictedBytes() const
{
    return m_evictedBytes;
}

qint64 CacheManager::savedReads() const
{
    return m_hits;  // 命中一次 = 省了一次磁盘读，语义化命名让面试时好讲
}

void CacheManager::resetStatistics()
{
    m_hits = 0;
    m_misses = 0;
    m_stale = 0;
    m_insertions = 0;
    m_rejections = 0;
    m_evictions = 0;
    m_evictedBytes = 0;
}

QString CacheManager::statisticsText() const
{
    const qint64 totalLookups = m_hits + m_misses + m_stale;
    const double hitRate = totalLookups > 0 ? (100.0 * double(m_hits) / double(totalLookups)) : 0.0;

    // #8：现在能报"命中率 + 省了多少次读 + 淘汰释放了多少字节"这种硬数据。
    return QStringLiteral("缓存 %1 条 / %2 MB；命中 %3、过期 %4、未命中 %5（命中率 %6%）；"
                          "写入 %7 次、淘汰 %8 条（释放 %9 KB）")
        .arg(m_cache.size())
        .arg(double(m_cache.maxCost()) / (1024.0 * 1024.0), 0, 'f', 1)
        .arg(m_hits)
        .arg(m_stale)
        .arg(m_misses)
        .arg(hitRate, 0, 'f', 1)
        .arg(m_insertions)
        .arg(m_evictions)
        .arg(double(m_evictedBytes) / 1024.0, 0, 'f', 0);
}

}  // namespace markdown_editor::core::storage
