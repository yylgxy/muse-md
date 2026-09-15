#include "configmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace {

// 空 = 用默认路径（见 ConfigManager::filePath()）
QString g_customFilePath;

// 进程内复用一份 QSettings。故意不回收：它活到进程结束，回收反而可能在工作线程里出问题。
QSettings *g_settings = nullptr;

QString defaultFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        // 极端情况兜底：至少保证配置有地方放
        base = QCoreApplication::applicationDirPath();
    }
    return base + QStringLiteral("/config.ini");
}

}  // namespace

QString ConfigManager::filePath()
{
    return g_customFilePath.isEmpty() ? defaultFilePath() : g_customFilePath;
}

void ConfigManager::setFilePath(const QString &path)
{
    g_customFilePath = path;
    // 换文件之后旧实例不能再用（否则会继续读写上一个文件）
    delete g_settings;
    g_settings = nullptr;
}

QSettings *ConfigManager::settings()
{
    if (g_settings == nullptr) {
        const QString path = filePath();
        // QSettings 会自己建文件，但不会建目录 —— 应用数据目录第一次用时可能还不存在
        const QString dir = QFileInfo(path).absolutePath();
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
        }
        g_settings = new QSettings(path, QSettings::IniFormat);
    }
    return g_settings;
}

QVariant ConfigManager::value(const QString &key, const QVariant &defaultValue)
{
    return settings()->value(key, defaultValue);
}

void ConfigManager::setValue(const QString &key, const QVariant &value)
{
    settings()->setValue(key, value);
}

void ConfigManager::remove(const QString &key)
{
    settings()->remove(key);
}

bool ConfigManager::contains(const QString &key)
{
    return settings()->contains(key);
}

QStringList ConfigManager::stringList(const QString &key)
{
    return settings()->value(key).toStringList();
}

void ConfigManager::setStringList(const QString &key, const QStringList &values)
{
    if (values.isEmpty()) {
        // 不留空壳：列表空了就把键删掉，配置文件里干干净净
        settings()->remove(key);
        return;
    }
    settings()->setValue(key, values);
}

void ConfigManager::sync()
{
    settings()->sync();
}
