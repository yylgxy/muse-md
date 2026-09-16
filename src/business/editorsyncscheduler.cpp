#include "editorsyncscheduler.h"

#include "editorwidget.h"
#include "logger.h"

EditorSyncScheduler::EditorSyncScheduler(QObject *parent) : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(kDefaultDelayMs);
    connect(&m_timer, &QTimer::timeout, this, &EditorSyncScheduler::flushAll);
}

void EditorSyncScheduler::setSyncHandler(SyncHandler handler)
{
    m_handler = std::move(handler);
}

void EditorSyncScheduler::setDelay(int ms)
{
    const int delay = qMax(0, ms);
    m_timer.setInterval(delay);
    // 延迟改成 0：这个项目里"0 延迟"的语义是"立刻做"，而不是"下一轮事件循环做" ——
    // 否则测试里还得转事件循环才能看到同步发生，用起来别扭。
    if (delay == 0) {
        flushAll();
    }
}

int EditorSyncScheduler::delay() const
{
    return m_timer.interval();
}

void EditorSyncScheduler::markDirty(EditorWidget *editor)
{
    if (editor == nullptr) {
        return;
    }

    if (!isPending(editor)) {
        m_pending.append(QPointer<EditorWidget>(editor));
    }

    if (m_timer.interval() == 0) {
        flushAll();  // 不延迟模式：立刻同步
        return;
    }

    // 重新启动定时器 = "从最后一次输入算起再等 150ms"。
    // 连续打字时它会被一直往后推，停手之后才真正同步一次 —— 和预览的防抖是同一个思路。
    m_timer.start();
}

void EditorSyncScheduler::forget(EditorWidget *editor)
{
    if (editor == nullptr) {
        return;
    }

    for (int i = m_pending.size() - 1; i >= 0; --i) {
        if (m_pending.at(i) == editor) {
            m_pending.removeAt(i);
        }
    }

    if (m_pending.isEmpty()) {
        m_timer.stop();  // 没有欠着的了就别再空转一个定时器
    }
}

bool EditorSyncScheduler::isPending(const EditorWidget *editor) const
{
    if (editor == nullptr) {
        return false;
    }
    for (const QPointer<EditorWidget> &pending : m_pending) {
        if (pending == editor) {
            return true;
        }
    }
    return false;
}

int EditorSyncScheduler::pendingCount() const
{
    return m_pending.size();
}

void EditorSyncScheduler::flushAll()
{
    m_timer.stop();

    if (m_pending.isEmpty()) {
        return;
    }

    // 先整体取出来再清空：回调里可能会 markDirty（理论上不会），
    // 那样也不该打断这一轮已经在处理的列表。
    const QList<QPointer<EditorWidget>> pending = m_pending;
    m_pending.clear();

    for (const QPointer<EditorWidget> &editor : pending) {
        if (editor.isNull()) {
            continue;  // 已经销毁了（标签关掉了）：跳过，不要去碰它
        }
        if (m_handler) {
            m_handler(editor.data());
        }
        emit synced(editor.data());
    }
}
