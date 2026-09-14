#ifndef EDITORWORKBENCH_H
#define EDITORWORKBENCH_H

#include <QList>
#include <QSplitter>
#include <QString>

class EditorWidget;
class QWebEngineView;
class QWidget;

namespace markdown_editor::core::document {
class PreviewRenderer;
class SyncBridge;
}

// 编辑工作台：左边编辑器、右边预览，两者之间的分屏与同步都在这里管。
//
// 它是一台"工作台"，把三件事收在一起（主窗口就不用再操心）：
//   1. **分屏布局**：用 QSplitter 左右排开，拖分隔条就能调比例；分隔条两侧都不许拖到 0 宽
//   2. **三种显示模式**：仅编辑 / 左右分屏 / 仅预览。
//      切换时**记住分屏比例**：从"仅编辑"切回"分屏"时不会变成一边倒的怪比例
//   3. **双向同步**：内部持有渲染管线（PreviewRenderer）和同步桥（SyncBridge），
//      并把它们接起来 —— 编辑器滚动 → 预览跟着滚；点预览 → 编辑器跳到对应源码行
//
// 为什么编辑器和预览不在这里创建：布局由 mainwindow.ui 描述（哪块放哪儿是设计问题），
// 这里只负责"给这两块加行为"。所以 setup() 接收外面建好的两块控件。
//
// 测试友好性（这不是巧合，是设计）：
//   setup() 的第二块参数只要是 QWidget 就行。**如果它不是 QWebEngineView**（比如测试里传一个
//   空 QWidget），那就不去碰 Chromium —— 分屏、三种模式、比例记忆、同步桥的接线全都能正常测；
//   渲染管线也照样存在，只是没附着页面，它的调用会安全地变成空操作。
//   这样这个部件在没有 Chromium 的环境（受限沙箱、纯命令行 CI）里也能验证。
class EditorWorkbench : public QSplitter
{
    Q_OBJECT

public:
    // 显示模式
    enum class ViewMode {
        Split,        // 左右分屏（默认）
        EditorOnly,   // 仅编辑
        PreviewOnly,  // 仅预览
    };
    Q_ENUM(ViewMode)

    explicit EditorWorkbench(QWidget *parent = nullptr);

    // 挂上左右两块控件：editorSide 通常是我们自己的 TabManager，previewSide 通常是 QWebEngineView。
    // 成功返回 true；任一为空返回 false（并且什么都不改）。
    // 如果 previewSide 确实是 QWebEngineView，会顺带：换页面（转发 JS 日志）→ 建 WebChannel →
    // 把同步桥注册给页面 → 加载预览模板。
    bool setup(QWidget *editorSide, QWidget *previewSide);

    QWidget *editorSide() const;
    QWidget *previewSide() const;

    // 预览视图（只有真正挂了 QWebEngineView 才有值）
    QWebEngineView *previewView() const;

    // 渲染管线与同步桥（永不返回 nullptr，即使还没 setup）
    markdown_editor::core::document::PreviewRenderer *renderer();
    markdown_editor::core::document::SyncBridge *bridge();

    // ---- 三种显示模式 ----
    ViewMode viewMode() const;
    void setViewMode(ViewMode mode);

    // ---- 同步 ----
    // 告诉工作台"现在编辑的是哪个编辑器"（多标签切换时调用）。
    // 它会接上这个编辑器的滚动条，并断开上一个的 —— 所以后台标签不会抢走预览。
    void setCurrentEditor(EditorWidget *editor);
    EditorWidget *currentEditor() const;

    // 把内容推给预览。baseDir 是文档所在目录（相对路径图片的基准）；
    // forceReload = true 表示"文档目录换了，必须重新加载模板"。
    // 不换目录时只推内容：页面不重载，所以切标签/切文档不会闪一下白屏。
    void showContent(const QString &markdown, const QString &baseDir = QString(), bool forceReload = false);

    QString previewBaseDir() const;

signals:
    void viewModeChanged(ViewMode mode);
    // 预览里被点了一下，行号从 1 起算。工作台已经替你把光标跳过去了，
    // 这个信号是给界面记日志/更新状态栏用的。
    void editorLineClicked(int line);

private:
    void applyViewMode(ViewMode mode);
    void rememberSplitSizes();     // 切到单栏之前记住比例
    void restoreSplitSizes();      // 切回分屏时恢复比例
    void syncScrollToPreview();    // 编辑器滚动 → 预览滚动

    QWidget *m_editorSide = nullptr;
    QWidget *m_previewSide = nullptr;
    QWebEngineView *m_previewView = nullptr;

    markdown_editor::core::document::PreviewRenderer *m_renderer = nullptr;
    markdown_editor::core::document::SyncBridge *m_bridge = nullptr;

    EditorWidget *m_editor = nullptr;      // 当前正在同步的那个编辑器
    ViewMode m_mode = ViewMode::Split;
    QList<int> m_savedSizes;               // 单栏模式下暂存的分屏比例
    QString m_previewBaseDir;              // 当前预览用的 baseUrl 目录
};

#endif // EDITORWORKBENCH_H
