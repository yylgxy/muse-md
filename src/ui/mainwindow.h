#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>

#include "filemanager.h"      // 值成员，需要完整类型
#include "previewrenderer.h"  // 值成员，需要完整类型

class QAction;
class MarkdownHighlighter;  // 全局命名空间的类（命名空间不统一的遗留）

namespace markdown_editor::core::document {
class SyncBridge;
}

// uic 会把 mainwindow.ui 编译成 ui_mainwindow.h，里面是 namespace Ui { class MainWindow; }，
// 成员就是 .ui 里那些控件的指针（editor / preview / splitter / menubar / statusbar …）。
QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// 主窗口：左边编辑器 + 右边预览，两边双向同步。
//
// 界面写法是"混合式"（真实 Qt 项目最主流的做法）：
//   * mainwindow.ui  —— 只管"有哪些控件、怎么摆"（splitter + editor + preview）
//   * 本文件的代码    —— 管所有"行为"：菜单/工具栏/动作（快捷键是 C++ 常量）、
//                       信号槽接线、WebChannel 注册、防抖、JS 调用、行号换算
//     .ui 省不掉这些，所以它们留在代码里。
//
// 三个部件各管一段（主窗口只做接线和弹窗）：
//   FileManager     —— 文件层面：打开/保存/另存为/新建、编码、只读、修改标志（4.2.1）
//   PreviewRenderer —— 渲染管线：模板、防抖、解析、推给预览页（4.1.4）
//   SyncBridge      —— 双向同步：编辑器滚动 ↔ 预览点击换行号（4.1.3）
//
// 数据流：
//   1) 打开文件 → FileManager 读盘判编码 → text() 灌进编辑器
//      → 编辑器 textChanged → FileManager::setText() 置脏 → PreviewRenderer::updateContent()
//      → 防抖 300ms → 渲染 → JS 替换预览内容
//   2) 编辑器滚动 → 算出当前顶行 → SyncBridge 发 editorScrolled 信号
//      → 预览页里的 JS scrollToLine() 跟着滚
//   3) 预览被点击 → JS 调 SyncBridge::reportPreviewClick(行号)
//      → previewClicked 信号 → 编辑器光标跳到那一行
//   4) 保存 → FileManager 按原编码原子写盘 → fileSaved 信号 → 标题栏/状态栏更新
//
// 行号约定：**1 起算**。这里只有两处做 0/1 转换（onEditorScrolled 和 onPreviewClicked）。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 打开一个 Markdown 文件（main() 用命令行参数调用，将来做文件关联也走这里）
    bool openFile(const QString &path);

private slots:
    void onNewFile();
    void onOpenFile();
    void onSaveFile();
    void onSaveFileAs();

    void onEditorTextChanged();
    void onEditorScrolled();
    void onPreviewClicked(int line);

    // FileManager 的信号
    void onFileOpened(const QString &path);
    void onFileSaved(const QString &path);
    void onModificationChanged(bool modified);
    void onReadOnlyDetected(const QString &path, const QString &reason);

private:
    // 把 .ui 建好的控件和外部对象（高亮器、文件管理器、同步桥、WebChannel、渲染器）接起来
    void initUi();
    // 菜单/工具栏/状态栏：这些用 .ui 表达不了（快捷键、动作、连接都是代码的事），所以留在代码里
    void initMenuBar();
    void initToolBar();
    void initStatusBar();

    // 有未保存的修改时先问一句（保存/放弃/取消）。
    // 返回 false = 用户取消，调用方必须**中止**当前操作，否则就把没保存的内容丢了。
    bool maybeSave();

    void updateWindowTitle();

    Ui::MainWindow *ui = nullptr;

    MarkdownHighlighter *m_highlighter = nullptr;
    markdown_editor::core::document::SyncBridge *m_bridge = nullptr;

    // 文档内容 + 磁盘路径 + 脏标志 + 编码 + 只读状态（唯一事实来源，别再在别处存一份）
    markdown_editor::core::storage::FileManager m_files;

    // 渲染管线：模板加载、页面持有、防抖、渲染、推送（4.1.4）
    markdown_editor::core::document::PreviewRenderer m_renderer;

    QAction *m_newAction = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
};

#endif // MAINWINDOW_H
