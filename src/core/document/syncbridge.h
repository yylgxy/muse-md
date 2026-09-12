#ifndef SYNCBRIDGE_H
#define SYNCBRIDGE_H

#include <QList>
#include <QObject>
#include <QString>

namespace markdown_editor::core::document {

// 编辑区（QPlainTextEdit）与预览区（QWebEngineView 里的 JS）之间的"双向同步桥"。
//
// 为什么需要它：网页里的 JS 和 C++ 是两个互不相干的世界，默认谁也看不见谁。
// Qt 用 QWebChannel 架桥，用法是：
//
//     C++ 侧：channel->registerObject(QStringLiteral("syncBridge"), bridge);
//     JS  侧：channel.objects.syncBridge          // 名字必须和上面那个字符串一致
//
// 注册之后，这一类只需要"一对信号 + 一对槽"就能双向通：
//     * C++ 的**信号**能被 JS 用 .connect() 连上  → C++ 主动通知 JS
//     * C++ 的**槽函数**能被 JS 直接调用           → JS 主动通知 C++
//
// 行号约定：**统一 1 起算**（人类眼里的第几行）。
// QPlainTextEdit 的 blockNumber() 是 0 起算的，转换只在 MainWindow 的两处
// （+1 / -1）发生，别的任何地方都直接用 1 起算。
class SyncBridge : public QObject
{
    Q_OBJECT

public:
    explicit SyncBridge(QObject *parent = nullptr);

    // 扫描 Markdown 源码，算出"每个顶层块从第几行开始"（1 起算）。
    // 返回的数组要和渲染出的 HTML 里 #content 的顶层子元素**按顺序一一对应**。
    //
    // 为什么必须自己扫：md4c 的解析回调**不提供行号**（见 md4c.h 里的 MD_PARSER：
    // 只有类型 + detail，而 detail 结构里只有语义信息），所以这个映射只能自己算。
    //
    // 容错设计：算多/算少了只影响那几个没对齐的元素（JS 侧点击时退回到前一个已标注的块），
    // 不会让整篇文档错位；JS 在数量不一致时会 console.warn。
    static QList<int> buildLineMap(const QString &markdown);

signals:
    // C++ → JS：编辑器滚到了第 line 行，请预览跟着滚（模板里的 JS 连这个信号）
    void editorScrolled(int line);

    // C++ 内部：预览被点击，要求编辑器把光标跳到第 line 行
    void previewClicked(int line);

public slots:
    // 给 MainWindow 调用：报告编辑器当前顶行。信号只能由本类自己发射，
    // 所以外面要通过这个槽来"触发"信号。
    void reportEditorScroll(int line);

    // 给 JS 调用（WebChannel 把它暴露成 syncBridge.reportPreviewClick(line)）
    void reportPreviewClick(int line);
};

}  // namespace markdown_editor::core::document

#endif // SYNCBRIDGE_H
