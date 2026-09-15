#include "filetreeview.h"

#include "logger.h"

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QUrl>

FileTreeView::FileTreeView(QWidget *parent) : QTreeView(parent)
{
    // QFileSystemModel 自带目录监听：别的程序增删文件它会自己刷新，所以不用担心"树是旧的"。
    m_model = new QFileSystemModel(this);
    // 只要名字列；隐藏大小/类型/时间那三列（侧边栏没那么宽）
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    setModel(m_model);

    for (int column = 1; column < m_model->columnCount(); ++column) {
        hideColumn(column);
    }
    setHeaderHidden(true);          // 侧边栏不需要表头
    setUniformRowHeights(true);     // 大目录时滚动更顺（每行等高）
    setAnimated(false);
    // 重命名只走右键菜单这一条路：把行内编辑关掉，行为就不存在"两条路不一致"的问题
    setEditTriggers(QAbstractItemView::NoEditTriggers);

    // activated：双击或回车都会发；目录交给 QTreeView 自己展开/折叠
    connect(this, &QTreeView::activated, this, &FileTreeView::onActivated);

    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QTreeView::customContextMenuRequested, this, &FileTreeView::onContextMenuRequested);
}

// ============================ 根目录 ============================

void FileTreeView::setRootPath(const QString &dir)
{
    if (dir.isEmpty()) {
        setRootIndex(QModelIndex());
        return;
    }

    // setRootPath() 让模型开始监听这个目录；setRootIndex() 才决定"树从哪儿显示起"
    const QModelIndex rootIndex = m_model->setRootPath(dir);
    setRootIndex(rootIndex);
    expand(rootIndex);  // 顺手展开一层，省得用户还得点一下
}

QString FileTreeView::rootPath() const
{
    if (!rootIndex().isValid()) {
        return QString();
    }
    return m_model->filePath(rootIndex());
}

QFileSystemModel *FileTreeView::fileSystemModel() const
{
    return m_model;
}

// ============================ 查询 ============================

QString FileTreeView::pathForIndex(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return QString();
    }
    return m_model->filePath(index);
}

QString FileTreeView::selectedPath() const
{
    return pathForIndex(currentIndex());
}

QString FileTreeView::targetDirectoryFor(const QString &path) const
{
    if (path.isEmpty()) {
        // 没点到任何东西：落在根目录（没有根就用当前目录兜底）
        const QString root = rootPath();
        return root.isEmpty() ? QDir::currentPath() : root;
    }

    const QFileInfo info(path);
    if (info.isDir()) {
        return info.absoluteFilePath();
    }
    return info.absolutePath();  // 点在文件上 → 用它的父目录
}

// ============================ 文件操作（不弹窗）============================

bool FileTreeView::createFile(const QString &dirPath, const QString &fileName, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    const QString name = fileName.trimmed();
    if (dirPath.isEmpty() || name.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("目录或文件名为空");
        }
        return false;
    }

    const QString path = QDir(dirPath).filePath(name);
    if (QFileInfo::exists(path)) {
        // 不覆盖已有文件：用户没说"我要覆盖"，覆盖就是数据丢失
        if (error != nullptr) {
            *error = QStringLiteral("同名文件已经存在：%1").arg(path);
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    file.close();  // 只建一个空文件，内容交给编辑器

    LOG_INFO("新建文件: %1", path);
    emit fileCreated(path);
    return true;
}

bool FileTreeView::removePath(const QString &path, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    const QFileInfo info(path);
    if (!info.exists()) {
        if (error != nullptr) {
            *error = QStringLiteral("文件不存在：%1").arg(path);
        }
        return false;
    }

    const bool ok = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
    if (!ok) {
        if (error != nullptr) {
            *error = QStringLiteral("删不掉（可能正被占用，或者没有权限）：%1").arg(path);
        }
        return false;
    }

    LOG_INFO("已删除: %1", path);
    emit fileRemoved(path);
    return true;
}

bool FileTreeView::renamePath(const QString &path, const QString &newName, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    const QFileInfo info(path);
    if (!info.exists()) {
        if (error != nullptr) {
            *error = QStringLiteral("文件不存在：%1").arg(path);
        }
        return false;
    }

    const QString name = newName.trimmed();
    if (name.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("新名字不能为空");
        }
        return false;
    }

    const QString newPath = info.absoluteDir().filePath(name);
    if (newPath == info.absoluteFilePath()) {
        return true;  // 名字根本没变，不算失败
    }
    if (QFileInfo::exists(newPath)) {
        if (error != nullptr) {
            *error = QStringLiteral("已经存在同名的东西：%1").arg(newPath);
        }
        return false;
    }

    // QFile::rename 对文件和目录都能用（同目录内改名）
    if (!QFile::rename(path, newPath)) {
        if (error != nullptr) {
            *error = QStringLiteral("重命名失败（可能正被占用，或者没有权限）：%1").arg(path);
        }
        return false;
    }

    LOG_INFO("已重命名: %1 -> %2", path, newPath);
    emit fileRenamed(path, newPath);
    return true;
}

