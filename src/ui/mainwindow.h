#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>

#include "markdowndocument.h"  // 值成员，需要完整类型

class QAction;
class QTimer;
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
// 数据流（4.1.3 的核心）：
//   1) 编辑器改动 → MarkdownDocument（脏标记 + HTML 缓存）→ MarkdownParser 渲染
//      → runJavaScript 把「HTML + 行号表」推给预览页
//   2) 编辑器滚动 → 算出当前顶行 → SyncBridge 发 editorScrolled 信号
//      → 预览页里的 JS scrollToLine() 跟着滚
//   3) 预览被点击 → JS 调 SyncBridge::reportPreviewClick(行号)
//      → previewClicked 信号 → 编辑器光标跳到那一行
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
    void onOpenFile();
    void onSaveFile();
    void onSaveFileAs();

    void onEditorTextChanged();
    void onEditorScrolled();
    void onPreviewClicked(int line);
    void onPreviewLoadFinished(bool ok);

    // 防抖到期后真正刷新预览
    void refreshPreview();

private:
    // 把 .ui 建好的控件和外部对象（高亮器、同步桥、WebChannel）接起来
    void initUi();
    // 菜单/工具栏/状态栏：这些用 .ui 表达不了（快捷键、动作、连接都是代码的事），所以留在代码里
    void initMenuBar();
    void initToolBar();
    void initStatusBar();

    // 加载预览"外壳"页面（模板 + baseUrl）。baseDir 是当前文档所在目录：
    // 文档里的 ![](./img/a.png) 这类相对路径要靠 baseUrl 才能找到磁盘文件。
    void loadPreviewPage(const QString &baseDir);
    // 把渲染好的 HTML 和"每个顶层块的行号"推给页面里的 JS
    void pushContentToPreview();
    void updateWindowTitle();

    Ui::MainWindow *ui = nullptr;

    MarkdownHighlighter *m_highlighter = nullptr;
    markdown_editor::core::document::SyncBridge *m_bridge = nullptr;

    markdown_editor::core::document::MarkdownDocument m_document;  // 文档模型（含 HTML 缓存）

    QTimer *m_previewTimer = nullptr;  // 预览刷新防抖：敲字时别每个字符都重新渲染
    bool m_previewReady = false;       // 预览页面（和 WebChannel）是否已就绪

    QAction *m_openAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
};

#endif // MAINWINDOW_H
