#include "recentfiles.h"

#include "configmanager.h"

#include <QDir>
#include <QFileInfo>

QString RecentFiles::storageKey()
{
    return QStringLiteral("recentFiles");
}

QString RecentFiles::normalize(const QString &path)
{
    // 统一成正斜杠 + 小写，并去掉结尾的分隔符：
    // 和 CacheManager::normalizeKey() 用的是同一套规则，全项目对"同一个文件"的判断保持一致。
    QString key = QDir::fromNativeSeparators(path).trimmed();
    while (key.size() > 1 && key.endsWith(QLatin1Char('/'))) {
        key.chop(1);
    }
    return key.toLower();
}

QStringList RecentFiles::updatedList(const QStringList &current, const QString &path, int maxCount)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty() || maxCount <= 0) {
        return current;  // 空路径/无效上限：什么都不做
    }

    const QString wanted = normalize(trimmed);

    QStringList result;
    result.append(trimmed);  // 新的排最前（显示用户给的原写法，比规范化后的好看）

    for (const QString &existing : current) {
        if (result.size() >= maxCount) {
            break;
        }
        if (normalize(existing) == wanted) {
            continue;  // 同一个文件已经在最前面了，别再来一条
        }
        result.append(existing);
    }

    while (result.size() > maxCount) {
        result.removeLast();
    }
    return result;
}

bool RecentFiles::fileExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile();
}

RecentFiles::RecentFiles(QObject *parent) : QObject(parent) {}

QStringList RecentFiles::files() const
{
    return m_files;
}

int RecentFiles::count() const
{
    return m_files.size();
}

bool RecentFiles::isEmpty() const
{
    return m_files.isEmpty();
}

bool RecentFiles::contains(const QString &path) const
{
    const QString wanted = normalize(path);
    for (const QString &existing : m_files) {
        if (normalize(existing) == wanted) {
            return true;
        }
    }
    return false;
}

void RecentFiles::load()
{
    m_files = ConfigManager::stringList(storageKey());
    emit changed();
}

void RecentFiles::add(const QString &path)
{
    const QStringList updated = updatedList(m_files, path);
    if (updated == m_files) {
        return;  // 没有变化（比如重复点了同一个文件）就不白写配置、也不发信号
    }

    m_files = updated;
    save();
    emit changed();
}

void RecentFiles::remove(const QString &path)
{
    const QString wanted = normalize(path);
    QStringList kept;
    for (const QString &existing : m_files) {
        if (normalize(existing) != wanted) {
            kept.append(existing);
        }
    }
    if (kept.size() == m_files.size()) {
        return;
    }

    m_files = kept;
    save();
    emit changed();
}

void RecentFiles::clear()
{
    if (m_files.isEmpty()) {
        return;
    }
    m_files.clear();
    save();
    emit changed();
}

void RecentFiles::save() const
{
    ConfigManager::setStringList(storageKey(), m_files);
}
