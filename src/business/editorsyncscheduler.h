#ifndef EDITORSYNCSCHEDULER_H
#define EDITORSYNCSCHEDULER_H

#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

class EditorWidget;

// 「编辑器 → 文档管理器」同步的节流器（性能优化：P0-1）。
//
// 为什么需要它：
//   主窗口原来在**每次 textChanged**（也就是每敲一个键）里做
//       files->setText(editor->toPlainText());
//   而 toPlainText() 会把整篇文档深拷贝一遍，紧接着 setText 还要跟旧内容
//   全串比较一遍。三十万字符的文档上，每敲一个字都要付这笔钱 —— 打字会明显地"黏"。
//
//   但"文档管理器里的内容"其实只在几个时刻才真的需要是最新的：
//   保存、关闭、切标签、导出、渲染预览。所以这里改成：
//     * 打字时只记下"这个编辑器脏了"（O(1)，不碰文本）；
//     * 停手 delayMs（默认 150ms）之后统一同步一次；
//     * 上面那些关键时刻调用 flushAll() 立刻同步，用户绝不会"保存到旧内容"。
//
// 为什么单独一个类而不是塞在主窗口里：主窗口没法在测试里实例化（里面有
// QWebEngineView，构造就要拉起 Chromium），而"连续敲十个键只同步一次""flush 真的
// 立刻同步了""标签关掉后不会再回调已销毁的对象"这些正是必须自动验证的东西。
class EditorSyncScheduler : public QObject
{
    Q_OBJECT

public:
    // 真正干活的回调：主窗口在这里做 files->setText(editor->toPlainText())
    using SyncHandler = std::function<void(EditorWidget *)>;

    static constexpr int kDefaultDelayMs = 150;

    explicit EditorSyncScheduler(QObject *parent = nullptr);

    void setSyncHandler(SyncHandler handler);
    void setDelay(int ms);   // 0 = 不延迟（每次 markDirty 立刻同步；测试用得上）
    int delay() const;

    // 打字时调用：O(1)，只是标记一下并（重新）启动定时器
    void markDirty(EditorWidget *editor);

    // 标签被关掉 / 编辑器即将销毁时调用：把它从待同步列表里摘掉，
    // 免得定时器到点时去碰一个已经析构的对象。
    void forget(EditorWidget *editor);

    bool isPending(const EditorWidget *editor) const;
    int pendingCount() const;

    // 立刻同步所有欠着的（保存/关闭/切标签/导出之前调用）
    void flushAll();

signals:
    // 每次真正同步完一个编辑器发一次（给日志和测试看"到底同步了几次"）
    void synced(EditorWidget *editor);

private:
    SyncHandler m_handler;
    // 用 QPointer：编辑器被销毁后指针自动变空，比"记住原始指针 + 手工摘除"更抗错
    QList<QPointer<EditorWidget>> m_pending;
    QTimer m_timer;
};

#endif // EDITORSYNCSCHEDULER_H
