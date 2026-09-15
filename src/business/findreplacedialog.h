#ifndef FINDREREPLACEDIALOG_H
#define FINDREREPLACEDIALOG_H

#include <QDialog>
#include <QString>

class EditorWidget;  // 全局命名空间的类（见 editorwidget.h）
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

// 查找 / 替换对话框（主窗口布局那一节的「编辑」菜单要它）。
//
// 为什么在 business 层而不是 ui 层：它和 FileTreeView、SearchPanel 是同一类东西 ——
// "带对话框的界面部件"，行为（查找下一个/替换/全部替换）都委托给 EditorWidget。
// 放在 business 的好处是测试不用链 ui 层就能把它跑起来（ui 层要拖进整个 MainWindow）。
//
// 设计上的两个选择：
//   * **非模态**：查着改着是最常见的用法，模态对话框会让人没法一边看结果一边改文档。
//   * 对话框本身**不存搜索状态**："当前找到哪一处"由编辑器里的光标与选区表示，
//     所以切标签、手动点别处之后行为依然正确，不会出现"对话框以为在第 3 处"的错位。
class FindReplaceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FindReplaceDialog(QWidget *parent = nullptr);

    // 绑定当前要操作的编辑器（切标签时主窗口会重新绑一次）。
    // 传 nullptr（没有标签时）安全：所有按钮会变成"没东西可查"的状态。
    void setEditor(EditorWidget *editor);
    EditorWidget *editor() const;

    // ---- 输入与状态（给菜单/测试用，不需要模拟键盘输入）----
    QString searchText() const;
    void setSearchText(const QString &text);
    QString replaceText() const;
    void setReplaceText(const QString &text);
    bool caseSensitive() const;
    void setCaseSensitive(bool enabled);
    QString statusText() const;  // 底部那行提示（"共 12 处，已到第 3 处"之类）

    // ---- 机制（不弹窗，能单独测）----
    // 都不是槽，但都做了空指针保护：没有编辑器时返回"没找到/替换 0 处"。
    bool findNext();
    bool findPrevious();
    bool replaceOne();
    int replaceAll();

    // 把焦点放到"查找内容"输入框并全选（用户按 Ctrl+F 之后应该能直接打字）
    void focusSearchField();

private:
    // 重新数一遍并更新提示与按钮可用状态。prefix 非空时加在提示前面
    //（例如"没有找到（共 3 处）"—— 让人既看到这次没找到、也知道全文有几处）。
    void refreshStatus(const QString &prefix = QString());
    void updateButtons();

    EditorWidget *m_editor = nullptr;

    QLineEdit *m_searchEdit = nullptr;
    QLineEdit *m_replaceEdit = nullptr;
    QCheckBox *m_caseCheck = nullptr;
    QPushButton *m_findNextButton = nullptr;
    QPushButton *m_findPrevButton = nullptr;
    QPushButton *m_replaceButton = nullptr;
    QPushButton *m_replaceAllButton = nullptr;
    QLabel *m_status = nullptr;
};

#endif // FINDREREPLACEDIALOG_H
