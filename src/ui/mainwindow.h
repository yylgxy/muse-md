#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QHash>
#include <QMainWindow>
#include <QString>

#include "editorworkbench.h"  // 显示模式的枚举类型出现在槽签名里，需要完整定义
#include "filemanager.h"      // 会话表里用它的指针，但接口里出现它的类型，所以需要完整定义

class EditorWidget;  // 业务层的编辑器控件（全局命名空间，和 MainWindow 一致）
class QAction;
class QActionGroup;
class QLabel;

// uic 会把 mainwindow.ui 编译成 ui_mainwindow.h，里面是 namespace Ui { class MainWindow; }，
// 成员就是 .ui 里那些控件的指针（tabManager / preview / splitter / menubar / statusbar …）。
QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// 主窗口：左边多标签编辑器（每个标签一个文档）+ 右边共享预览。
//
// 5.2 之后的结构（和单文档时代最大的区别）：
//   * **一个标签 = 一个会话**：会话 = FileManager（文档内容/路径/脏标志/编码/缓存/历史）
//     + EditorWidget（那个标签的编辑器）。两者放在 m_sessions 里配对。
//   * **预览只有一个**：所有标签共用同一个 QWebEngineView 和同一个渲染管线。
//     切标签时把新文档的内容推过去；只有"文档目录变了"才重新加载模板，
//     否则页面不重载、不会闪一下白屏。
//   * 标签页本身的增删/排序/关闭确认机制在 TabManager 里；主窗口通过
//     setCloseConfirmHandler() 注入"要不要保存"的对话框（策略留在界面层）。
//
// 数据流（和单文档时代一样，只是"当前会话"而已）：
//   1) 打字 → FileManager::setText() 置脏 → PreviewRenderer::updateContent() → 防抖 300ms → 推给页面
//   2) 编辑器滚动 → 算出当前顶行 → SyncBridge 发信号 → 预览页里的 JS scrollToLine()
//   3) 预览被点击 → JS 调 SyncBridge::reportPreviewClick(行号) → 当前标签的光标跳过去
//   4) 保存 → 按原编码原子写盘 → 打一个历史快照 → 标签上的 * 消失
//
// 行号约定：**1 起算**。EditorWidget 内部已经把光标行列换算成 1 起算；
// 这里只在两处做 0/1 转换（onEditorScrolled 和 onPreviewClicked）。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // 头文件里少写点限定名：这个别名只在 MainWindow 内部可见，不会污染别处。
    // （在类的成员函数定义里也能用，所以 mainwindow.cpp 里不用再写一遍长名字。）
    using FileManager = markdown_editor::core::storage::FileManager;

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 打开一个 Markdown 文件（main() 用命令行参数调用，将来做文件关联也走这里）。
    // 已经打开过的文件不会再开一个标签，而是直接切过去。
    bool openFile(const QString &path);

protected:
    // 关窗口：每个有未保存修改的标签都问一遍（和关标签用的是同一个 maybeSave）
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onNewFile();       // 多标签时代 = 新建一个标签
    void onOpenFile();
    void onSaveFile();
    void onSaveFileAs();
    void onCloseTab();
    void onNextTab();
    void onPreviousTab();

    // 版本历史（4.2.2）：查历史快照 / 与上一版对比 / 回滚到历史版本（都作用于当前标签）
    void onShowHistory();
    void onDiffWithPrevious();
    void onRollbackToVersion();

    // 清空当前文档的内存缓存（4.2.3）
    void onClearCache();

    // 标签切换（TabManager 的信号；nullptr = 已经没有标签了）
    void onCurrentTabChanged(EditorWidget *editor);

    // 预览里被点了一下（工作台已经跳好光标了，这里只记日志/更新状态栏）
    void onEditorLineClicked(int line);

    // 显示模式变了（可能是代码改的，也可能由菜单触发）→ 同步菜单里的勾选状态
    void onViewModeChanged(EditorWorkbench::ViewMode mode);

