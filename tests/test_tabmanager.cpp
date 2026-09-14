// TabManager（5.2 多标签页管理）的契约测试。
//
// 需要 QApplication（QWidget 依赖它）。
//
// 重点验三件"只读代码看不出来"的事：
//   1. **关闭确认回调**：用户点取消时必须**什么都不做**（标签还在、页面没被销毁），
//      这是防数据丢失的关键路径；测试注入一个假回调就能把这条钉死。
//   2. 关标签真的把页面释放了（用 QPointer 观察，避免"删了标签但页面泄漏"）。
//   3. 拖拽排序之后顺序真的变了，并且会发信号（主窗口靠它记顺序）。
//
// 跑法：ctest -C Debug --output-on-failure

#include "editorwidget.h"
#include "tabmanager.h"

#include <QApplication>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTabBar>

#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-58s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

TabManager::TabInfo infoFor(const QString &name, bool modified, const QString &path = QString())
{
    TabManager::TabInfo info;
    info.fileName = name;
    info.filePath = path;
    info.modified = modified;
    return info;
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ============================ A. 新建与基本状态 ============================
    {
        TabManager tabs;
        check(tabs.count() == 0 && tabs.currentEditor() == nullptr,
              QStringLiteral("初始: 没有标签，currentEditor 是 nullptr"));
        check(tabs.tabsClosable() && tabs.isMovable(),
              QStringLiteral("初始: 标签可关闭、可拖拽排序"));

        EditorWidget *first = tabs.addEditorTab();
        check(tabs.count() == 1 && first != nullptr, QStringLiteral("新建标签: 数量 +1 并返回编辑器"));
        check(tabs.tabText(0) == QStringLiteral("未命名"), QStringLiteral("新建标签: 默认标题是「未命名」"),
              tabs.tabText(0));
        check(tabs.currentEditor() == first, QStringLiteral("新建标签: 立刻成为当前标签"));
        check(tabs.editorAt(0) == first && tabs.indexOf(first) == 0, QStringLiteral("新建标签: editorAt/indexOf 一致"));

        EditorWidget *second = tabs.addEditorTab(QStringLiteral("笔记.md"));
        check(tabs.count() == 2, QStringLiteral("新建标签: 再来一个"));
        check(tabs.tabText(1) == QStringLiteral("笔记.md"), QStringLiteral("新建标签: 标题按传入的名字"),
              tabs.tabText(1));
        check(tabs.currentIndex() == 1 && tabs.currentEditor() == second,
              QStringLiteral("新建标签: 新标签自动切过去"));
        check(second != first, QStringLiteral("新建标签: 两个标签是不同的编辑器对象"));

        check(tabs.editorAt(-1) == nullptr && tabs.editorAt(99) == nullptr,
              QStringLiteral("越界访问: editorAt 返回 nullptr（不崩）"));
        check(tabs.indexOf(nullptr) == -1, QStringLiteral("越界访问: indexOf(nullptr) = -1"));
    }

    // ============================ B. 标签标题与修改标记 ============================
    {
        TabManager tabs;
        EditorWidget *editor = tabs.addEditorTab();

        tabs.updateTab(0, infoFor(QStringLiteral("笔记.md"), false, QStringLiteral("D:/notes/笔记.md")));
        check(tabs.tabText(0) == QStringLiteral("笔记.md"), QStringLiteral("标题: 未修改时不带星号"),
              tabs.tabText(0));
        check(tabs.tabToolTip(0) == QStringLiteral("D:/notes/笔记.md"),
              QStringLiteral("标题: 完整路径进 tooltip"), tabs.tabToolTip(0));

        tabs.updateTab(0, infoFor(QStringLiteral("笔记.md"), true, QStringLiteral("D:/notes/笔记.md")));
        check(tabs.tabText(0) == QStringLiteral("笔记.md *"), QStringLiteral("标题: 有未保存修改时带星号"),
              tabs.tabText(0));

        tabs.updateTab(0, infoFor(QStringLiteral("笔记.md"), false, QStringLiteral("D:/notes/笔记.md")));
        check(tabs.tabText(0) == QStringLiteral("笔记.md"),
              QStringLiteral("标题: 保存后星号消失"), tabs.tabText(0));

        tabs.updateTab(0, infoFor(QString(), false));
        check(tabs.tabText(0) == QStringLiteral("未命名"),
              QStringLiteral("标题: 文件名空 -> 未命名"), tabs.tabText(0));

        tabs.updateTab(99, infoFor(QStringLiteral("x"), false));
        check(tabs.count() == 1, QStringLiteral("标题: 越界索引被忽略（不崩）"));
        check(editor != nullptr, QStringLiteral("标题: 编辑器指针仍然有效"));
    }

    // ============================ C. 切换标签 ============================
    {
        TabManager tabs;
        EditorWidget *first = tabs.addEditorTab(QStringLiteral("a.md"));
        EditorWidget *second = tabs.addEditorTab(QStringLiteral("b.md"));

        EditorWidget *lastReported = nullptr;
        int changes = 0;
        QObject::connect(&tabs, &TabManager::currentEditorChanged, [&](EditorWidget *editor) {
            lastReported = editor;
            ++changes;
        });

        tabs.setCurrentIndex(0);
        check(changes == 1 && lastReported == first,
              QStringLiteral("切换: 发 currentEditorChanged 且带上正确的编辑器"),
              QStringLiteral("changes=%1").arg(changes));

        tabs.setCurrentIndex(1);
        check(lastReported == second, QStringLiteral("切换: 换回来也正确"));
        check(first != second, QStringLiteral("切换: 两个编辑器互不影响"));
    }

    // ============================ D. 关闭标签（含确认回调）============================
    {
        TabManager tabs;
        EditorWidget *first = tabs.addEditorTab(QStringLiteral("a.md"));
        EditorWidget *second = tabs.addEditorTab(QStringLiteral("b.md"));

        QPointer<EditorWidget> firstGuard(first);
        QPointer<EditorWidget> secondGuard(second);
        check(!firstGuard.isNull() && !secondGuard.isNull(), QStringLiteral("关闭前: 两个页面都还在"));

        // ---- 回调说"取消"：必须什么都不做 ----
        int asked = 0;
        EditorWidget *askedAbout = nullptr;
        tabs.setCloseConfirmHandler([&](EditorWidget *editor) {
            ++asked;
            askedAbout = editor;
            return false;  // 用户在"要不要保存"里点了取消
        });

        check(!tabs.requestCloseTab(0), QStringLiteral("确认回调返回 false: requestCloseTab 返回 false"));
        check(tabs.count() == 2, QStringLiteral("确认回调返回 false: ★标签没有被关掉"));
        check(asked == 1 && askedAbout == first,
              QStringLiteral("确认回调: 收到的是「要关的那个编辑器」"));
        check(!firstGuard.isNull(), QStringLiteral("确认回调返回 false: 页面没有被销毁"));

        // ---- 回调说"可以关" ----
        tabs.setCloseConfirmHandler([](EditorWidget *) { return true; });
        check(tabs.requestCloseTab(0), QStringLiteral("确认回调返回 true: requestCloseTab 返回 true"));
        check(tabs.count() == 1, QStringLiteral("确认回调返回 true: 标签被关掉"));
        check(firstGuard.isNull(), QStringLiteral("关闭: ★页面真的被销毁了（没泄漏）"));
        check(!secondGuard.isNull(), QStringLiteral("关闭: 另一个标签不受影响"));

        // ---- 没注入回调时：当作同意（无界面场景也能用）----
        tabs.setCloseConfirmHandler(nullptr);
        check(tabs.requestCloseTab(0), QStringLiteral("没注入回调: 按「同意」处理"));
        check(tabs.count() == 0, QStringLiteral("没注入回调: 标签被关掉"));
        check(secondGuard.isNull(), QStringLiteral("关闭: 最后一个页面也释放了"));
    }

    // ============================ E. 全部关闭 ============================
    {
        TabManager tabs;
        tabs.addEditorTab(QStringLiteral("a.md"));
        tabs.addEditorTab(QStringLiteral("b.md"));
        tabs.addEditorTab(QStringLiteral("c.md"));

        int asked = 0;
        tabs.setCloseConfirmHandler([&](EditorWidget *) {
            ++asked;
            return false;  // 每次都说取消
        });
        check(!tabs.requestCloseAllTabs(), QStringLiteral("全部关闭: 用户取消 -> 返回 false"));
        check(tabs.count() == 3 && asked == 1,
              QStringLiteral("全部关闭: ★第一次被取消就停手，一个都没关"),
              QStringLiteral("count=%1 asked=%2").arg(tabs.count()).arg(asked));

        tabs.setCloseConfirmHandler([](EditorWidget *) { return true; });
        check(tabs.requestCloseAllTabs(), QStringLiteral("全部关闭: 一路同意 -> 返回 true"));
        check(tabs.count() == 0, QStringLiteral("全部关闭: 标签全没了"));
    }

    // ============================ F. 拖拽排序 ============================
    {
        TabManager tabs;
        tabs.addEditorTab(QStringLiteral("a.md"));
        tabs.addEditorTab(QStringLiteral("b.md"));
        tabs.addEditorTab(QStringLiteral("c.md"));
        check(tabs.tabTitles() == QStringList({QStringLiteral("a.md"), QStringLiteral("b.md"), QStringLiteral("c.md")}),
              QStringLiteral("排序: 初始顺序 a,b,c"), tabs.tabTitles().join(QLatin1Char(',')));

        QStringList reported;
        int orderSignals = 0;
        QObject::connect(&tabs, &TabManager::tabOrderChanged, [&](const QStringList &titles) {
            reported = titles;
            ++orderSignals;
        });

        // 模拟用户把第一个标签拖到中间（QTabWidget 的拖拽最终就是 tabBar 的 moveTab）
        tabs.tabBar()->moveTab(0, 1);

        check(orderSignals == 1, QStringLiteral("排序: 拖拽之后发了 tabOrderChanged"),
              QStringLiteral("signals=%1").arg(orderSignals));
        check(reported == QStringList({QStringLiteral("b.md"), QStringLiteral("a.md"), QStringLiteral("c.md")}),
              QStringLiteral("排序: 信号里带的是新顺序"), reported.join(QLatin1Char(',')));
        check(tabs.tabTitles() == reported, QStringLiteral("排序: tabTitles() 与实际顺序一致"));
    }

    if (g_fail == 0) {
        std::printf("\n=== TabManager 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== TabManager 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
