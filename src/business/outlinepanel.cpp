#include "outlinepanel.h"

#include <QEvent>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

using markdown_editor::core::document::MarkdownOutline;
using markdown_editor::core::document::OutlineItem;

namespace {

// 数一棵树里有多少个节点（含所有后代）。
// 写成"真的去走树"而不是直接返回 m_items.size()：这样测试拿它和 items().size() 对比时，
// 比的是**两个独立的来源**（层级栈建出来的树 ↔ 抽出来的列表）。要是这里偷懒直接返回
// items().size()，那个断言就永远成立，等于没测。
int countNodes(const QTreeWidgetItem *item)
{
    int count = 1;
    for (int i = 0; i < item->childCount(); ++i) {
        count += countNodes(item->child(i));
    }
    return count;
}

}  // namespace

OutlinePanel::OutlinePanel(QWidget *parent) : QWidget(parent)
{
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({QStringLiteral("标题"), QStringLiteral("行")});
    m_tree->setRootIsDecorated(true);      // 有子节点时给出折叠箭头（大纲一半的价值在这）
    m_tree->setUniformRowHeights(true);    // 行高一致时 QTreeWidget 可以走快路径（大文档友好）
    m_tree->setAlternatingRowColors(false);  // 深色主题下斑马纹会和背景打架
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    // 缩进宽度从 MarkdownOutline 取：层级视觉只有一处定义（见 indentForLevel 的注释）。
    // 传 2 是因为它算的是"第二层的缩进量"，也就是一层的步长。
    m_tree->setIndentation(MarkdownOutline::indentForLevel(2));

    QHeaderView *header = m_tree->header();
    header->setSectionResizeMode(0, QHeaderView::Stretch);          // 标题吃掉剩余宽度
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents); // 行号列刚好够宽

    // 空状态 / 暂停态的说明文字。默认藏起来 —— 有内容时不该占一行地方。
    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_hint->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_hint, 0);

    // 单击（不是双击）就跳：大纲是导航控件，多一步双击没有意义。
    // 用 itemClicked 而不是 itemActivated —— itemActivated 在部分平台上要双击或回车。
    connect(m_tree, &QTreeWidget::itemClicked, this, &OutlinePanel::onItemActivated);

    updateHint();
}

void OutlinePanel::setSource(const QString &text)
{
    // 全量重建。标题最多几百条，重建比"增量维护哪一条变了"简单得多、也可靠得多
    //（和 MainWindow::rebuildRecentMenu 的取舍一致：10 条以内整体重建更划算）。
    m_paused = false;  // 有了新文本就退出暂停态（暂停是"上一次的结论"，不该留到下一次）
    m_items = MarkdownOutline::extract(text);
    rebuildTree();
    updateHint();
    applyLineNumberColor();
}

void OutlinePanel::clear()
{
    m_paused = false;
    m_items.clear();
    rebuildTree();
    updateHint();
}

void OutlinePanel::setPaused(bool paused)
{
    if (m_paused == paused) {
        return;
    }
    m_paused = paused;
    if (paused) {
        // 暂停就把树清空：留着上一次的标题会让人以为"大纲是旧的、还能点"。
        m_items.clear();
        rebuildTree();
    }
    updateHint();
}

int OutlinePanel::visibleItemCount() const
{
    // 顶层 + 各层后代。QTreeWidget::topLevelItemCount() 只算顶层，
    // 而"树里实际有几项"必须把挂在 ## 下面的那些也算上。
    int count = 0;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        count += countNodes(m_tree->topLevelItem(i));
    }
    return count;
}

QString OutlinePanel::displayTextFor(const OutlineItem &item, int maxChars)
{
    return MarkdownOutline::displayTextFor(item, maxChars);
}

void OutlinePanel::onItemActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (item == nullptr) {
        return;
    }

    // 行号从 role 里取（不是从显示文本里抠数字）
    const int line = item->data(0, kLineRole).toInt();
    if (line <= 0) {
        return;  // 没有行号的项目（不该出现）点了也没意义
    }

    emit lineActivated(line);
    emit statusMessage(QStringLiteral("已跳到第 %1 行：%2").arg(line).arg(item->text(0)));
}

void OutlinePanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event != nullptr && event->type() == QEvent::PaletteChange) {
        // 亮 / 暗主题切换会换掉整个调色板，行号列那个灰得跟着新调色板重算。
        // 不监听 ThemeManager 而是监听自己的 PaletteChange：面板就不必认识主题系统
        // （谁换的调色板都行 —— 应用级、父窗口级都一样会送到这里）。
        applyLineNumberColor();
    }
}

void OutlinePanel::rebuildTree()
{
    m_tree->clear();

    // 层级栈：stack[i] 是当前第 i+1 层最近的那个节点。
    // 遇到新标题时先把"层级 >= 自己"的弹出，剩下栈顶就是它的父节点。
    QList<QTreeWidgetItem *> stack;
    for (const OutlineItem &entry : m_items) {
        while (!stack.isEmpty() && stack.size() >= entry.level) {
            stack.removeLast();
        }

        QTreeWidgetItem *node = stack.isEmpty() ? new QTreeWidgetItem(m_tree)
                                                : new QTreeWidgetItem(stack.last());
        node->setText(0, displayTextFor(entry));
        node->setText(1, QString::number(entry.line));
        node->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        node->setData(0, kLineRole, entry.line);
        // 悬停给完整标题（列表里可能被截断）与行号，免得为了看清去点它
        node->setToolTip(0, QStringLiteral("第 %1 行：%2").arg(entry.line).arg(entry.title));
        stack.append(node);
    }

    if (m_tree->topLevelItemCount() > 0) {
        m_tree->expandAll();  // 层级最多 6 层、条目最多几百，全展开比"折叠着让人自己点"好用
    }
}

void OutlinePanel::updateHint()
{
    if (m_paused) {
        // 说清楚"为什么没有"和"多大才算大"。只说"已暂停"，用户会以为是 bug。
        m_hint->setText(QStringLiteral("文档过大，已暂停大纲\n（超过 30 万字符时不再实时扫描："
                                       "全文扫描会把「停手 300 毫秒刷新」变成「停手几秒」）"));
    } else if (m_items.isEmpty()) {
        m_hint->setText(QStringLiteral("本文档还没有标题\n（用 `# 标题` 写一级，`##` 写二级…）"));
    } else {
        m_hint->setVisible(false);
        return;
    }
    m_hint->setVisible(true);
}

void OutlinePanel::applyLineNumberColor()
{
    // 行号是"辅助信息"，用调色板里的次要文字色（Qt 5.12+ 的 PlaceholderText），
    // 这样亮 / 暗主题下都是"比正文淡一点"的正确观感，不写死任何颜色值。
    const QBrush brush = palette().brush(QPalette::Disabled, QPalette::PlaceholderText);
    QTreeWidgetItemIterator it(m_tree);
    while (*it != nullptr) {
        (*it)->setForeground(1, brush);
        ++it;
    }
}
