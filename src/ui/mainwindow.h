#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QHash>
#include <QMainWindow>
#include <QString>

#include "editorworkbench.h"    // 显示模式的枚举类型出现在槽签名里，需要完整定义
#include "exporter.h"           // 5.6：导出器是整个窗口的一个成员（按值持有）
#include "filemanager.h"        // 会话表里用它的指针，但接口里出现它的类型，所以需要完整定义
#include "findreplacedialog.h"  // 查找/替换对话框是窗口的一个成员（按值持有）
#include "recentfiles.h"        // 5.4.2 的最近文件列表是整个窗口的一个成员（按值持有）
#include "sessionstate.h"       // 6.2：窗口/会话记忆（静态工具类）
#include "thememanager.h"       // 5.7：主题（槽签名里用它的枚举）

class EditorWidget;  // 业务层的编辑器控件（全局命名空间，和 MainWindow 一致）
class QAction;
class QActionGroup;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QMenu;
class QMimeData;

// uic 会把 mainwindow.ui 编译成 ui_mainwindow.h，里面是 namespace Ui { class MainWindow; }，
// 成员就是 .ui 里那些控件的指针（tabManager / preview / splitter / menubar / statusbar …）。
QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// 主窗口：中央是"编辑器 + 预览"的工作台，两侧/底部是可停靠的面板。
//
// 整体布局（.ui 里描述结构，行为在代码里）：
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │ 菜单栏：文件 / 编辑 / 视图 / 帮助                              │
//   │ 工具栏：新建 打开 保存 | 撤销 重做 | 预览 主题                  │
//   ├──────────┬───────────────────────────────────────────────────┤
//   │ 文件      │  中央区：EditorWorkbench（左：多标签编辑器        │
//   │ （左侧     │                        右：共享预览）             │
//   │  停靠面板）│                                                   │
//   ├──────────┴───────────────────────────────────────────────────┤
//   │ 全文搜索（底部停靠面板，默认隐藏）                             │
//   ├──────────────────────────────────────────────────────────────┤
//   │ 状态栏：行 x 列 y · 字符数 · 修改状态 · 路径 · 缓存            │
//   └──────────────────────────────────────────────────────────────┘
//
// 两块侧边面板都是 QDockWidget：可以拖到别的停靠区、也可以关掉（视图菜单里能再打开）。
// 这样做还有个副作用是好的：中央区分屏只剩"编辑器 + 预览"两块，比例不需要照顾第三块。
//
// 一个标签 = 一个会话（5.2 之后的结构）：
//   * 会话 = FileManager（内容/路径/脏标志/编码/缓存/历史）+ EditorWidget（那个标签的编辑器），
//     两者在 m_sessions 里配对。
//   * **预览只有一个**：所有标签共用同一个 QWebEngineView 和同一条渲染管线。
//     切标签只推内容不重载页面（不闪白、不丢滚动位置）。
//
// 数据流：
//   1) 打字 → FileManager::setText() 置脏 → PreviewRenderer::updateContent() → 防抖 300ms → 推给页面
//   2) 编辑器滚动 → 算出当前顶行 → SyncBridge 发信号 → 预览页里的 JS scrollToLine()
//   3) 预览被点击 → JS 调 SyncBridge::reportPreviewClick(行号) → 当前标签的光标跳过去
//   4) 保存 → 按原编码原子写盘 → 打一个历史快照 → 标签上的 * 消失
//
// 行号约定：**1 起算**（状态栏显示的和编辑器内部换算好的都是这个约定）。
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

    // ---- 拖拽打开的纯逻辑（不碰窗口，所以能单独测）----
    // 从拖进来的数据里挑出"可以打开的文件"：只收本地文件、只要 .md/.markdown/.txt、
    // 文件夹里的 .md 会被展开（拖一个目录进来也能用）。
    static QStringList droppedFiles(const QMimeData *data);

