#include "cachemanager.h"

#include <QDir>
#include <QStringList>

namespace markdown_editor::core::storage {

CacheManager::CacheManager(int maxEntries)
{
    // 代价固定 1（见头文件说明）：maxCost 直接就是"最多几个文件"
    m_cache.setMaxCost(maxEntries > 0 ? maxEntries : 0);
}

// ============================ 容量 ============================

bool CacheManager::cacheEnabled() const
{
    return m_cache.maxCost() > 0;
}

int CacheManager::maxEntries() const
{
    return m_cache.maxCost();
}

void CacheManager::setMaxEntries(int count)
{
    // QCache::setMaxCost 调小的时候会**立刻**把多出来的记录淘汰掉，不用我们手工清理
    m_cache.setMaxCost(count > 0 ? count : 0);
}

qint64 CacheManager::maxEntryBytes() const
{
    return m_maxEntryBytes;
}

void CacheManager::setMaxEntryBytes(qint64 bytes)
{
    m_maxEntryBytes = bytes;
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
        ++m_rejections;  // 缓存被关掉（maxEntries <= 0）
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

    // cost = 1：于是 QCache 的代价预算就等于"最多几个文件"
    if (!m_cache.insert(key, new Entry(entry), 1)) {
        ++m_rejections;  // 理论上进不来（代价 1 <= maxCost），留个兜底
        return false;
    }

    ++m_insertions;
    if (!replacing && m_cache.size() == sizeBefore) {
        ++m_evictions;  // 条数没增加 → 说明挤掉了最久未使用的那条（粗略计数）
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

void CacheManager::resetStatistics()
{
    m_hits = 0;
    m_misses = 0;
    m_stale = 0;
    m_insertions = 0;
    m_rejections = 0;
    m_evictions = 0;
}

QString CacheManager::statisticsText() const
{
    const qint64 totalLookups = m_hits + m_misses + m_stale;
    const double hitRate = totalLookups > 0 ? (100.0 * double(m_hits) / double(totalLookups)) : 0.0;

    return QStringLiteral("缓存 %1/%2 条；命中 %3、过期 %4、未命中 %5（命中率 %6%）；写入 %7 次、淘汰 %8 条、跳过 %9 条")
        .arg(m_cache.size())
        .arg(m_cache.maxCost())
        .arg(m_hits)
        .arg(m_stale)
        .arg(m_misses)
        .arg(hitRate, 0, 'f', 1)
        .arg(m_insertions)
        .arg(m_evictions)
        .arg(m_rejections);
}

}  // namespace markdown_editor::core::storage
