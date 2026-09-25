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

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QMenu>
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

    // ============================ G. 右键菜单与"关其它/关右侧"（6.2）============================
    {
        TabManager tabs;
        tabs.addEditorTab(QStringLiteral("a.md"));
        tabs.addEditorTab(QStringLiteral("b.md"));
        tabs.addEditorTab(QStringLiteral("c.md"));

        // 标签的"磁盘路径"是给右键菜单的"复制路径/在文件管理器中显示"用的
        TabManager::TabInfo infoA;
        infoA.fileName = QStringLiteral("a.md");
        infoA.filePath = QStringLiteral("D:/notes/a.md");
        tabs.updateTab(0, infoA);
        TabManager::TabInfo infoB;
        infoB.fileName = QStringLiteral("b.md");
        tabs.updateTab(1, infoB);  // b 故意不给路径（模拟"新建还没保存"）

        check(tabs.tabFilePath(0) == QStringLiteral("D:/notes/a.md"),
              QStringLiteral("路径: updateTab 记下了文件路径"), tabs.tabFilePath(0));
        check(tabs.tabFilePath(1).isEmpty(), QStringLiteral("路径: 没路径的标签返回空"));
        check(tabs.tabFilePath(99).isEmpty(), QStringLiteral("路径: 越界返回空（不崩）"));

        // 悬停提示（7.3）：有路径的标签提示完整路径，改过的再加一句"有未保存的修改"
        check(tabs.tabToolTip(0) == QStringLiteral("D:/notes/a.md"),
              QStringLiteral("提示: 未修改时就是完整路径"), tabs.tabToolTip(0));
        {
            TabManager::TabInfo modifiedInfo = infoA;
            modifiedInfo.modified = true;
            tabs.updateTab(0, modifiedInfo);
            check(tabs.tabToolTip(0).contains(QStringLiteral("未保存")),
                  QStringLiteral("提示: 有未保存修改时提示里会说明"), tabs.tabToolTip(0));
            check(tabs.tabText(0).endsWith(QStringLiteral("*")),
                  QStringLiteral("提示: 标签标题上的 * 也还在"), tabs.tabText(0));
            tabs.updateTab(0, infoA);  // 复原
        }

        // ---- 菜单结构 ----
        QMenu *menu = tabs.createTabContextMenu(0);
        QStringList texts;
        for (QAction *action : menu->actions()) {
            texts << (action->isSeparator() ? QStringLiteral("---") : action->text());
        }
        check(texts.contains(QStringLiteral("关闭标签")) && texts.contains(QStringLiteral("关闭其它标签"))
                  && texts.contains(QStringLiteral("关闭右侧标签")) && texts.contains(QStringLiteral("全部关闭")),
              QStringLiteral("菜单: 关闭/关闭其它/关闭右侧/全部关闭 都在"), texts.join(QStringLiteral(" / ")));
        check(texts.contains(QStringLiteral("复制文件路径")) && texts.contains(QStringLiteral("在文件管理器中显示")),
              QStringLiteral("菜单: 有复制路径与在文件管理器中显示"));

        const auto enabledOf = [&menu](const QString &text) {
            for (QAction *action : menu->actions()) {
                if (action->text() == text) {
                    return action->isEnabled();
                }
            }
            return false;
        };
        check(enabledOf(QStringLiteral("复制文件路径")), QStringLiteral("菜单: 有路径的标签 -> 复制路径可用"));
        check(enabledOf(QStringLiteral("关闭其它标签")) && enabledOf(QStringLiteral("关闭右侧标签")),
              QStringLiteral("菜单: 中间那个标签 -> 关其它/关右侧都可用"));
        delete menu;

        // 第一个标签：右边还有别的，所以"关闭右侧"可用；最后一个标签则不该可用
        QMenu *firstMenu = tabs.createTabContextMenu(0);
        QMenu *lastMenu = tabs.createTabContextMenu(2);
        const auto enabledIn = [](QMenu *m, const QString &text) {
            for (QAction *action : m->actions()) {
                if (action->text() == text) {
                    return action->isEnabled();
                }
            }
            return false;
        };
        check(enabledIn(firstMenu, QStringLiteral("关闭右侧标签")),
              QStringLiteral("菜单: 第一个标签 -> 关闭右侧可用"));
        check(!enabledIn(lastMenu, QStringLiteral("关闭右侧标签")),
              QStringLiteral("菜单: 最后一个标签 -> 关闭右侧禁用（右边没有东西）"));
        delete firstMenu;
        delete lastMenu;

        // 没有路径的标签："复制路径"要禁用（否则会复制出一个空串）
        QMenu *noPathMenu = tabs.createTabContextMenu(1);
        check(!enabledIn(noPathMenu, QStringLiteral("复制文件路径")),
              QStringLiteral("菜单: 没路径的标签 -> 复制路径禁用"));
        delete noPathMenu;

        // ---- 关闭行为也要走"先问一声"那条路 ----
        int asked = 0;
        tabs.setCloseConfirmHandler([&](EditorWidget *) {
            ++asked;
            return false;  // 一律取消
        });
        check(!tabs.requestCloseOtherTabs(0), QStringLiteral("关其它: 用户取消 -> 返回 false"));
        check(tabs.count() == 3, QStringLiteral("关其它: 取消时一个都没关"), QStringLiteral("count=%1").arg(tabs.count()));
        check(!tabs.requestCloseTabsToRight(0), QStringLiteral("关右侧: 用户取消 -> 返回 false"));
        check(tabs.count() == 3, QStringLiteral("关右侧: 取消时一个都没关"));

        tabs.setCloseConfirmHandler([](EditorWidget *) { return true; });
        check(tabs.requestCloseTabsToRight(0), QStringLiteral("关右侧: 同意 -> 成功"));
        check(tabs.count() == 1 && tabs.tabTitles().first().startsWith(QStringLiteral("a.md")),
              QStringLiteral("关右侧: 只剩最左边那个"), tabs.tabTitles().join(QLatin1Char(',')));

        // 关其它：现在只剩一个标签了，先补两个再试
        tabs.addEditorTab(QStringLiteral("d.md"));
        tabs.addEditorTab(QStringLiteral("e.md"));
        check(tabs.count() == 3, QStringLiteral("关其它: 准备（3 个标签）"));
        check(tabs.requestCloseOtherTabs(0), QStringLiteral("关其它: 同意 -> 成功"));
        check(tabs.count() == 1 && tabs.tabTitles().first().startsWith(QStringLiteral("a.md")),
              QStringLiteral("关其它: 只留下被右键的那一个"), tabs.tabTitles().join(QLatin1Char(',')));
    }

    // ============================ H. 最近关闭的栈（C3）============================
    //
    // Ctrl+Shift+T 能不能"真的把那一个开回来"，取决于栈记的对不对。
    // 这里面最容易做错、又最不该做错的是一条**否定规则**：
    //   ★ 没有路径的标签（从没保存过的新文档）绝不能进栈。
    // 因为它进去之后，用户按 Ctrl+Shift+T 会以为自己那半页字能回来，
    // 结果只弹出一个空标签 —— 我们其实没留任何副本。这条规则要用测试钉住。
    {
        TabManager tabs;
        int availabilitySignals = 0;
        bool lastAvailability = false;
        QObject::connect(&tabs, &TabManager::closedTabAvailabilityChanged, [&](bool canReopen) {
            ++availabilitySignals;
            lastAvailability = canReopen;
        });

        check(!tabs.canReopenClosedTab(), QStringLiteral("栈: 一开始没有可重开的标签"));
        check(availabilitySignals == 0, QStringLiteral("栈: 没人关过东西 -> 不发状态信号"));
        const TabManager::ClosedTab emptyRecord = tabs.takeLastClosedTab();
        check(emptyRecord.filePath.isEmpty(), QStringLiteral("栈: 空栈弹出的是「没有路径」的记录（不崩）"));

        // ---- 关一个有路径的标签 ----
        tabs.addEditorTab();
        tabs.updateTab(0, infoFor(QStringLiteral("a.md"), false, QStringLiteral("D:/notes/a.md")));
        tabs.closeTab(0);

        check(tabs.canReopenClosedTab(), QStringLiteral("栈: 关掉有路径的标签 -> 可以重开"));
        check(availabilitySignals == 1 && lastAvailability,
              QStringLiteral("栈: 0→1 时发了一次 available=true"),
              QStringLiteral("signals=%1").arg(availabilitySignals));
        QList<TabManager::ClosedTab> stack = tabs.closedTabs();
        check(stack.size() == 1, QStringLiteral("栈: 多了 1 条"), QStringLiteral("size=%1").arg(stack.size()));
        check(stack.first().filePath == QStringLiteral("D:/notes/a.md"),
              QStringLiteral("栈: 记的是完整路径"), stack.first().filePath);
        check(stack.first().displayName == QStringLiteral("a.md"),
              QStringLiteral("栈: 显示名是关闭时标签上的名字"), stack.first().displayName);
        check(!stack.first().hadUnsavedChanges, QStringLiteral("栈: 关闭时没有未保存改动"));
        check(stack.first().closedAt.isValid(), QStringLiteral("栈: 记了关闭时间（提示语里要说清是哪一个）"));

        // ---- 关一个没路径的标签：★ 不能进栈 ----
        tabs.addEditorTab();  // 未命名，从没保存过
        tabs.updateTab(0, infoFor(QStringLiteral("未命名"), true, QString()));  // 有路径字段是空的
        tabs.closeTab(0);
        check(tabs.closedTabs().size() == 1,
              QStringLiteral("栈: ★没保存过的新文档不进栈（不给「能重开」的假承诺）"),
              QStringLiteral("size=%1").arg(tabs.closedTabs().size()));
        check(availabilitySignals == 1, QStringLiteral("栈: 那条被挡掉 -> 不发多余的状态信号"));

        // ---- 带着未保存改动关掉的：要能提醒"改动不会回来" ----
        tabs.addEditorTab();
        tabs.updateTab(0, infoFor(QStringLiteral("b.md"), true, QStringLiteral("D:/notes/b.md")));
        tabs.closeTab(0);
        check(tabs.closedTabs().size() == 2 && tabs.closedTabs().first().hadUnsavedChanges,
              QStringLiteral("栈: 记下「关的时候还带着未保存的改动」"));

        // ---- 逆序恢复：后关的在前 ----
        tabs.addEditorTab();  // 上一步把标签关空了，所以新标签又是 index 0
        tabs.updateTab(0, infoFor(QStringLiteral("c.md"), false, QStringLiteral("D:/notes/c.md")));
        tabs.closeTab(0);

        const QStringList expectedOrder{QStringLiteral("D:/notes/c.md"),
                                        QStringLiteral("D:/notes/b.md"),
                                        QStringLiteral("D:/notes/a.md")};
        QStringList popped;
        while (tabs.canReopenClosedTab()) {
            popped << tabs.takeLastClosedTab().filePath;
        }
        check(popped == expectedOrder,
              QStringLiteral("栈: ★连按 N 次是「逆序恢复」（最近关的先回来）"),
              popped.join(QStringLiteral(" -> ")));
        check(availabilitySignals == 2 && !lastAvailability,
              QStringLiteral("栈: 弹空之后发了 available=false（动作该变灰了）"),
              QStringLiteral("signals=%1").arg(availabilitySignals));
    }

    // ---- 上限与"每条关闭路径都会进栈" ----
    {
        TabManager tabs;
        // createTabContextMenu / requestCloseTabsToRight / requestCloseAllTabs 各自都是**独立的入口**，
        // 记录只写在 closeTab() 一处，所以这些入口必须全部覆盖到 —— 这个循环就是在钉这一点。
        for (int i = 0; i < TabManager::kMaxClosedTabs + 3; ++i) {
            tabs.addEditorTab();
            tabs.updateTab(i, infoFor(QStringLiteral("f%1.md").arg(i), false,
                                      QStringLiteral("D:/notes/f%1.md").arg(i)));
        }
        for (int i = tabs.count() - 1; i >= 0; --i) {
            tabs.requestCloseTab(i);  // 从后往前关（和 requestCloseAllTabs 的走法一致）
        }

        const QList<TabManager::ClosedTab> stack = tabs.closedTabs();
        check(stack.size() == TabManager::kMaxClosedTabs,
              QStringLiteral("栈: 关到超过上限时只留最近 %1 条").arg(TabManager::kMaxClosedTabs),
              QStringLiteral("size=%1").arg(stack.size()));
        check(stack.first().filePath == QStringLiteral("D:/notes/f0.md"),
              QStringLiteral("栈: 最后关掉的那条在最前面"), stack.first().filePath);
        check(!stack.last().filePath.contains(QStringLiteral("f11.md")),
              QStringLiteral("栈: 最老的被挤出去了"), stack.last().filePath);
        check(tabs.count() == 0 && tabs.canReopenClosedTab(),
              QStringLiteral("栈: 标签都关掉了，栈里还有东西（可以一路重开回来）"));
    }

    // ---- 通过 requestCloseTabsToRight / requestCloseAllTabs 关掉的也要进栈 ----
    {
        TabManager tabs;
        tabs.setCloseConfirmHandler([](EditorWidget *) { return true; });
        tabs.addEditorTab();
        tabs.updateTab(0, infoFor(QStringLiteral("a.md"), false, QStringLiteral("D:/notes/a.md")));
        tabs.addEditorTab();
        tabs.updateTab(1, infoFor(QStringLiteral("b.md"), false, QStringLiteral("D:/notes/b.md")));
        tabs.addEditorTab();
        tabs.updateTab(2, infoFor(QStringLiteral("c.md"), false, QStringLiteral("D:/notes/c.md")));

        check(tabs.requestCloseTabsToRight(0), QStringLiteral("栈: 关右侧 -> 成功"));
        check(tabs.requestCloseAllTabs(), QStringLiteral("栈: 关全部 -> 成功"));
        check(tabs.closedTabs().size() == 3,
              QStringLiteral("栈: ★其它关闭入口（关右侧/关全部）也进了栈，一条都没漏"),
              QStringLiteral("size=%1").arg(tabs.closedTabs().size()));
        // 栈是"最近的在前"，所以最后关掉的 a.md 在栈顶（不是栈底）。
        // 这条别写反 —— 写反了测试也是绿的，但"重开"就会翻出最老的那个。
        check(tabs.closedTabs().first().filePath == QStringLiteral("D:/notes/a.md"),
              QStringLiteral("栈: 最后关的 a.md 在栈顶"), tabs.closedTabs().first().filePath);
    }

    if (g_fail == 0) {
        std::printf("\n=== TabManager 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== TabManager 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
