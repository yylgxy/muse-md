#ifndef FILETREEVIEW_H
#define FILETREEVIEW_H

#include <QString>
#include <QTreeView>

class QFileSystemModel;
class QMenu;
class QModelIndex;
class QPoint;

// 文件树侧边栏（5.4.1）。
//
// 基于 QTreeView + QFileSystemModel：显示一个工作目录，双击（或回车）一个文件就打开它。
// QFileSystemModel 自带目录监听 —— 别的程序增删了文件它会自己刷新，不用我们手工刷新列表。
//
// 右键菜单：新建 Markdown 文件 / 重命名 / 删除 / 在文件管理器中显示。
//
// 关于"本类弹对话框"这一点：三个文件操作都得先问用户（文件名、确认删除），
// 所以菜单那部分就在本类里弹窗（它本质是界面部件，和 TabManager 一样放在 business 层）。
// 但**真正的文件操作被抽成了公开函数**（createFile / renamePath / removePath）：
// 它们不弹任何对话框、失败只返回 false + 原因，所以测试可以直接调它们验证行为，
// 不必去模拟用户点菜单；主窗口也能复用它们（比如将来做"重命名当前文档"）。
class FileTreeView : public QTreeView
{
    Q_OBJECT

public:
    explicit FileTreeView(QWidget *parent = nullptr);

    // 显示哪个目录。空字符串 = 不设根（从整个文件系统开始）。
    void setRootPath(const QString &dir);
    QString rootPath() const;

    QFileSystemModel *fileSystemModel() const;

    // ============================ 真正的文件操作 ============================
    // 都不弹窗：失败时返回 false，并把人能看懂的原因写进 *error（若给了非空指针）。
    // dirPath 里用 / 还是 \ 都行（Qt 自己会处理）。

    // 新建一个空文件。同名文件已存在时**不覆盖**，直接失败并说明原因。
    bool createFile(const QString &dirPath, const QString &fileName, QString *error = nullptr);

    // 删除文件或目录（目录是递归删除的，调用方负责先跟用户确认）。
    bool removePath(const QString &path, QString *error = nullptr);

    // 重命名（文件、目录都行，只改名字、不换目录）。
    bool renamePath(const QString &path, const QString &newName, QString *error = nullptr);

    // ============================ 查询与菜单 ============================

    QString selectedPath() const;                  // 当前选中项的路径；没选中返回空
    QString pathForIndex(const QModelIndex &index) const;

    // 造一份右键菜单（调用方负责 delete）。公开出来是为了让"菜单里有哪些动作、
    // 什么时候该禁用"也能被测到 —— 菜单本身也是行为。
    QMenu *createContextMenu(const QString &path);

    // 右键菜单会用到的那几个"先问再干"的入口，公开出来方便将来加到主菜单里
    void promptCreateFile(const QString &path);
    void promptRename(const QString &path);
    void promptRemove(const QString &path);

signals:
    // 双击/回车了一个**文件**（目录不会触发，那是展开/折叠）
    void fileActivated(const QString &path);
    void fileCreated(const QString &path);
    void fileRemoved(const QString &path);
    void fileRenamed(const QString &oldPath, const QString &newPath);
    // 出错了。本类自己会弹窗提示，这个信号是给主窗口记日志/更新状态栏用的。
    void errorOccurred(const QString &message);

private slots:
    void onActivated(const QModelIndex &index);
    void onContextMenuRequested(const QPoint &pos);

private:
    // 右键点在文件上时，"新建"应该落在它所在的目录里
    QString targetDirectoryFor(const QString &path) const;
    void reportError(const QString &title, const QString &message);

    QFileSystemModel *m_model = nullptr;
};

#endif // FILETREEVIEW_H
