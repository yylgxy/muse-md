#include "sessionstate.h"

#include "configmanager.h"
#include "logger.h"

#include <QFileInfo>

QString SessionState::geometryKey()
{
    return QStringLiteral("window/geometry");
}

QString SessionState::openFilesKey()
{
    return QStringLiteral("session/openFiles");
}

QString SessionState::currentIndexKey()
{
    return QStringLiteral("session/currentIndex");
}

QString SessionState::fileTreeVisibleKey()
{
    return QStringLiteral("session/fileTreeVisible");
}

QString SessionState::searchPanelVisibleKey()
{
    return QStringLiteral("session/searchPanelVisible");
}

SessionState::Data SessionState::load()
{
    Data data;
    data.geometry = ConfigManager::value(geometryKey()).toByteArray();
    data.openFiles = usableFiles(ConfigManager::stringList(openFilesKey()));
    data.currentIndex = ConfigManager::value(currentIndexKey(), 0).toInt();
    data.fileTreeVisible = ConfigManager::value(fileTreeVisibleKey(), true).toBool();
    data.searchPanelVisible = ConfigManager::value(searchPanelVisibleKey(), false).toBool();
    return data;
}

void SessionState::save(const Data &data)
{
    // 几何用 QByteArray 原样存：QSettings 会把它按 base64 写进 ini，
    // 既能手改（虽然没人会去改），也不会因为二进制内容把 ini 写坏。
    if (data.geometry.isEmpty()) {
        ConfigManager::remove(geometryKey());
    } else {
        ConfigManager::setValue(geometryKey(), data.geometry);
    }

    const QStringList files = usableFiles(data.openFiles);
    // setStringList 在遇到空列表时会把这个键删掉（ConfigManager 的约定），正合适
    ConfigManager::setStringList(openFilesKey(), files);
    ConfigManager::setValue(currentIndexKey(), qMax(0, data.currentIndex));
    ConfigManager::setValue(fileTreeVisibleKey(), data.fileTreeVisible);
    ConfigManager::setValue(searchPanelVisibleKey(), data.searchPanelVisible);

    // 关窗口时进程马上就要退出，不能指望"析构时自动落盘"：显式同步一次
    ConfigManager::sync();
    LOG_INFO("会话状态已保存：%1 个文件，当前第 %2 个", files.size(), data.currentIndex);
}

QStringList SessionState::usableFiles(const QStringList &paths)
{
    QStringList result;
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (trimmed.isEmpty()) {
            continue;  // 空路径（比如没保存过的新标签）不记
        }
        // 去重：同一个文件只留一次（大小写不敏感 —— Windows 上 D:\A.md 和 d:/a.md 是同一个）
        bool duplicated = false;
        for (const QString &existing : result) {
            if (QString::compare(QFileInfo(existing).absoluteFilePath(),
                                 QFileInfo(trimmed).absoluteFilePath(),
                                 Qt::CaseInsensitive)
                == 0) {
                duplicated = true;
                break;
            }
        }
        if (!duplicated) {
            result.append(trimmed);
        }
    }
    return result;
}
