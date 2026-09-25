// OutlinePanel（C4 大纲面板）的契约测试。
//
// 需要 Qt6::Widgets（QTreeWidget / QLabel）—— 这正是"规则下沉到 core、界面留在 business"
// 分工的结果：**规则部分**（markdownoutline）在 test_outline 里跑，不需要任何窗口；
// 这里只管"标题列表 → 树"这一步，以及空状态 / 暂停态。
//
// 重点验三件"只读代码看不出来"的事：
//   1. ★ **真父子关系**：`##` 必须挂在**最近的** `#` 下面，不是"每行加缩进"。
//      靠一个"层级栈"撑起来，而栈最容易写错的就是连续降级 / 跳级 / 从第 2 级开头
//      （文档里第一个标题就是 `##`，栈是空的，此时必须当顶层而不是崩）。
//   2. 行号存进 UserRole，点击时从 role 取 —— 不去解析显示文本。
//   3. 空状态 / 暂停态都**有话说**（留一块空白用户会以为功能坏了）。

#include "outlinepanel.h"

#include <QApplication>
#include <QLabel>
#include <QString>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>

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

// 把树压成 "层级:行号:标题" —— 层级用**真实的父子深度**推出来，
// 不是读我们自己的数据结构。这样断言测的是树本身。
void dumpTree(const QTreeWidgetItem *item, int depth, QStringList *out)
{
    out->append(QStringLiteral("%1:%2:%3")
                    .arg(depth)
                    .arg(item->data(0, OutlinePanel::kLineRole).toInt())
                    .arg(item->text(0)));
    for (int i = 0; i < item->childCount(); ++i) {
        dumpTree(item->child(i), depth + 1, out);
    }
}

QStringList dumpTree(const OutlinePanel &panel)
{
    const auto *tree = panel.findChild<QTreeWidget *>();
    QStringList out;
    if (tree == nullptr) {
        return out;
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        dumpTree(tree->topLevelItem(i), 1, &out);
    }
    return out;
}

