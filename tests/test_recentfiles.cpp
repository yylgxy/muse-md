// RecentFiles（5.4.2 最近文件）+ ConfigManager（配置层）的契约测试。
//
// 需要 QCoreApplication（RecentFiles 是 QObject，要发 changed() 信号）。**不需要窗口**。
//
// 这个测试盯住三件事：
//   1. **列表规则**（纯函数）：去重、置顶、截断到 10 条、空路径不算数。
//      这类规则手写起来"看着简单"，但边界不少（同一文件两种写法、超过 10 条、
//      重复点同一个文件），所以拆成静态函数单独钉住。
//   2. **配置真的落盘了**：不是"内存里对"，而是关掉再打开（模拟重启在配置这一层的样子）
//      还能读回来 —— 这正是"重启后最近文件还在"的核心。
//   3. **测试不碰用户配置**：ConfigManager::setFilePath() 指到临时目录里的 config.ini，
//      用户真实的 <AppData>/Dev/MarkdownEditor/config.ini 一个字节都不动。
//
// 跑法：ctest -C Debug --output-on-failure

#include "configmanager.h"
#include "recentfiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-62s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// 造一个真实存在的空文件（fileExists 和"能不能加进列表"都要求文件真的在）
bool touch(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write("x");
    file.close();
    return true;
}

