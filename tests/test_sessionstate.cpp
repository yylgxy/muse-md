// SessionState（6.2 窗口记忆）的契约测试。
//
// 需要 QCoreApplication 就够了：这个类只搬数据（配置 ←→ 结构体），不认识 QWidget。
// 正因为如此它才能被自动测 —— 主窗口本身没法在测试里实例化（里面有 QWebEngineView，
// 构造就要拉起 Chromium），"窗口记忆"这种东西如果写在主窗口里，就只能靠人肉重启验证。
//
// 测三件事：
//   1. 存进去再读出来，一模一样（几何、文件列表、索引、面板可见性）；
//   2. 配置里什么都没有时是安全默认值（首次启动不能崩、也不能瞎恢复）；
//   3. usableFiles() 的规则：去空、去重（大小写不敏感）、保序。
//
// 配置指向临时文件，绝不碰用户真实的 config.ini。
//
// 跑法：ctest -C Debug --output-on-failure

#include "configmanager.h"
#include "sessionstate.h"

#include <QCoreApplication>
#include <QDir>
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

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString base = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-session-test"));
    QDir(base).removeRecursively();
    QDir().mkpath(base);
    const QString configPath = base + QStringLiteral("/config.ini");
    ConfigManager::setFilePath(configPath);

    // ============================ A. 首次启动（配置里什么都没有）============================
    {
        std::printf("---- A. 首次启动 ----\n");

        const SessionState::Data state = SessionState::load();
        check(state.geometry.isEmpty(), QStringLiteral("首次: 没有几何存档（窗口该用默认大小）"));
        check(state.openFiles.isEmpty(), QStringLiteral("首次: 没有上次打开的文件"));
        check(state.currentIndex == 0, QStringLiteral("首次: 当前索引是 0"));
        check(state.fileTreeVisible, QStringLiteral("首次: 文件树默认可见"));
        check(!state.searchPanelVisible, QStringLiteral("首次: 搜索面板默认隐藏"));
    }

    // ============================ B. 存了再读（往返）============================
    {
        std::printf("---- B. 存进去再读出来 ----\n");

        SessionState::Data saved;
        saved.geometry = QByteArrayLiteral("\x01\x00\x00\x00\xd9\xd0\xcb\x9d");  // 随便一段"不透明字节"
        saved.openFiles = QStringList{base + QStringLiteral("/a.md"), base + QStringLiteral("/b.md")};
        saved.currentIndex = 1;
        saved.fileTreeVisible = false;
        saved.searchPanelVisible = true;
        SessionState::save(saved);

        // 换一次配置路径 = 丢掉内存里的 QSettings、下次读是真的从文件读
        ConfigManager::setFilePath(configPath);
        const SessionState::Data loaded = SessionState::load();

        check(loaded.geometry == saved.geometry, QStringLiteral("往返: 几何字节一字不差"),
              QStringLiteral("%1 字节").arg(loaded.geometry.size()));
        check(loaded.openFiles == saved.openFiles, QStringLiteral("往返: 文件列表不变（含顺序）"),
              loaded.openFiles.join(QStringLiteral(", ")));
        check(loaded.currentIndex == 1, QStringLiteral("往返: 当前索引不变"));
        check(!loaded.fileTreeVisible, QStringLiteral("往返: 文件树可见性不变（这次是隐藏）"));
        check(loaded.searchPanelVisible, QStringLiteral("往返: 搜索面板可见性不变（这次是显示）"));

        // 真的落到文件里了（不是"内存里对"）
        check(QFile::exists(configPath), QStringLiteral("往返: 配置文件真的写出来了"));
    }

    // ============================ C. 空值的处理 ============================
    {
        std::printf("---- C. 空值 / 清空 ----\n");

        SessionState::Data empty;
        empty.geometry = QByteArray();          // 没存过几何
        empty.openFiles = QStringList();        // 一个文件都没有
        SessionState::save(empty);

        ConfigManager::setFilePath(configPath);
        const SessionState::Data loaded = SessionState::load();
        check(loaded.geometry.isEmpty(), QStringLiteral("清空: 几何存档被删掉了（不留空壳）"));
        check(loaded.openFiles.isEmpty(), QStringLiteral("清空: 文件列表为空"));
        check(!ConfigManager::contains(SessionState::openFilesKey()),
              QStringLiteral("清空: 配置里没有留下空的 openFiles 键"));
        check(!ConfigManager::contains(SessionState::geometryKey()),
              QStringLiteral("清空: 配置里没有留下空的 geometry 键"));
    }

    // ============================ D. usableFiles 的规则 ============================
    {
        std::printf("---- D. 文件列表的规则 ----\n");

        const QStringList raw = {base + QStringLiteral("/a.md"),
                                 QString(),                                  // 空路径（没保存过的新标签）
                                 QStringLiteral("   "),                     // 只有空白
                                 base + QStringLiteral("/A.MD"),             // 同一个文件的大写写法
                                 base + QStringLiteral("/b.md"),
                                 base + QStringLiteral("/a.md")};            // 重复
        const QStringList cleaned = SessionState::usableFiles(raw);
        check(cleaned.size() == 2, QStringLiteral("规则: 去空 + 去重（大小写不敏感）之后只剩 2 个"),
              QStringLiteral("%1 个").arg(cleaned.size()));
        check(cleaned.first() == base + QStringLiteral("/a.md"),
              QStringLiteral("规则: 保留第一次出现的那个写法（顺序不变）"), cleaned.join(QStringLiteral(", ")));
        check(cleaned.last() == base + QStringLiteral("/b.md"), QStringLiteral("规则: 第二个是 b.md"));

        check(SessionState::usableFiles(QStringList()).isEmpty(), QStringLiteral("规则: 空列表 -> 空列表"));
        check(SessionState::usableFiles(QStringList{QString(), QStringLiteral("  ")}).isEmpty(),
              QStringLiteral("规则: 全是空字符串 -> 空列表"));

        // 存的时候也要走同一套规则（否则配置里会留一个空字符串）
        SessionState::Data noisy;
        noisy.openFiles = QStringList{QString(), base + QStringLiteral("/c.md"), base + QStringLiteral("/C.md")};
        SessionState::save(noisy);
        ConfigManager::setFilePath(configPath);
        check(SessionState::load().openFiles == QStringList{base + QStringLiteral("/c.md")},
              QStringLiteral("规则: 存进去时也做了清理（配置里不会留空串/重复）"),
              SessionState::load().openFiles.join(QStringLiteral(", ")));
    }

    // ============================ E. 键名稳定 ============================
    {
        std::printf("---- E. 配置键名 ----\n");

        // 键名是对外契约：手改配置的人、以及"上一个版本的配置"都靠它。
        check(SessionState::geometryKey() == QStringLiteral("window/geometry"),
              QStringLiteral("键名: 几何"), SessionState::geometryKey());
        check(SessionState::openFilesKey() == QStringLiteral("session/openFiles"),
              QStringLiteral("键名: 文件列表"), SessionState::openFilesKey());
        check(SessionState::currentIndexKey() == QStringLiteral("session/currentIndex"),
              QStringLiteral("键名: 当前索引"));
        check(SessionState::fileTreeVisibleKey() == QStringLiteral("session/fileTreeVisible"),
              QStringLiteral("键名: 文件树可见性"));
        check(SessionState::searchPanelVisibleKey() == QStringLiteral("session/searchPanelVisible"),
              QStringLiteral("键名: 搜索面板可见性"));
    }

    QDir(base).removeRecursively();
    ConfigManager::setFilePath(QString());

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