QTreeWidget *treeOf(const OutlinePanel &panel)
{
    return panel.findChild<QTreeWidget *>();
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ============================ A. 层级栈（真父子关系）============================
    {
        OutlinePanel panel;
        // # A / ## B / ### C / ## D / # E
        // 期望：A 下挂 B，B 下挂 C，B 的兄弟 D（挂回 A），E 是新的顶层。
        panel.setSource(QStringLiteral("# A\n## B\n### C\n## D\n# E"));

        const QStringList expected{QStringLiteral("1:1:A"),
                                   QStringLiteral("2:2:B"),
                                   QStringLiteral("3:3:C"),
                                   QStringLiteral("2:4:D"),
                                   QStringLiteral("1:5:E")};
        const QStringList actual = dumpTree(panel);
        check(actual == expected, QStringLiteral("A1) ★层级栈：真父子关系（## 挂在最近的 # 下面）"),
              actual.join(QStringLiteral(" | ")));
        check(panel.visibleItemCount() == 5, QStringLiteral("A2) 树里 5 项（含所有后代，不是只数顶层）"),
              QStringLiteral("count=%1").arg(panel.visibleItemCount()));
        check(panel.items().size() == 5, QStringLiteral("A2b) items() 与树里的节点数一致（两个来源对得上）"));
    }

    {
        // 跳级：# 然后 ##### —— 深层标题照样挂在浅层下面（没有中间层就少几级缩进，不该崩）
        OutlinePanel panel;
        panel.setSource(QStringLiteral("# 一\n##### 很深\n# 二"));
        const QStringList expected{QStringLiteral("1:1:一"),
                                   QStringLiteral("2:2:很深"),
                                   QStringLiteral("1:3:二")};
        check(dumpTree(panel) == expected, QStringLiteral("A3) 跳级（# 直接接 #####）不会崩，层级照样算对"),
              dumpTree(panel).join(QStringLiteral(" | ")));
    }

    {
        // ★ 文档里第一个标题就是 ##：栈是空的，它必须成为顶层
        OutlinePanel panel;
        panel.setSource(QStringLiteral("## 二级开头\n### 下面这个\n# 后来的一级"));
        const QStringList expected{QStringLiteral("1:1:二级开头"),
                                   QStringLiteral("2:2:下面这个"),
                                   QStringLiteral("1:3:后来的一级")};
        check(dumpTree(panel) == expected,
              QStringLiteral("A4) ★第一个标题是二级（栈空）-> 当顶层，不崩"),
              dumpTree(panel).join(QStringLiteral(" | ")));
    }

    {
        // 连续同级：三个 ## 应该是三个兄弟，不是链式父子
        OutlinePanel panel;
        panel.setSource(QStringLiteral("# 父\n## 甲\n## 乙\n## 丙"));
        const QStringList actual = dumpTree(panel);
        check(actual.size() == 4 && actual.at(1).startsWith(QStringLiteral("2:"))
                  && actual.at(2).startsWith(QStringLiteral("2:"))
                  && actual.at(3).startsWith(QStringLiteral("2:")),
              QStringLiteral("A5) 连续同级是兄弟，不是一条链"), actual.join(QStringLiteral(" | ")));
        const QTreeWidget *tree = treeOf(panel);
        check(tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 3,
              QStringLiteral("A5b) 顶层 1 个、它下面 3 个子节点"),
              QStringLiteral("top=%1 child=%2")
                  .arg(tree->topLevelItemCount())
                  .arg(tree->topLevelItem(0)->childCount()));
    }

    // ============================ B. 行号与显示 ============================
    {
        OutlinePanel panel;
        panel.setSource(QStringLiteral("# A\n\n\n## B"));
        QTreeWidget *tree = treeOf(panel);
        QTreeWidgetItem *second = tree->topLevelItem(0)->child(0);
        check(second != nullptr && second->data(0, OutlinePanel::kLineRole).toInt() == 4,
              QStringLiteral("B1) 行号存在 UserRole 里（不靠解析显示文本）"),
              second == nullptr ? QString() : QString::number(second->data(0, OutlinePanel::kLineRole).toInt()));
        check(second != nullptr && second->text(1) == QStringLiteral("4"),
              QStringLiteral("B1b) 第 1 列显示行号"), second == nullptr ? QString() : second->text(1));
        check(second != nullptr && second->text(0) == QStringLiteral("B"),
              QStringLiteral("B1c) 第 0 列只放标题文本（缩进交给树）"));
        check(second != nullptr && second->toolTip(0).contains(QStringLiteral("第 4 行")),
              QStringLiteral("B1d) 悬停提示带完整标题与行号"));
    }

    {
        // 点击 → 发跳转信号，行号从 role 取
        OutlinePanel panel;
        panel.setSource(QStringLiteral("# A\n## B"));
        int activated = -1;
        QString message;
        QObject::connect(&panel, &OutlinePanel::lineActivated, [&](int line) { activated = line; });
        QObject::connect(&panel, &OutlinePanel::statusMessage, [&](const QString &text) { message = text; });

        QTreeWidget *tree = treeOf(panel);
        QTreeWidgetItem *second = tree->topLevelItem(0)->child(0);
        emit tree->itemClicked(second, 0);
        check(activated == 2, QStringLiteral("B2) ★点击子节点 -> lineActivated(第 2 行)"),
              QStringLiteral("line=%1").arg(activated));
        check(message.contains(QStringLiteral("第 2 行")), QStringLiteral("B2b) 同时给了一句状态栏提示"), message);
    }

    {
        // displayTextFor 与 MarkdownOutline 是**同一份**实现（面板不自己再写一遍）
        markdown_editor::core::document::OutlineItem item;
        // 50 个「长」字（QChar 码点，不用 QLatin1Char('长')，理由同 test_outline 14b）
        item.title = QString(50, QChar(0x957F));
        const QString shown = OutlinePanel::displayTextFor(item, 6);
        check(shown == markdown_editor::core::document::MarkdownOutline::displayTextFor(item, 6),
              QStringLiteral("B3) 面板的 displayTextFor 转发给 core（截断规则只有一份）"), shown);
    }

    // ============================ C. 空状态与暂停态 ============================
    {
        OutlinePanel panel;
        auto *hint = panel.findChild<QLabel *>();
        check(hint != nullptr, QStringLiteral("C0) 面板里有那句提示标签（空状态要靠它说话）"));

        panel.setSource(QStringLiteral("没有标题的正文\n只有这一行"));
        check(panel.visibleItemCount() == 0, QStringLiteral("C1) 没标题 -> 树是空的"));
        // ⚠️ 判"提示在不在"只能用 isHidden()，**不能用 isVisible()**：
        //    这个面板从头到尾没 show() 过（单元测试不需要真窗口），而 isVisible() 只要
        //    自己或任一祖先被隐藏就是 false —— 用它这几条断言会变成永远成立的空断言。
        //    isHidden() 问的是"它自己被显式藏起来了吗"，才是这里想问的事。
        check(hint != nullptr && !hint->isHidden() && hint->text().contains(QStringLiteral("还没有标题")),
              QStringLiteral("C1b) ★空状态有说明文字（不留一块空白）"),
              hint == nullptr ? QString() : hint->text());

        panel.setSource(QStringLiteral("# 有标题了"));
        check(panel.visibleItemCount() == 1,
              QStringLiteral("C2) 再来一个有标题的文档 -> 树重建，不受上一次影响"));
        check(hint != nullptr && hint->isHidden(), QStringLiteral("C2b) 有内容时提示收起来"));
    }

    {
        OutlinePanel panel;
        auto *hint = panel.findChild<QLabel *>();
        panel.setSource(QStringLiteral("# 之前的标题"));
        panel.setPaused(true);
        check(panel.isPaused(), QStringLiteral("C3) setPaused(true) 生效"));
        check(panel.visibleItemCount() == 0 && panel.items().isEmpty(),
              QStringLiteral("C3b) ★暂停时清空树（留着旧标题会让人以为还能点）"));
        check(hint != nullptr && !hint->isHidden() && hint->text().contains(QStringLiteral("30 万字符")),
              QStringLiteral("C3c) 暂停提示里写清了阈值与原因"),
              hint == nullptr ? QString() : hint->text());

        // 有新的文本进来就解除暂停（暂停是"上一次的结论"，不该留到下一次）
        panel.setSource(QStringLiteral("# 新文档"));
        check(!panel.isPaused() && panel.visibleItemCount() == 1,
              QStringLiteral("C4) setSource 解除暂停态"));
    }

    {
        OutlinePanel panel;
        panel.setSource(QStringLiteral("# A\n## B"));
        panel.clear();
        check(panel.visibleItemCount() == 0 && panel.items().isEmpty() && !panel.isPaused(),
              QStringLiteral("C5) clear() 回到空状态，且不残留暂停标记"));
        auto *hint = panel.findChild<QLabel *>();
        check(hint != nullptr && !hint->isHidden(), QStringLiteral("C5b) clear() 后又是空状态提示"));
    }

    // ============================ D. 重复设源不累积 ============================
    {
        OutlinePanel panel;
        for (int i = 0; i < 5; ++i) {
            panel.setSource(QStringLiteral("# 第 %1 次\n## 子项").arg(i));
        }
        check(panel.visibleItemCount() == 2,
              QStringLiteral("D1) ★连续 setSource 5 次：树不累积（每次全量重建）"),
              QStringLiteral("count=%1").arg(panel.visibleItemCount()));
        check(treeOf(panel)->topLevelItemCount() == 1 && treeOf(panel)->topLevelItem(0)->text(0).contains(QStringLiteral("第 4 次")),
              QStringLiteral("D1b) 留下的是最后一次的内容"),
              treeOf(panel)->topLevelItem(0)->text(0));
    }

    if (g_fail == 0) {
        std::printf("\n=== OutlinePanel 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== OutlinePanel 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