// ============================ 菜单与"先问再干" ============================

QMenu *FileTreeView::createContextMenu(const QString &path)
{
    auto *menu = new QMenu(this);
    const bool hasTarget = !path.isEmpty();
    const bool isDir = hasTarget && QFileInfo(path).isDir();

    QAction *createAction = menu->addAction(QStringLiteral("新建 Markdown 文件…"));
    connect(createAction, &QAction::triggered, this, [this, path] { promptCreateFile(path); });

    menu->addSeparator();

    QAction *renameAction = menu->addAction(QStringLiteral("重命名…"));
    renameAction->setEnabled(hasTarget);
    connect(renameAction, &QAction::triggered, this, [this, path] { promptRename(path); });

    QAction *removeAction = menu->addAction(isDir ? QStringLiteral("删除目录…") : QStringLiteral("删除文件…"));
    removeAction->setEnabled(hasTarget);
    connect(removeAction, &QAction::triggered, this, [this, path] { promptRemove(path); });

    menu->addSeparator();

    QAction *revealAction = menu->addAction(QStringLiteral("在文件管理器中显示"));
    revealAction->setEnabled(hasTarget);
    connect(revealAction, &QAction::triggered, this, [this, path] {
        const QFileInfo info(path);
        const QString dir = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    return menu;
}

void FileTreeView::promptCreateFile(const QString &path)
{
    const QString dir = targetDirectoryFor(path);

    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("新建文件"),
                                               QStringLiteral("在 %1 里新建：").arg(dir),
                                               QLineEdit::Normal,
                                               QStringLiteral("新建文档.md"),
                                               &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }

    QString error;
    const QString created = QDir(dir).filePath(name.trimmed());
    if (!createFile(dir, name, &error)) {
        reportError(QStringLiteral("新建失败"), error);
        return;
    }

    // 建完顺手打开它：用户的意图通常就是"新建并开始写"
    emit fileActivated(created);
}

void FileTreeView::promptRename(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }

    const QFileInfo info(path);
    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("重命名"),
                                               QStringLiteral("新名字："),
                                               QLineEdit::Normal,
                                               info.fileName(),
                                               &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }

    QString error;
    if (!renamePath(path, name, &error)) {
        reportError(QStringLiteral("重命名失败"), error);
    }
}

void FileTreeView::promptRemove(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }

    const QFileInfo info(path);
    const QString message =
        info.isDir()
            ? QStringLiteral("确定要删除整个目录吗？\n\n%1\n\n目录里的所有东西都会一起删掉，删了找不回来。").arg(path)
            : QStringLiteral("确定要删除这个文件吗？\n\n%1\n\n删了找不回来。").arg(path);

    const QMessageBox::StandardButton answer = QMessageBox::warning(this,
                                                                    QStringLiteral("删除确认"),
                                                                    message,
                                                                    QMessageBox::Yes | QMessageBox::No,
                                                                    QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!removePath(path, &error)) {
        reportError(QStringLiteral("删除失败"), error);
    }
}

void FileTreeView::reportError(const QString &title, const QString &message)
{
    LOG_WARN("%1: %2", title, message);
    emit errorOccurred(message);
    QMessageBox::warning(this, title, message);
}

// ============================ 事件 ============================

void FileTreeView::onActivated(const QModelIndex &index)
{
    if (!index.isValid() || m_model->isDir(index)) {
        return;  // 目录交给 QTreeView 自己展开/折叠
    }
    emit fileActivated(m_model->filePath(index));
}

void FileTreeView::onContextMenuRequested(const QPoint &pos)
{
    const QString path = pathForIndex(indexAt(pos));
    QMenu *menu = createContextMenu(path);
    menu->exec(viewport()->mapToGlobal(pos));
    delete menu;  // 菜单是自己 new 的，用完要还回去
}
