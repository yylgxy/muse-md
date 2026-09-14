#ifndef TABMANAGER_H
#define TABMANAGER_H

#include <QString>
#include <QStringList>
#include <QTabWidget>

#include <functional>

class EditorWidget;  // 全局命名空间的类（和 MainWindow、MarkdownHighlighter 一致）

// 多标签页管理：一个标签 = 一个文档，页面就是 EditorWidget。
//
// 它只管"标签这一层"的事：新建/关闭/切换/拖拽排序、标签上的文件名与修改标记、
// 以及"关之前先问一声"的**机制**。
//
// 有一件事它**刻意不做**：不弹任何对话框。
// "这个文档改过了要不要保存"是界面策略（而且要知道文档对象），所以做成一个**注入的回调**
// （setCloseConfirmHandler），由主窗口提供。这样有两个好处：
//   1. 弹窗逻辑留在界面层，TabManager 保持可复用；
//   2. 测试能注入一个"假回调"（返回 false），从而验证"用户取消时标签不会被关掉"这种
//      光靠读代码看不出来的行为。
//
// 拖拽排序直接用 QTabWidget 自带的 movable 能力（setMovable(true)）：Qt 会连页面一起搬，
// 我们只需要在顺序变化后发个信号，方便主窗口记顺序（将来做会话恢复用得上）。
class TabManager : public QTabWidget
{
    Q_OBJECT

public:
    // 关闭前的确认回调：参数是要关掉的那个编辑器，返回 false = 用户取消，别关。
    using CloseConfirmHandler = std::function<bool(EditorWidget *)>;

    // 标签上要显示的信息
    struct TabInfo
    {
        QString fileName;              // 只显示文件名；空 = 未命名
        QString filePath;              // 完整路径，放到 tooltip 里
        bool modified = false;         // 有未保存修改时标题后面加 *
        QString title;                 // 可选：自定义标题（给"未命名 2"这种用）
    };

    explicit TabManager(QWidget *parent = nullptr);

    void setCloseConfirmHandler(CloseConfirmHandler handler);

    // 新建一个标签页（页面是新建的 EditorWidget），并切过去。返回那个编辑器。
    EditorWidget *addEditorTab(const QString &title = QString());

    EditorWidget *editorAt(int index) const;   // 越界返回 nullptr
    EditorWidget *currentEditor() const;       // 没有标签时返回 nullptr
    int indexOf(const EditorWidget *editor) const;

    // 更新标签文字与提示：文件名 + 修改标记（*）
    void updateTab(int index, const TabInfo &info);

    // 直接关掉（不询问）。页面是 new 出来的，removeTab 不会释放它，所以这里负责 delete。
    void closeTab(int index);
    void closeCurrentTab();

    // 问一遍再关：确认回调返回 false 就什么都不做，并返回 false。
    bool requestCloseTab(int index);

    // 从后往前逐个问（倒序是为了不受"关掉之后索引前移"的影响）。
    // 中途用户取消时返回 false —— 已经关掉的不再恢复，这和常见编辑器的行为一致。
    bool requestCloseAllTabs();

    // 当前所有标签的标题，按显示顺序。用于会话恢复/调试。
    QStringList tabTitles() const;

signals:
    // 当前标签换了（编辑器指针；没有标签时是 nullptr）。
    // 主窗口用它来切换预览内容、标题栏、状态栏。
    void currentEditorChanged(EditorWidget *editor);

    // 标签顺序变了（用户拖拽排序之后），带上新的标题顺序。
    void tabOrderChanged(const QStringList &titles);

private slots:
    void onCloseRequested(int index);   // 标签上的 × 被点了
    void onCurrentChanged(int index);   // QTabWidget 自带信号 → 转成 currentEditorChanged

private:
    CloseConfirmHandler m_closeConfirm;
};

#endif // TABMANAGER_H
