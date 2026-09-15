#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <QString>
#include <QStringList>
#include <QVariant>

class QSettings;

// 配置管理（全部为静态函数，不需要实例化）。
//
// 存储用的是 Qt 自带的 **QSettings + IniFormat**：配置文件就是一份
// <AppData>/Dev/MarkdownEditor/config.ini，用户能直接用记事本看和改。
// 这里没有用 FileUtils 手写读写 —— FileUtils 是给"文档内容"用的（要保证字节级原子替换），
// 而键值配置正是 QSettings 的本职工作（它还负责转义、编码、平台差异）。
// 两者都落在同一个应用数据目录下，位置是统一的。
//
// 线程/生命周期：QSettings 实例在一个进程里复用一份（配置项不多，没必要每次开文件）。
// setFilePath() 是给测试用的 —— 让测试把配置指向临时文件，不碰用户真实的配置。
class ConfigManager
{
public:
    // 配置文件路径。默认 <AppData>/Dev/MarkdownEditor/config.ini；
    // 万一拿不到应用数据目录（极端情况），退回程序所在目录。
    static QString filePath();

    // 换一个配置文件（进程内生效）。传空字符串 = 回到默认路径。
    // 测试用它指向临时文件；正常运行时不需要调用。
    static void setFilePath(const QString &path);

    static QVariant value(const QString &key, const QVariant &defaultValue = QVariant());
    static void setValue(const QString &key, const QVariant &value);
    static void remove(const QString &key);
    static bool contains(const QString &key);

    // 字符串列表的读写（最近文件用它）。
    // 写入空列表时会把这个键整个删掉，不在配置里留一个空壳。
    static QStringList stringList(const QString &key);
    static void setStringList(const QString &key, const QStringList &values);

    // 立刻落盘。一般不用手工调（QSettings 会在析构/事件循环里同步），
    // 但"关窗口前把最近文件存下来"这类时机明确的地方值得显式来一下。
    static void sync();

private:
    ConfigManager() = delete;  // 工具类不允许实例化：所有函数都是静态的，造对象没有意义

    static QSettings *settings();  // 内部：按需创建并复用
};

#endif // CONFIGMANAGER_H
