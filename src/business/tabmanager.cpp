#include "tabmanager.h"

#include "editorwidget.h"

#include <QTabBar>

TabManager::TabManager(QWidget *parent) : QTabWidget(parent)
{
    // 可以关（标签上出现 ×）、可以拖（拖拽排序）
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);  // 视觉上更像"文档标签"而不是"工具箱抽屉"

    // 标签上的 × → 先问一声再关（问的逻辑在 requestCloseTab 里）
    connect(this, &QTabWidget::tabCloseRequested, this, &TabManager::onCloseRequested);

    // QTabWidget 自带 currentChanged(int)，这里转成"当前编辑器指针"更好用：
    // 主窗口关心的是"现在编辑的是哪个文档"，不是索引数字。
    connect(this, &QTabWidget::currentChanged, this, &TabManager::onCurrentChanged);

    // 拖拽排序之后 QTabBar 会发 tabMoved；转发成我们自己的信号
    connect(tabBar(), &QTabBar::tabMoved, this, [this](int, int) { emit tabOrderChanged(tabTitles()); });
}

void TabManager::setCloseConfirmHandler(CloseConfirmHandler handler)
{
    m_closeConfirm = std::move(handler);
}

EditorWidget *TabManager::addEditorTab(const QString &title)
{
    auto *editor = new EditorWidget(this);
    editor->setPlaceholderText(QStringLiteral("在这里写 Markdown…"));

    const QString label = title.isEmpty() ? QStringLiteral("未命名") : title;
    const int index = addTab(editor, label);
    setCurrentIndex(index);   // 新标签立刻成为当前标签（会触发 currentChanged → 发出信号）
    editor->setFocus();       // 光标直接落在新标签里，用户不用再点一下

    return editor;
}

EditorWidget *TabManager::editorAt(int index) const
{
    return qobject_cast<EditorWidget *>(widget(index));
}

EditorWidget *TabManager::currentEditor() const
{
    return editorAt(currentIndex());
}

int TabManager::indexOf(const EditorWidget *editor) const
{
    if (editor == nullptr) {
        return -1;
    }
    // 显式写基类：不然这里会递归调用自己（QTabWidget::indexOf 的参数是 const QWidget*）
    return QTabWidget::indexOf(editor);
}

void TabManager::updateTab(int index, const TabInfo &info)
{
    if (index < 0 || index >= count()) {
        return;
    }

    const QString name = info.fileName.isEmpty() ? QStringLiteral("未命名") : info.fileName;
    // 修改标记放在名字后面：一眼能看出哪个标签还有没保存的东西
    setTabText(index, info.modified ? name + QStringLiteral(" *") : name);

    const QString tip = info.filePath.isEmpty()
                            ? (info.title.isEmpty() ? name : info.title)
                            : info.filePath;
    setTabToolTip(index, tip);
}

void TabManager::closeTab(int index)
{
    if (index < 0 || index >= count()) {
        return;
    }

    QWidget *page = widget(index);
    removeTab(index);   // 只从标签栏摘掉，不释放页面
    delete page;        // 页面是我们 new 的（不在 .ui 里），得自己还回去

    // 如果关掉的正好是当前标签，removeTab 会让 QTabWidget 发 currentChanged，
    // 我们的 onCurrentChanged 会跟着发 currentEditorChanged —— 所以这里不用再补一发。
}

void TabManager::closeCurrentTab()
{
    closeTab(currentIndex());
}

bool TabManager::requestCloseTab(int index)
{
    EditorWidget *editor = editorAt(index);
    if (editor == nullptr) {
        return false;
    }

    // 没注入回调时就当"用户同意"：这样在没有界面的场景（比如测试、将来的命令行模式）
    // 也能用，不会因为没人接手而卡住。
    if (m_closeConfirm && !m_closeConfirm(editor)) {
        return false;  // 用户取消（比如在保存提示里点了「取消」）
    }

    closeTab(index);
    return true;
}

bool TabManager::requestCloseAllTabs()
{
    // 倒着问：每关掉一个，后面的索引都会前移，从后往前就不受影响
    for (int index = count() - 1; index >= 0; --index) {
        if (!requestCloseTab(index)) {
            return false;
        }
    }
    return true;
}

QStringList TabManager::tabTitles() const
{
    QStringList titles;
    titles.reserve(count());
    for (int i = 0; i < count(); ++i) {
        titles.append(tabText(i));
    }
    return titles;
}

void TabManager::onCloseRequested(int index)
{
    requestCloseTab(index);
}

void TabManager::onCurrentChanged(int index)
{
    emit currentEditorChanged(editorAt(index));
}