private:
    // 把 .ui 建好的控件和外部对象（同步桥、WebChannel、渲染器、标签页）接起来
    void initUi();
    // 菜单/工具栏/状态栏：这些用 .ui 表达不了（快捷键、动作、连接都是代码的事），所以留在代码里
    void initMenuBar();
    void initToolBar();
    void initStatusBar();

    // ============================ 会话（一个标签 = 一个文档）============================

    // 新建一个会话：建 FileManager + 让 TabManager 开一个新标签，并接上所有信号。返回新的编辑器。
    EditorWidget *createSession();
    void connectSession(EditorWidget *editor, FileManager *files);

    EditorWidget *currentEditor() const;
    FileManager *filesFor(const EditorWidget *editor) const;  // 找不到返回 nullptr
    FileManager *currentFiles() const;
    EditorWidget *editorFor(const FileManager *files) const;  // 反查（关标签时要用）

    // 把文件名和修改标记刷到标签上（"笔记.md *"）
    void updateTabLabel(FileManager *files);
    // 让预览显示某个会话的内容。forceReload = 文档目录变了，需要重新加载模板。
    void showSession(FileManager *files, bool forceReload);
    // 回收一个会话（关标签时调用：把 FileManager 还回去，并从会话表里摘掉）
    void removeSession(EditorWidget *editor);

    // ---- 编辑器的信号（都显式带上"是哪个编辑器"，省得用 sender() 反查）----
    // 注意：滚动/点击这两个"编辑器 ↔ 预览"的同步现在归 EditorWorkbench 管，
    // 所以这里只剩内容变化和光标位置。
    void onEditorTextChanged(EditorWidget *editor);
    void onCursorMoved(EditorWidget *editor, int line, int column);

    // ---- FileManager 的信号 ----
    void onFileOpened(FileManager *files, const QString &path);
    void onFileSaved(FileManager *files, const QString &path);
    void onModificationChanged(FileManager *files, bool modified);
    void onReadOnlyDetected(FileManager *files, const QString &reason);

    // 有未保存的修改时先问一句（保存/放弃/取消）。返回 false = 用户取消，调用方必须中止。
    bool maybeSave(FileManager *files);
    // 保存某个会话（Ctrl+S 语义：没有路径时会转去另存为）。返回是否真的保存成功。
    bool saveSession(FileManager *files, EditorWidget *editor);
    bool saveSessionAs(FileManager *files, EditorWidget *editor);

    // 一个只读的文本窗口：历史列表和版本差异都用它显示
    // （差异可能几百行，需要等宽字体、不折行、可选可复制，QMessageBox 不够用）
    void showTextDialog(const QString &title, const QString &header, const QString &body);

    void updateWindowTitle();
    // 状态栏右侧那行缓存状态（当前标签的缓存：条数 / 命中次数 / 命中率）
    void updateCacheStatus();

    Ui::MainWindow *ui = nullptr;

    // 会话表：编辑器 → 它的文档管理器。
    // 两个对象的所有权都不在这里：EditorWidget 归标签页（TabManager 管理），
    // FileManager 是主窗口的子对象（parent = this）。所以这张表只是"看一眼"，
    // 关标签时要显式 removeSession()，否则会留下已销毁编辑器的悬空键。
    QHash<EditorWidget *, FileManager *> m_sessions;

    // 注意：预览渲染管线、同步桥、分屏比例、显示模式都在 EditorWorkbench（ui->workbench）里，
    // 主窗口只管文件与标签 —— 这是 5.3 把布局收进去之后的结果。

    QAction *m_newAction = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_nextTabAction = nullptr;
    QAction *m_previousTabAction = nullptr;
    QAction *m_historyAction = nullptr;
    QAction *m_diffAction = nullptr;
    QAction *m_rollbackAction = nullptr;
    QAction *m_clearCacheAction = nullptr;
    QAction *m_viewSplitAction = nullptr;        // 视图：左右分屏
    QAction *m_viewEditorOnlyAction = nullptr;   // 视图：仅编辑
    QAction *m_viewPreviewOnlyAction = nullptr;  // 视图：仅预览
    QActionGroup *m_viewModeGroup = nullptr;     // 三个显示模式互斥

    QLabel *m_cacheLabel = nullptr;   // 状态栏右侧的缓存状态（父对象是状态栏，生命周期归它）
    QLabel *m_cursorLabel = nullptr;  // 状态栏上的"行 x，列 y"（来自 EditorWidget::cursorMoved）
};

#endif // MAINWINDOW_H
