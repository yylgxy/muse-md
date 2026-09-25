#ifndef TABMANAGER_H
#define TABMANAGER_H

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTabWidget>

#include <functional>

class EditorWidget;  // 全局命名空间的类（和 MainWindow、MarkdownHighlighter 一致）
class QMenu;
class QPoint;
class QTabBar;

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

    // 最近关掉的一个标签（C3：Ctrl+Shift+T 重开）。
    //
    // ⚠️ **只记有路径的标签**。没保存过的新文档关掉之后内容就真的没了（我们没留副本），
    // 把它放进"可重开"的栈里会给用户一个假的承诺：按 Ctrl+Shift+T 期待那半页字回来，
    // 结果弹出一个空标签 —— 这比"按钮是灰的"更糟。所以没路径的直接不进栈。
    struct ClosedTab
    {
        QString filePath;                    // 一定是非空路径
        QString displayName;                 // 关闭时标签上的名字（提示语里用）
        bool hadUnsavedChanges = false;      // 关的时候还带着未保存的修改
        QDateTime closedAt;                  // 什么时候关的（提示语里说清楚是哪一个）
    };

    // 栈的深度：再多也没什么意义（人会忘，路子也该往回翻了）
    static constexpr int kMaxClosedTabs = 10;

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

    // 关闭除 index 之外的其它标签 / 关闭 index 右侧的标签。
    // 同样走"先问一声"的那条路（确认回调），所以不会绕过"未保存提示"。
    bool requestCloseOtherTabs(int index);
    bool requestCloseTabsToRight(int index);

    // ============================ 右键菜单 ============================
    // 造一份标签页右键菜单（调用方负责 delete）。公开出来是为了让"菜单里有哪些动作、
    // 什么时候该禁用"也能被测到 —— 和 FileTreeView/EditorWidget 是同一个做法。
    QMenu *createTabContextMenu(int index);

    // 这个标签对应的文档路径（给"复制路径/在文件管理器中显示"用）；没有路径返回空。
    QString tabFilePath(int index) const;
    // 这个标签的显示名（不含修改标记）
    QString tabFileName(int index) const;

    // 当前所有标签的标题，按显示顺序。用于会话恢复/调试。
    QStringList tabTitles() const;

    // ============================ 最近关闭（C3）============================
    // 栈的语义：**后关的在前面**（prepend），所以 takeLastClosedTab() 弹出来的是
    // "最近关掉的那一个"，连按 N 次就是"逆序恢复"，和浏览器 Ctrl+Shift+T 一致。
    //
    // 为什么这条栈放在 TabManager 而不是主窗口：
    //   所有关闭路径（× 按钮、菜单「关闭当前标签」、关闭其它/右侧/全部）最后都汇到
    //   closeTab()。放在这里，"记录"只需要写一处；放在主窗口就得在五六个入口各记一遍，
    //   将来加一个入口就会漏。
    bool canReopenClosedTab() const { return !m_closedTabs.isEmpty(); }

    // 弹出最近关闭的那条（栈空时返回一个 filePath 为空的记录）。
    // 这里是**弹出**不是"查看"：重开失败（文件被删/改名）也照样消耗掉一条 ——
    // 那条记录再留着也开不出来，只会让下一次 Ctrl+Shift+T 反复失败。
    ClosedTab takeLastClosedTab();

    // 当前栈里的内容（最近的在前）。给测试用；界面不需要看全栈。
    QList<ClosedTab> closedTabs() const { return m_closedTabs; }

signals:
    // 能不能重开的状态变了（0 条 ↔ 有 1 条以上）。动作的可用性直接挂在这上面，
    // 而不是每次关闭都无脑 setEnabled(true) —— 那样"没历史时按钮是灰的"就成了靠运气。
    void closedTabAvailabilityChanged(bool canReopen);

    // 当前标签换了（编辑器指针；没有标签时是 nullptr）。
    // 主窗口用它来切换预览内容、标题栏、状态栏。
    void currentEditorChanged(EditorWidget *editor);

    // 标签顺序变了（用户拖拽排序之后），带上新的标题顺序。
    void tabOrderChanged(const QStringList &titles);

private slots:
    void onCloseRequested(int index);   // 标签上的 × 被点了
    void onCurrentChanged(int index);   // QTabWidget 自带信号 → 转成 currentEditorChanged
    void onTabContextMenuRequested(const QPoint &pos);

private:
    // 每个标签除了显示文字，还记着"它的文档在磁盘上的哪个位置"：
    // 右键菜单里的"复制路径 / 在文件管理器中显示"要用它。
    // （QTabToolTip 里也有路径，但那是"顺便存"的，不该当数据来源。）
    struct TabMeta
    {
        QString filePath;
        QString fileName;
        // 关闭时要不要提醒"你当时有没保存的东西"（C3）。
        // 记在这里而不是现问编辑器：closeTab() 会把页面 delete 掉，
        // 那时候再去问编辑器就晚了。updateTab() 是唯一的写入点。
        bool modified = false;
    };

    // 关掉 index 之前把它的信息压进"最近关闭"栈（没有路径就不压，理由见 ClosedTab 的注释）。
    void recordClosedTab(int index);

    CloseConfirmHandler m_closeConfirm;
    QHash<const EditorWidget *, TabMeta> m_meta;  // 键是那个编辑器
    QList<ClosedTab> m_closedTabs;                // 最近的在前
};

#endif // TABMANAGER_H
