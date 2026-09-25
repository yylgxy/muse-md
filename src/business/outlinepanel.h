#ifndef OUTLINEPANEL_H
#define OUTLINEPANEL_H

#include "markdownoutline.h"

#include <QList>
#include <QWidget>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

// 大纲面板：把 MarkdownOutline 抽出来的标题画成一棵**真父子树**，点击发跳转信号。
//
// ---- 分工（和 SearchPanel 一字不差）----
//   * **机制是公开函数**（setSource / clear / items / setPaused）：不弹窗、不依赖主窗口，
//     测试能直接驱动它并断言树长什么样；
//   * **面板不认识主窗口**：用户点了某一项，它只发 lineActivated(行号)。
//     "切到那个文档、把光标移过去、拿焦点"是主窗口的事（跳行入口全局只有
//     EditorWidget::goToLine 一个，见 C4 的说明）。
//
// ---- 为什么是"真父子树"而不是"每行加缩进" ----
//   大纲一半的价值在折叠（一篇长文里先看骨架，再展开要看的那一节）。
//   所以这里维护一个"层级栈"：遇到 level 时把栈顶所有 >= 当前 level 的弹出，
//   剩下的栈顶就是它的父节点；栈空则是顶层。栈的行为有测试钉着（test_outlinepanel）。
//
// ---- 行号存在哪 ----
//   存在 Qt::UserRole 里（kLineRole），点击时从 role 取，**不去解析显示文本**。
//   显示文本以后可能加别的东西（标记、序号），从文本反解析行号属于自找的脆弱。
class OutlinePanel : public QWidget
{
    Q_OBJECT

public:
    explicit OutlinePanel(QWidget *parent = nullptr);

    // ---- 机制（都不弹窗）----
    void setSource(const QString &text);  // 重新扫描并整体重建树（同时解除暂停态）
    void clear();                          // 变成"没有文档"的空状态
    QList<markdown_editor::core::document::OutlineItem> items() const { return m_items; }

    // ---- 结果（给界面和测试用）----
    int visibleItemCount() const;   // 树里实际有几项
    bool isPaused() const { return m_paused; }

    // 因为文档过大而暂停（调用方判阈值，面板只负责"显示成暂停态"）。
    // 暂停时清空树并说明原因 —— 让面板看起来是"空白的"，用户会以为功能坏了。
    void setPaused(bool paused);

    // 一行怎么显示。★ 截断规则只有一份：直接转发 MarkdownOutline::displayTextFor()，
    // 面板不自己再写一遍（同一条规则抄两份，迟早漂移）。
    // 层级缩进不在这里做：那由 QTreeWidget 的父子关系表达（见类注释）。
    static QString displayTextFor(const markdown_editor::core::document::OutlineItem &item,
                                  int maxChars = 120);

    // 行号存在这个 role 里（从 role 取，不解析显示文本）
    static constexpr int kLineRole = Qt::UserRole + 1;

signals:
    void lineActivated(int line);          // 1 起算
    void statusMessage(const QString &text);

protected:
    // 主题切换时调色板会变，行号列的灰色要跟着新调色板重算
    void changeEvent(QEvent *event) override;

private slots:
    void onItemActivated(QTreeWidgetItem *item, int column);

private:
    void rebuildTree();
    void updateHint();          // 空状态 / 暂停态：显示哪一句话（或都不显示）
    void applyLineNumberColor();  // 行号列用"次要文字"的灰

    QTreeWidget *m_tree = nullptr;
    QLabel *m_hint = nullptr;
    QList<markdown_editor::core::document::OutlineItem> m_items;
    bool m_paused = false;
};

#endif  // OUTLINEPANEL_H
