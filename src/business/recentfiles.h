#ifndef RECENTFILES_H
#define RECENTFILES_H

#include <QObject>
#include <QString>
#include <QStringList>

// 最近打开的文件（5.4.2）。
//
// 三件事：
//   * 记住最近打开的 10 个路径，**最新的在最前面**
//   * 去重：同一个文件只留一条（包括大小写、正反斜杠写法不同的"同一个文件"）
//   * 存进配置（ConfigManager → config.ini），下次启动能读回来
//
// "去重 / 置顶 / 截断"这三条规则抽成了静态纯函数 updatedList()，
// 所以它们能脱离配置和界面单独测 —— 这类规则看着简单，边界却不少
//（重复路径、空路径、超过 10 个、同一个文件两种写法……）。
class RecentFiles : public QObject
{
    Q_OBJECT

public:
    static constexpr int kMaxCount = 10;

    // 配置里的键名
    static QString storageKey();

    // 路径规范化：去重时用它比较。
    // Windows 上 D:\a\B.md 和 d:/a/b.md 是同一个文件，必须算同一条。
    static QString normalize(const QString &path);

    // 把 path 放到 current 最前面，去掉重复的，并截断到 maxCount。
    // 空路径直接返回原列表（不算"打开过"）。
    static QStringList updatedList(const QStringList &current, const QString &path, int maxCount = kMaxCount);

    // 文件现在还在不在（菜单里标灰/点击时判断用）
    static bool fileExists(const QString &path);

    explicit RecentFiles(QObject *parent = nullptr);

    QStringList files() const;  // 最近的在前
    int count() const;
    bool isEmpty() const;
    bool contains(const QString &path) const;

    // 从配置读回列表（构造时不自动读：让调用方/测试明确决定什么时候读）
    void load();

    // 记录一次打开：置顶 + 去重 + 截断 + 存盘，然后发 changed()
    void add(const QString &path);

    // 从列表里去掉一个（比如用户点了菜单里一个已经不存在的文件）
    void remove(const QString &path);

    void clear();
    void save() const;  // 显式存盘（关窗口前那种时机明确的地方用）

signals:
    void changed();

private:
    QStringList m_files;
};

#endif // RECENTFILES_H