protected:
    // 关窗口：每个有未保存修改的标签都问一遍（和关标签用的是同一个 maybeSave），
    // 然后把窗口几何 + 这次打开的文件记进配置（下次启动恢复）
    void closeEvent(QCloseEvent *event) override;

    // ---- 拖拽打开（6.2）----
    // 把一个 .md 文件从资源管理器拖进窗口就能打开。只需要两步：
    //   dragEnterEvent 决定"要不要接受"（不接受的话鼠标会显示禁止图标）
    //   dropEvent      真正打开
    // dragMoveEvent **故意不重写**：默认实现会沿用 dragEnterEvent 给出的答案，
    // 而我们没有"拖到不同区域做不同事"的需求。
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onNewFile();       // 多标签时代 = 新建一个标签
    void onOpenFile();
    void onOpenFolder();    // 5.4.1：把文件树侧边栏切到某个目录
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

    // 导出当前文档（5.6）：弹出导出对话框，然后按选择的格式导出
    void onExportHtml();
    void onExportPdf();

    // 插入代码块（5.7）：先选语言，再把围栏插到当前光标处（预览会立刻按这种语言着色）
    void onInsertCodeBlock();

    // 主题切换（5.7）：把新主题应用到所有编辑器和预览区（菜单栏等控件由 QSS 自动跟）
    void onThemeChanged(ThemeManager::Theme theme);

    // 查找 / 替换（编辑菜单）：打开非模态的查找对话框，并指向当前标签
    void onFind();
    void onReplace();

    // 帮助 → 关于
    void onAbout();

    // 点了一条全文搜索结果（5.5）：打开那个文件并跳到那一行
    void onSearchResultActivated(const QString &filePath, int line);

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
    // 「编辑」菜单里的撤销/重做/剪切/复制/粘贴：作用于**当前标签**，可用状态也跟着它走
    void initEditActions();
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

    // ============================ 最近文件（5.4.2）============================
    // 按 m_recent 里的列表重建「文件 → 最近打开」子菜单。
    // 列表一变就整个重建：条目最多 10 个，重建比"精确增量更新"简单也难出错。
    void rebuildRecentMenu();
    // 打开一条最近记录。文件已经不在了就提示一句、并把它从列表里摘掉。
    bool openRecentFile(const QString &path);

    // ============================ 文件树侧边栏（5.4.1）============================
    // 让侧边栏跟着当前文档走：根目录已经是这个文件的上级时**不动**
    //（用户正在树下浏览，别把他的位置抢走），否则切到文件所在目录。
    void syncSidebarTo(const QString &filePath);

    // ============================ 全文搜索（5.5）============================
    // 搜索面板的默认目录跟侧边栏的根目录保持一致 —— 用户在侧边栏里看到的目录，
    // 就是搜索会去索引的目录，不需要在两处各选一遍。
    void syncSearchDirectoryToSidebar();

    // ============================ 导出（5.6）============================
    // 弹导出对话框 → 按用户的选择导出。dialogFormat 是预设的格式（菜单点哪一项就是哪种）。
    void exportCurrentDocument(bool asPdf);
    // 导出成功后问一句"要不要现在打开看看" —— 验收"HTML 浏览器打开正常"最省事的路径
    void offerToOpenExportedFile(const QString &path, bool asPdf);

    void updateWindowTitle();
    // 状态栏右侧那行缓存状态（当前标签的缓存：条数 / 命中次数 / 命中率）
    void updateCacheStatus();
    // 状态栏上的"当前文档"信息：行列、字符数、路径、修改状态。
    // 汇总成一个函数：这四样东西的刷新时机几乎一样（切标签/打字/保存/光标动），
    // 分散着更新迟早会漏一处。
    void updateDocumentStatus();

    // ============================ 窗口记忆（6.2）============================
    // 启动时恢复：窗口几何 + 上次打开的文件（磁盘上没了的跳过）+ 面板可见性
    void restoreSession();
    // 退出前保存（几何 + 当前打开的文件 + 当前索引 + 面板可见性）
    void saveSession() const;
    // 把当前所有标签里"有磁盘路径"的那些收集起来（顺序 = 标签顺序）
    QStringList openFilePaths() const;

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
    QAction *m_openFolderAction = nullptr;  // 5.4.1：选一个目录做文件树的根
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_exportHtmlAction = nullptr;  // 5.6：导出为 HTML
    QAction *m_exportPdfAction = nullptr;   // 5.6：导出为 PDF
    QAction *m_closeTabAction = nullptr;
    QAction *m_nextTabAction = nullptr;
    QAction *m_previousTabAction = nullptr;
    // ---- 编辑菜单（作用于当前标签）----
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_cutAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_selectAllAction = nullptr;
    QAction *m_findAction = nullptr;
    QAction *m_replaceAction = nullptr;
    QAction *m_aboutAction = nullptr;  // 帮助 → 关于
    QAction *m_aboutQtAction = nullptr;
    QAction *m_togglePreviewAction = nullptr;   // 工具栏：切换预览（显示/仅编辑）
    QAction *m_darkThemeAction = nullptr;       // 工具栏：切换主题（勾上 = 暗色）
    QAction *m_historyAction = nullptr;
    QAction *m_diffAction = nullptr;
    QAction *m_rollbackAction = nullptr;
    QAction *m_clearCacheAction = nullptr;
    QAction *m_viewSplitAction = nullptr;        // 视图：左右分屏
    QAction *m_viewEditorOnlyAction = nullptr;   // 视图：仅编辑
    QAction *m_viewPreviewOnlyAction = nullptr;  // 视图：仅预览
    QActionGroup *m_viewModeGroup = nullptr;     // 三个显示模式互斥
    QAction *m_showFileTreeAction = nullptr;     // 视图：显示/隐藏文件树侧边栏
    QAction *m_searchAction = nullptr;           // 视图：全文搜索面板（Ctrl+Shift+F）
    QAction *m_themeLightAction = nullptr;       // 视图 → 主题：亮色
    QAction *m_themeDarkAction = nullptr;        // 视图 → 主题：暗色
    QActionGroup *m_themeGroup = nullptr;        // 两个主题互斥

    QMenu *m_recentMenu = nullptr;  // 文件 →「最近打开」子菜单（内容随列表重建）

    // 最近文件列表（5.4.2）。按值持有：它就是主窗口状态的一部分，
    // 由 MainWindow 的构造函数统一 load()、由 changed() 信号驱动菜单重建。
    RecentFiles m_recent;

    // 导出器（5.6）。按值持有：一次导出一个任务，PDF 是异步的（结果靠 pdfExported 信号回来），
    // 成员活到窗口析构，不会出现"导出还没结束，对象先没了"的情况。
    Exporter m_exporter;

    // 查找/替换对话框（非模态，整个窗口只用一个实例）：
    // 复用同一个实例的好处是"上次搜的词还在"，不用每次重打。
    FindReplaceDialog m_findDialog;

    QLabel *m_cacheLabel = nullptr;     // 状态栏右侧：当前文档的缓存状态
    QLabel *m_cursorLabel = nullptr;    // 状态栏：行 x，列 y（来自 EditorWidget::cursorMoved）
    QLabel *m_charCountLabel = nullptr; // 状态栏：字符数
    QLabel *m_pathLabel = nullptr;      // 状态栏：当前文件路径（中间省略，可选中复制）
    QLabel *m_modifiedLabel = nullptr;  // 状态栏：已修改 / 已保存
};

#endif // MAINWINDOW_H
