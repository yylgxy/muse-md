// SessionState（6.2 窗口记忆）的契约测试。
//
// 需要 QCoreApplication 就够了：这个类只搬数据（配置 ←→ 结构体），不认识 QWidget。
// 正因为如此它才能被自动测 —— 主窗口本身没法在测试里实例化（里面有 QWebEngineView，
// 构造就要拉起 Chromium），"窗口记忆"这种东西如果写在主窗口里，就只能靠人肉重启验证。
//
// 测四件事：
//   1. 存进去再读出来，一模一样（几何、文件列表、索引、面板可见性）；
//   2. 配置里什么都没有时是安全默认值（首次启动不能崩、也不能瞎恢复）；
//   3. usableFiles() 的规则：去空、去重（大小写不敏感）、保序；
//   4. C2 的光标/滚动位置编解码：格式、排序稳定、坏数据容错
//      —— 坏配置必须只被丢掉，不能让窗口起不来。
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
        check(state.viewState.isEmpty(), QStringLiteral("首次: 没有记过任何光标位置（C2）"));
        check(!state.outlinePanelVisible, QStringLiteral("首次: 大纲面板默认隐藏（C4）"));
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
        saved.outlinePanelVisible = true;  // C4
        // C2：两个文件的"上次看到哪"
        SessionState::setViewState(&saved.viewState, base + QStringLiteral("/a.md"), 42, 7, 900);
        SessionState::setViewState(&saved.viewState, base + QStringLiteral("/b.md"), 1, 1, 0);
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
        check(loaded.outlinePanelVisible, QStringLiteral("往返: 大纲面板可见性不变（C4）"));

        // ---- C2：光标/滚动位置往返 ----
        check(loaded.viewState.size() == 2, QStringLiteral("往返(C2): 记了 2 个文件的位置"),
              QStringLiteral("%1 条").arg(loaded.viewState.size()));
        int line = 0;
        int column = 0;
        int scroll = 0;
        const bool parsed = SessionState::parseViewState(
            loaded.viewState.value(base + QStringLiteral("/a.md")), &line, &column, &scroll);
        check(parsed && line == 42 && column == 7 && scroll == 900,
              QStringLiteral("往返(C2): a.md 的行/列/滚动值一字不差"),
              QStringLiteral("行 %1 列 %2 滚动 %3").arg(line).arg(column).arg(scroll));

        // ★ 键名大小写不该影响：Windows 上同一路径的大小写变体是同一个文件
        //   （这里只断言"key 是原样存的"，大小写归一由调用方保证 —— 主窗口用
        //    FileManager::filePath()，那是绝对的规范化路径）
        check(loaded.viewState.contains(base + QStringLiteral("/b.md")),
              QStringLiteral("往返(C2): b.md 那条也在"));

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
        check(SessionState::viewStateKey() == QStringLiteral("session/viewState"),
              QStringLiteral("键名: 光标位置（C2）"));
        // 键名会被写进用户目录下的配置文件：改一次键名 = 所有人的上次会话丢失。
        // 所以键名也当契约钉住（新增字段只能加新键，不能改老键）。
        check(SessionState::outlinePanelVisibleKey() == QStringLiteral("session/outlinePanelVisible"),
              QStringLiteral("键名: 大纲面板可见性（C4）"));
    }

    // ============================ F. C2：光标位置的编解码 ============================
    //
    // 这一段全是纯函数，是 C2 里真正容易出错的部分（格式、坏数据、排序稳定性）。
    {
        std::printf("---- F. 光标位置编解码（C2）----\n");

        // ---- 单个值的格式 ----
        check(SessionState::makeViewState(42, 7, 900) == QStringLiteral("42|7|900"),
              QStringLiteral("C2: makeViewState 的格式是「行|列|滚动」"),
              SessionState::makeViewState(42, 7, 900));
        // 负数夹成 0：光标和滚动条都不该出现负值，但存进去之前必须先夹住，
        // 否则读回来会让窗口去设一个非法的光标位置
        check(SessionState::makeViewState(-5, -1, -100) == QStringLiteral("0|0|0"),
              QStringLiteral("C2: 负数一律夹成 0"));

        int line = -1;
        int column = -1;
        int scroll = -1;
        check(SessionState::parseViewState(QStringLiteral("42|7|900"), &line, &column, &scroll)
                  && line == 42 && column == 7 && scroll == 900,
              QStringLiteral("C2: parseViewState 能读回三个数"));
        check(SessionState::parseViewState(QStringLiteral("42|7"), &line, &column, &scroll) == false,
              QStringLiteral("C2: 字段数不对 -> false（当作没记过）"));
        check(SessionState::parseViewState(QStringLiteral("a|b|c"), &line, &column, &scroll) == false,
              QStringLiteral("C2: 不是数字 -> false"));
        check(SessionState::parseViewState(QString(), &line, &column, &scroll) == false,
              QStringLiteral("C2: 空串 -> false"));
        check(SessionState::parseViewState(QStringLiteral("-9|-9|-9"), &line, &column, &scroll)
                  && line == 0 && column == 0 && scroll == 0,
              QStringLiteral("C2: 手改配置写成了负数 -> 夹到 0，不崩"));

        // ---- 整表的编解码 ----
        QHash<QString, QString> table;
        SessionState::setViewState(&table, base + QStringLiteral("/z.md"), 3, 1, 10);
        SessionState::setViewState(&table, base + QStringLiteral("/a.md"), 9, 2, 20);
        SessionState::setViewState(&table, QString(), 1, 1, 0);          // 空路径 -> 不记
        SessionState::setViewState(&table, QStringLiteral("   "), 1, 1, 0);  // 全空白 -> 不记
        check(table.size() == 2, QStringLiteral("C2: 空路径/空白路径不进表"),
              QStringLiteral("%1 条").arg(table.size()));

        const QStringList encoded = SessionState::encodeViewState(table);
        check(encoded.size() == 2, QStringLiteral("C2: 编码出 2 行"));
        // ★ 排序稳定：不排的话 QHash 每次遍历顺序都不一样，配置文件会一直"看起来变了"
        check(encoded.size() == 2
                  && encoded.at(0).startsWith(base + QStringLiteral("/a.md") + QLatin1Char('\t'))
                  && encoded.at(1).startsWith(base + QStringLiteral("/z.md") + QLatin1Char('\t')),
              QStringLiteral("C2: 编码结果按路径排序（配置文件的 diff 才稳定）"),
              encoded.join(QStringLiteral(" ; ")));

        const QStringList twice = SessionState::encodeViewState(table);
        check(twice == encoded, QStringLiteral("C2: 同一份表编码两次结果完全相同"));

        const QHash<QString, QString> decoded = SessionState::decodeViewState(encoded);
        check(decoded == table, QStringLiteral("C2: 编码再解码 = 原表（往返）"));

        // ---- 坏数据容错 ----
        const QStringList dirty{
            QStringLiteral("没有分隔符的一行"),                                  // 坏行
            QStringLiteral("\t只有值没有路径"),                                   // 路径为空
            base + QStringLiteral("/good.md") + QLatin1Char('\t') + QStringLiteral("5|6|7"),
            base + QStringLiteral("/bad.md") + QLatin1Char('\t') + QStringLiteral("这不是数字"),
        };
        const QHash<QString, QString> recovered = SessionState::decodeViewState(dirty);
        check(recovered.size() == 1, QStringLiteral("C2: 坏行被丢掉，好的那条留下"),
              QStringLiteral("%1 条").arg(recovered.size()));
        check(recovered.contains(base + QStringLiteral("/good.md")),
              QStringLiteral("C2: 坏数据不影响别的条目"));
    }

    // ============================ G. C2：坏配置不该让窗口崩 ============================
    {
        std::printf("---- G. 坏配置的容错（C2）----\n");

        SessionState::Data broken;
        broken.openFiles = QStringList{base + QStringLiteral("/c.md")};
        SessionState::save(broken);
        ConfigManager::setFilePath(configPath);
        // 直接往配置里写一段坏的光标记录
        ConfigManager::setStringList(SessionState::viewStateKey(),
                                     QStringList{QStringLiteral("乱写的一行"),
                                                 base + QStringLiteral("/c.md") + QLatin1Char('\t')
                                                     + QStringLiteral("x|y|z")});
        ConfigManager::sync();

        const SessionState::Data loaded = SessionState::load();
        check(loaded.viewState.isEmpty(),
              QStringLiteral("C2: 配置文件被写坏 -> 读回来是空表（不抛、不崩、不影响别的键）"),
              QStringLiteral("%1 条").arg(loaded.viewState.size()));
        check(loaded.openFiles.size() == 1,
              QStringLiteral("C2: 坏的光标记录不影响文件列表"));
    }

    QDir(base).removeRecursively();
    ConfigManager::setFilePath(QString());

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
