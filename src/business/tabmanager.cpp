#include "tabmanager.h"

#include "editorwidget.h"

#include <QAction>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QPoint>
#include <QTabBar>
#include <QUrl>

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

    // 右键菜单挂在**标签栏**上（挂在 QTabWidget 上会在页面区域也弹，那不合适）
    tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabBar(), &QTabBar::customContextMenuRequested, this, &TabManager::onTabContextMenuRequested);
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

    // 顺手记下"这个标签对应磁盘上的哪个文件"：右键菜单要用它
    if (EditorWidget *editor = editorAt(index)) {
        TabMeta meta;
        meta.filePath = info.filePath;
        meta.fileName = info.fileName;
        m_meta.insert(editor, meta);
    }
}

void TabManager::closeTab(int index)
{
    if (index < 0 || index >= count()) {
        return;
    }

    QWidget *page = widget(index);
    if (const EditorWidget *editor = qobject_cast<const EditorWidget *>(page)) {
        m_meta.remove(editor);  // 标签没了，它那份"路径记录"也要跟着走
    }
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

bool TabManager::requestCloseOtherTabs(int index)
{
    if (index < 0 || index >= count()) {
        return false;
    }

    // 倒着关：每次关掉一个，后面的索引都会前移 —— 从后往前就不受影响。
    // 跳过 index 那一项（它是要留下的）；如果它被关掉（比如前面有取消），
    // 这里按"当前剩余数量"重新算位置，保证语义是"只留原来那一个"。
    const EditorWidget *keep = editorAt(index);
    for (int i = count() - 1; i >= 0; --i) {
        if (editorAt(i) == keep) {
            continue;
        }
        if (!requestCloseTab(i)) {
            return false;  // 用户取消：已经关掉的不再恢复（和 requestCloseAllTabs 一致）
        }
    }
    return true;
}

bool TabManager::requestCloseTabsToRight(int index)
{
    if (index < 0 || index >= count()) {
        return false;
    }

    for (int i = count() - 1; i > index; --i) {
        if (!requestCloseTab(i)) {
            return false;
        }
    }
    return true;
}

QString TabManager::tabFilePath(int index) const
{
    if (const EditorWidget *editor = editorAt(index)) {
        return m_meta.value(editor).filePath;
    }
    return QString();
}

QString TabManager::tabFileName(int index) const
{
    if (const EditorWidget *editor = editorAt(index)) {
        return m_meta.value(editor).fileName;
    }
    return QString();
}

QMenu *TabManager::createTabContextMenu(int index)
{
    auto *menu = new QMenu(this);
    const bool valid = (index >= 0 && index < count());
    const QString path = tabFilePath(index);

    QAction *closeAction = menu->addAction(QStringLiteral("关闭标签"));
    closeAction->setEnabled(valid);
    connect(closeAction, &QAction::triggered, this, [this, index] { requestCloseTab(index); });

    QAction *closeOthers = menu->addAction(QStringLiteral("关闭其它标签"));
    closeOthers->setEnabled(valid && count() > 1);
    connect(closeOthers, &QAction::triggered, this, [this, index] { requestCloseOtherTabs(index); });

    QAction *closeRight = menu->addAction(QStringLiteral("关闭右侧标签"));
    closeRight->setEnabled(valid && index < count() - 1);
    connect(closeRight, &QAction::triggered, this, [this, index] { requestCloseTabsToRight(index); });

    QAction *closeAll = menu->addAction(QStringLiteral("全部关闭"));
    closeAll->setEnabled(count() > 0);
    connect(closeAll, &QAction::triggered, this, [this] { requestCloseAllTabs(); });

    menu->addSeparator();

    // 这两项需要"文件在磁盘上的位置"：还没有路径的标签（新建未保存）就用不了
    QAction *copyPath = menu->addAction(QStringLiteral("复制文件路径"));
    copyPath->setEnabled(!path.isEmpty());
    connect(copyPath, &QAction::triggered, this, [path] { QGuiApplication::clipboard()->setText(path); });

    QAction *reveal = menu->addAction(QStringLiteral("在文件管理器中显示"));
    reveal->setEnabled(!path.isEmpty());
    connect(reveal, &QAction::triggered, this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });

    return menu;
}

void TabManager::onTabContextMenuRequested(const QPoint &pos)
{
    const int index = tabBar()->tabAt(pos);
    if (index < 0) {
        return;  // 点在标签栏空白处：不弹菜单
    }

    // 右键顺手把那个标签选中：这样"关闭"之类的动作和视觉上高亮的是同一个（符合直觉）
    setCurrentIndex(index);

    QMenu *menu = createTabContextMenu(index);
    menu->exec(tabBar()->mapToGlobal(pos));
    delete menu;
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