QString join(const QStringList &list)
{
    return list.join(QStringLiteral(" | "));
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 全程用临时目录 + 临时配置文件
    const QString base = QDir::tempPath() + QStringLiteral("/md-editor-recentfiles-test");
    QDir(base).removeRecursively();
    if (!QDir().mkpath(base)) {
        std::printf("无法建立临时目录：%s\n", base.toUtf8().constData());
        return 1;
    }
    const QString configPath = base + QStringLiteral("/config.ini");
    ConfigManager::setFilePath(configPath);

    // ============================ A. 路径规范化（纯函数）============================
    {
        std::printf("---- A. 路径规范化 ----\n");
        check(RecentFiles::normalize(QStringLiteral("D:\\Docs\\Note.MD")) == QStringLiteral("d:/docs/note.md"),
              QStringLiteral("normalize: 反斜杠转正斜杠 + 全小写"),
              RecentFiles::normalize(QStringLiteral("D:\\Docs\\Note.MD")));
        check(RecentFiles::normalize(QStringLiteral("d:/Docs/note.md/")) == QStringLiteral("d:/docs/note.md"),
              QStringLiteral("normalize: 去掉结尾的分隔符"));
        check(RecentFiles::normalize(QStringLiteral("  d:/docs/note.md  ")) == QStringLiteral("d:/docs/note.md"),
              QStringLiteral("normalize: 去掉首尾空白"));
        check(RecentFiles::normalize(QStringLiteral("/")) == QStringLiteral("/"),
              QStringLiteral("normalize: 根目录只留一个斜杠（不会切空）"));
        check(RecentFiles::storageKey() == QStringLiteral("recentFiles"),
              QStringLiteral("storageKey: 配置里的键名固定为 recentFiles"),
              RecentFiles::storageKey());
        check(RecentFiles::kMaxCount == 10, QStringLiteral("kMaxCount: 最多记 10 条"));
    }

    // ============================ B. 列表规则（纯函数）============================
    {
        std::printf("---- B. 列表规则 updatedList() ----\n");
        const QStringList empty;

        check(RecentFiles::updatedList(empty, QStringLiteral("a.md")) == QStringList{QStringLiteral("a.md")},
              QStringLiteral("空列表 + 一个路径 → 只有它一条"));

        const QStringList two{QStringLiteral("b.md"), QStringLiteral("c.md")};
        const QStringList afterA = RecentFiles::updatedList(two, QStringLiteral("a.md"));
        check(afterA == QStringList({QStringLiteral("a.md"), QStringLiteral("b.md"), QStringLiteral("c.md")}),
              QStringLiteral("新路径插到最前面，旧的顺序不变"), join(afterA));

        const QStringList dup = RecentFiles::updatedList(two, QStringLiteral("c.md"));
        check(dup == QStringList({QStringLiteral("c.md"), QStringLiteral("b.md")}),
              QStringLiteral("重复的置顶而不是复制一份（去重）"), join(dup));

        // 同一个文件两种写法：必须算同一条，而且留下的是**新的那种写法**
        const QStringList mixed = RecentFiles::updatedList(QStringList{QStringLiteral("d:/docs/a.md")},
                                                          QStringLiteral("D:\\Docs\\A.MD"));
        check(mixed.size() == 1 && mixed.first() == QStringLiteral("D:\\Docs\\A.MD"),
              QStringLiteral("大小写/分隔符不同也算同一个文件"), join(mixed));

        check(RecentFiles::updatedList(two, QString()) == two, QStringLiteral("空路径：原样返回（不算打开过）"));
        check(RecentFiles::updatedList(two, QStringLiteral("   ")) == two, QStringLiteral("只有空白的路径：同样忽略"));
        check(RecentFiles::updatedList(two, QStringLiteral("a.md"), 0) == two,
              QStringLiteral("上限 <= 0：原样返回（不崩、不清空）"));

        QStringList longList;
        for (int i = 0; i < 30; ++i) {
            longList << QStringLiteral("old%1.md").arg(i);
        }
        const QStringList capped = RecentFiles::updatedList(longList, QStringLiteral("new.md"), 10);
        check(capped.size() == 10, QStringLiteral("截断: 超过上限时只留 10 条"), QString::number(capped.size()));
        check(capped.first() == QStringLiteral("new.md") && capped.contains(QStringLiteral("new.md")),
              QStringLiteral("截断: 新的那条一定在里面且在首位"));
        check(capped.last() == QStringLiteral("old8.md"),
              QStringLiteral("截断：被丢掉的是最老的那些（old9 之后全没了）"), capped.last());
    }

    // ============================ C. 文件是否存在 ============================
    {
        std::printf("---- C. fileExists() ----\n");
        const QString realFile = base + QStringLiteral("/exists.md");
        check(touch(realFile), QStringLiteral("准备: 造一个真实文件"), realFile);
        check(RecentFiles::fileExists(realFile), QStringLiteral("fileExists: 真实文件 → true"));
        check(!RecentFiles::fileExists(base), QStringLiteral("fileExists: 目录不算文件 → false"));
        check(!RecentFiles::fileExists(base + QStringLiteral("/nope.md")),
              QStringLiteral("fileExists: 不存在的路径 → false"));
    }

    // ============================ D. ConfigManager ============================
    {
        std::printf("---- D. ConfigManager（QSettings + ini）----\n");
        check(ConfigManager::filePath() == configPath,
              QStringLiteral("setFilePath: 生效了（filePath 返回测试路径）"), ConfigManager::filePath());
        check(ConfigManager::filePath().endsWith(QStringLiteral("config.ini")),
              QStringLiteral("默认路径以 config.ini 结尾"));

        ConfigManager::setValue(QStringLiteral("windowWidth"), 1200);
        check(ConfigManager::contains(QStringLiteral("windowWidth")), QStringLiteral("setValue: 写进去之后 contains = true"));
        check(ConfigManager::value(QStringLiteral("windowWidth")).toInt() == 1200, QStringLiteral("value: 读回来还是 1200"));

        check(ConfigManager::value(QStringLiteral("neverSet"), QStringLiteral("兜底")).toString()
                  == QStringLiteral("兜底"),
              QStringLiteral("value: 没写过的键返回给定的默认值"));

        const QStringList list{QStringLiteral("第一条"), QStringLiteral("D:/两 个字.md")};
        ConfigManager::setStringList(QStringLiteral("demoList"), list);
        check(ConfigManager::stringList(QStringLiteral("demoList")) == list,
              QStringLiteral("stringList: 中文和空格都能原样读回"),
              join(ConfigManager::stringList(QStringLiteral("demoList"))));

        ConfigManager::setStringList(QStringLiteral("demoList"), QStringList());
        check(!ConfigManager::contains(QStringLiteral("demoList")),
              QStringLiteral("setStringList(空): 键被删掉，配置里不留空壳"));

        ConfigManager::remove(QStringLiteral("windowWidth"));
        check(!ConfigManager::contains(QStringLiteral("windowWidth")), QStringLiteral("remove: 键没了"));

        // 真的落盘了：直接读文件内容（不是"内存里对"）
        ConfigManager::setValue(QStringLiteral("probe"), QStringLiteral("alpha.md"));
        ConfigManager::sync();
        QFile raw(configPath);
        const bool opened = raw.open(QIODevice::ReadOnly);
        const QString text = opened ? QString::fromUtf8(raw.readAll()) : QString();
        if (opened) {
            raw.close();
        }
        check(opened && QFileInfo::exists(configPath), QStringLiteral("落盘: 配置文件真的被创建了"), configPath);
        check(text.contains(QStringLiteral("probe")) && text.contains(QStringLiteral("alpha.md")),
              QStringLiteral("落盘: 文件里能看到刚写的键值"));

        // "重新打开配置"：换一次路径会丢掉旧的 QSettings 实例（析构时同步落盘），
        // 再读就是从头加载 —— 等价于用户重启程序后第一次读配置。
        ConfigManager::setFilePath(configPath);
        check(ConfigManager::value(QStringLiteral("probe")).toString() == QStringLiteral("alpha.md"),
              QStringLiteral("重新打开配置后，值还能读回来（可持久化）"));
        ConfigManager::remove(QStringLiteral("probe"));
    }

    // ============================ E. RecentFiles 的行为 ============================
    {
        std::printf("---- E. RecentFiles ----\n");
        const QString p1 = base + QStringLiteral("/first.md");
        const QString p2 = base + QStringLiteral("/second.md");
        touch(p1);
        touch(p2);

        RecentFiles recent;
        int changes = 0;
        QObject::connect(&recent, &RecentFiles::changed, [&changes] { ++changes; });

        check(recent.isEmpty() && recent.count() == 0 && recent.files().isEmpty(),
              QStringLiteral("初始: 还没 load 就是空列表"));

        recent.load();
        check(recent.isEmpty() && changes == 1,
              QStringLiteral("load: 配置里没有这个键 → 空列表，并发了 changed()"),
              QStringLiteral("changes=%1").arg(changes));

        recent.add(p1);
        check(recent.count() == 1 && recent.files().first() == p1,
              QStringLiteral("add: 第一条进列表"), join(recent.files()));
        check(changes == 2, QStringLiteral("add: 列表变了要发 changed()（菜单靠它重建）"),
              QStringLiteral("changes=%1").arg(changes));
        check(ConfigManager::stringList(RecentFiles::storageKey()).size() == 1,
              QStringLiteral("add: 同一时刻配置里也写进去了（不用等退出）"));

        recent.add(p2);
        check(recent.files() == QStringList({p2, p1}), QStringLiteral("add: 最新的排最前"), join(recent.files()));

        recent.add(p2);
        check(recent.count() == 2 && changes == 3,
              QStringLiteral("add 重复项: 列表不变、也不白发信号"), QStringLiteral("changes=%1").arg(changes));

        recent.add(QString());
        check(recent.count() == 2 && changes == 3, QStringLiteral("add 空路径: 忽略（配置和信号都不动）"));

        check(recent.contains(p1), QStringLiteral("contains: 在列表里 → true"));
        // p1.toUpper() 是全大写的另一种写法：normalize() 之后应该和 p1 是同一条
        check(recent.contains(p1.toUpper()),
              QStringLiteral("contains: 大小写写法不同也算同一个文件"), p1.toUpper());
        check(!recent.contains(base + QStringLiteral("/never-added.md")),
              QStringLiteral("contains: 没加过的 → false"));

        // 超过 10 条
        for (int i = 0; i < 15; ++i) {
            const QString extra = base + QStringLiteral("/extra%1.md").arg(i);
            touch(extra);
            recent.add(extra);
        }
        check(recent.count() == RecentFiles::kMaxCount,
              QStringLiteral("上限: 一直加也只留 10 条"), QString::number(recent.count()));
        check(recent.files().first() == base + QStringLiteral("/extra14.md"),
              QStringLiteral("上限: 最后加的那条在最前面"), recent.files().first());
        check(!recent.contains(p1) && !recent.contains(p2),
              QStringLiteral("上限: 最早的两条被挤掉了"));

        recent.remove(recent.files().first());
        check(recent.count() == 9, QStringLiteral("remove: 去掉一条，数量 -1"), QString::number(recent.count()));

        const int beforeRemoveMissing = changes;
        recent.remove(base + QStringLiteral("/根本没这个文件.md"));
        check(changes == beforeRemoveMissing && recent.count() == 9,
              QStringLiteral("remove 不存在的: 什么都不做"));

        recent.clear();
        check(recent.isEmpty() && !ConfigManager::contains(RecentFiles::storageKey()),
              QStringLiteral("clear: 列表空、配置里的键也删掉了"));
        const int afterClear = changes;
        recent.clear();
        check(changes == afterClear, QStringLiteral("clear 空的列表: 不再发信号"));

        // ---- 模拟一次"重启" ----
        // 存两条（顺序：p2 在前），关掉配置再重新打开，用一个全新的 RecentFiles 读回来 ——
        // 这就是"上次打开过的文件，重启后还在菜单里"在数据层的样子。
        recent.add(p1);
        recent.add(p2);
        check(recent.files() == QStringList({p2, p1}), QStringLiteral("重启前: 列表是 p2, p1"), join(recent.files()));

        ConfigManager::setFilePath(configPath);  // 关掉旧 QSettings（析构时落盘）再重新打开
        RecentFiles reopened;
        reopened.load();
        check(reopened.files().size() == 2, QStringLiteral("重启后: 条数一样"), QString::number(reopened.files().size()));
        check(reopened.files() == QStringList({p2, p1}),
              QStringLiteral("重启后: **顺序也一模一样**（最新的仍在最前）"), join(reopened.files()));
        check(reopened.contains(p2), QStringLiteral("重启后: contains 仍然成立"));

        // 上限也要跨重启成立（配置里存的本来就只有 10 条）
        check(ConfigManager::stringList(RecentFiles::storageKey()).size() <= RecentFiles::kMaxCount,
              QStringLiteral("配置里存的条数不会超过 10"));

        reopened.clear();
        RecentFiles afterClearReload;
        afterClearReload.load();
        check(afterClearReload.isEmpty(), QStringLiteral("清除之后再重启: 还是空的"));
    }

    // 收尾：别在临时目录里留东西
    QDir(base).removeRecursively();
    ConfigManager::setFilePath(QString());

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
