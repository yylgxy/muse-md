// FileTreeView（5.4.1 文件树侧边栏）的契约测试。
//
// 需要 QApplication（QWidget 依赖它；QFileSystemModel 还依赖事件循环）。
//
// 这里有个刻意的设计：FileTreeView 把三个真正的文件操作（createFile / renamePath /
// removePath）做成了**不弹窗的公开函数**，右键菜单只是"问一句再调它们"的壳。
// 所以测试可以直接调它们验证行为 —— 不需要去模拟"用户点了菜单、又在输入框里打了字"，
// 也不需要有人点弹窗（无窗口环境下弹窗是死路）。
// 菜单本身也测：造出来看看有哪些动作、什么时候该禁用 —— 菜单也是行为。
//
// 测试全程在临时目录里干活（md-editor-filetree-test/），不碰项目文件。
//
// 跑法：ctest -C Debug --output-on-failure

#include "filetreeview.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QMenu>
#include <QModelIndex>
#include <QString>

#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-62s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// 路径比较：Windows 上大小写不敏感、分隔符也可能不一样，所以统一 cleanPath + 小写比较
bool samePath(const QString &a, const QString &b)
{
    return QString::compare(QDir::cleanPath(a), QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

bool touch(const QString &path, const QByteArray &content = QByteArray("x"))
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(content);
    file.close();
    return true;
}

// QFileSystemModel 的内容是异步填充的（有自己的监听线程），所以"等它看到"要转事件循环。
// 这是测试里唯一需要等的地方；文件操作本身是同步的。
bool waitForRows(QFileSystemModel *model, const QModelIndex &parent, int wanted, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (model->rowCount(parent) >= wanted) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}

// 拿到某个路径在模型里的 index。QFileSystemModel 的内容是异步填充的（它有自己的监听线程），
// 所以刚设完根目录时子项可能还没到位 —— 这里转一下事件循环等它，最多等 timeoutMs。
QModelIndex indexOf(QFileSystemModel *model, const QString &path, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (true) {
        const QModelIndex index = model->index(path);
        if (index.isValid()) {
            return index;
        }
        if (timer.elapsed() >= timeoutMs) {
            return QModelIndex();
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

QString actionTexts(const QMenu *menu)
{
    QStringList texts;
    const QList<QAction *> actions = menu->actions();
    for (QAction *action : actions) {
        texts << (action->isSeparator() ? QStringLiteral("---") : action->text());
    }
    return texts.join(QStringLiteral(" / "));
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString base = QDir::tempPath() + QStringLiteral("/md-editor-filetree-test");
    QDir(base).removeRecursively();
    if (!QDir().mkpath(base)) {
        std::printf("无法建立临时目录：%s\n", base.toUtf8().constData());
        return 1;
    }
    const QString subDir = base + QStringLiteral("/sub");
    QDir().mkpath(subDir);
    const QString aMd = base + QStringLiteral("/a.md");
    const QString bMd = subDir + QStringLiteral("/b.md");
    touch(aMd);
    touch(bMd);

    FileTreeView view;

    // ============================ A. 初始配置 ============================
    {
        std::printf("---- A. 初始配置 ----\n");
        check(view.model() != nullptr && view.fileSystemModel() != nullptr,
              QStringLiteral("构造后就有模型（不用外部再设一遍）"));
        check(view.model() == view.fileSystemModel(),
              QStringLiteral("模型就是 QFileSystemModel（自带目录监听，不用手工刷新）"));
        check(qobject_cast<QFileSystemModel *>(view.model()) != nullptr,
              QStringLiteral("模型类型正确"));

        check(view.isHeaderHidden(), QStringLiteral("侧边栏不显示表头"));
        check(view.isColumnHidden(1) && view.isColumnHidden(2) && view.isColumnHidden(3),
              QStringLiteral("只留名字列（大小/类型/时间三列都藏起来）"));
        check(!view.isColumnHidden(0), QStringLiteral("名字列当然要显示"));
        check(view.editTriggers() == QAbstractItemView::NoEditTriggers,
              QStringLiteral("行内编辑关掉：重命名只有右键菜单一条路"));
        check(view.contextMenuPolicy() == Qt::CustomContextMenu,
              QStringLiteral("用的是自定义右键菜单"));
        check(view.rootPath().isEmpty() && !view.rootIndex().isValid(),
              QStringLiteral("初始：没有根目录（rootIndex 无效，rootPath 为空）"));
        check(view.fileSystemModel()->filter().testFlag(QDir::NoDotAndDotDot)
                  && view.fileSystemModel()->filter().testFlag(QDir::AllEntries),
              QStringLiteral("过滤规则：显示所有条目但不显示 . 和 .."));
    }

    // ============================ B. 根目录 ============================
    {
        std::printf("---- B. 根目录 ----\n");
        view.setRootPath(base);
        check(samePath(view.rootPath(), base), QStringLiteral("setRootPath: rootPath() 返回设进去的目录"),
              view.rootPath());
        check(view.rootIndex().isValid(), QStringLiteral("setRootPath: rootIndex 变成有效"));
        check(samePath(view.fileSystemModel()->rootPath(), base),
              QStringLiteral("setRootPath: 模型的监听目录也切过去了"));

        const QModelIndex root = view.rootIndex();
        const bool loaded = waitForRows(view.fileSystemModel(), root, 2);
        check(loaded, QStringLiteral("目录内容被加载出来（a.md 和 sub）"),
              QStringLiteral("rows=%1").arg(view.fileSystemModel()->rowCount(root)));

        view.setRootPath(QString());
        check(view.rootPath().isEmpty() && !view.rootIndex().isValid(),
              QStringLiteral("setRootPath(空): 退回「没有根」，不会崩"));

        view.setRootPath(base);  // 后面几节都用它
        check(samePath(view.rootPath(), base), QStringLiteral("再设回来"));
    }

    // ============================ C. 查询 ============================
    {
        std::printf("---- C. 路径与选中项 ----\n");
        QFileSystemModel *model = view.fileSystemModel();

        const QModelIndex fileIndex = indexOf(model, aMd);
        check(fileIndex.isValid(), QStringLiteral("model->index(a.md) 有效"));

        // 下面这条正是"双击打开"要用的能力：从树里的一个 index 反查磁盘路径
        check(samePath(view.pathForIndex(fileIndex), aMd),
              QStringLiteral("pathForIndex: 从 index 拿到文件路径"), view.pathForIndex(fileIndex));
        check(view.pathForIndex(QModelIndex()).isEmpty(),
              QStringLiteral("pathForIndex(无效 index): 空字符串，不崩"));

        view.setCurrentIndex(fileIndex);
        check(samePath(view.selectedPath(), aMd), QStringLiteral("selectedPath: 跟随当前选中项"),
              view.selectedPath());

        view.setCurrentIndex(QModelIndex());
        check(view.selectedPath().isEmpty(), QStringLiteral("selectedPath: 没有选中项时是空的"));
    }

    // ============================ D. 新建文件 ============================
    {
        std::printf("---- D. createFile ----\n");
        QString createdPath;
        int createdSignals = 0;
        QObject::connect(&view, &FileTreeView::fileCreated, [&](const QString &path) {
            ++createdSignals;
            createdPath = path;
        });

        const QString newMd = base + QStringLiteral("/new.md");
        QString error = QStringLiteral("占位");
        check(view.createFile(base, QStringLiteral("new.md"), &error),
              QStringLiteral("新建: 成功返回 true"), error);
        check(error.isEmpty(), QStringLiteral("新建: 成功时 error 被清空"));
        check(QFileInfo::exists(newMd), QStringLiteral("新建: 文件真的在磁盘上了"), newMd);
        check(QFileInfo(newMd).size() == 0, QStringLiteral("新建: 是一个空文件（内容交给编辑器）"));
        check(createdSignals == 1 && samePath(createdPath, newMd),
              QStringLiteral("新建: 发了 fileCreated(路径)"), createdPath);

        check(!view.createFile(base, QStringLiteral("new.md"), &error),
              QStringLiteral("新建同名: 失败（不覆盖已有文件）"));
        check(error.contains(QStringLiteral("已经存在")), QStringLiteral("新建同名: 原因说得清楚"), error);

        check(!view.createFile(QString(), QStringLiteral("x.md"), &error),
              QStringLiteral("新建: 目录为空 → 失败"));
        check(!view.createFile(base, QStringLiteral("   "), &error),
              QStringLiteral("新建: 文件名为空（或只有空格）→ 失败"));
        check(!view.createFile(base + QStringLiteral("/没有这个目录"), QStringLiteral("x.md"), &error)
                  && !error.isEmpty(),
              QStringLiteral("新建: 目录不存在 → 失败并给出原因"), error);

        // 名字两边的空格会被去掉（用户手打时很常见）
        check(view.createFile(base, QStringLiteral("  spaced.md  "), &error),
              QStringLiteral("新建: 名字两边的空格自动去掉"), error);
        check(QFileInfo::exists(base + QStringLiteral("/spaced.md")),
              QStringLiteral("新建: 落地的名字是去掉空格后的"));
        check(createdSignals == 2, QStringLiteral("新建: 又发了一次信号"),
              QStringLiteral("signals=%1").arg(createdSignals));
    }

    // ============================ E. 重命名 ============================
    {
        std::printf("---- E. renamePath ----\n");
        const QString oldPath = base + QStringLiteral("/new.md");
        const QString renamedTo = base + QStringLiteral("/renamed.md");

        QString oldSeen;
        QString newSeen;
        int renameSignals = 0;
        QObject::connect(&view, &FileTreeView::fileRenamed, [&](const QString &o, const QString &n) {
            ++renameSignals;
            oldSeen = o;
            newSeen = n;
        });

        QString error;
        check(view.renamePath(oldPath, QStringLiteral("renamed.md"), &error),
              QStringLiteral("重命名: 成功"), error);
        check(renameSignals == 1 && samePath(oldSeen, oldPath) && samePath(newSeen, renamedTo),
              QStringLiteral("重命名: 发了 fileRenamed(旧, 新)"),
              QStringLiteral("%1 → %2").arg(oldSeen, newSeen));
        check(!QFileInfo::exists(oldPath) && QFileInfo::exists(renamedTo),
              QStringLiteral("重命名: 旧名字没了、新名字在"));

        check(!view.renamePath(renamedTo, QStringLiteral("a.md"), &error),
              QStringLiteral("重命名到一个已存在的名字: 失败（不覆盖）"));
        check(error.contains(QStringLiteral("已经存在")), QStringLiteral("重命名: 原因说得清楚"), error);
        check(renameSignals == 1, QStringLiteral("重命名失败: 不发信号"));

        check(view.renamePath(renamedTo, QStringLiteral("renamed.md"), &error),
              QStringLiteral("重命名成同一个名字: 算成功（不算失败）"));
        check(renameSignals == 1, QStringLiteral("重命名成同一个名字: 没必要发信号"));

        check(!view.renamePath(base + QStringLiteral("/没有这个东西.md"), QStringLiteral("x.md"), &error),
              QStringLiteral("重命名一个不存在的东西: 失败"));
        check(error.contains(QStringLiteral("不存在")), QStringLiteral("重命名: 说清是文件不存在"), error);

        error.clear();
        check(!view.renamePath(renamedTo, QStringLiteral("  "), &error),
              QStringLiteral("重命名: 新名字为空 → 失败"));
        check(error.contains(QStringLiteral("不能为空")), QStringLiteral("重命名: 说清新名字为空"), error);

        // 目录也能改名（同目录内改名，QFile::rename 对目录一样管用）
        check(view.renamePath(subDir, QStringLiteral("sub2"), &error),
              QStringLiteral("重命名目录: 也支持"), error);
        check(QFileInfo(base + QStringLiteral("/sub2")).isDir(),
              QStringLiteral("重命名目录: 目录确实改了名"));
        check(samePath(newSeen, base + QStringLiteral("/sub2")),
              QStringLiteral("重命名目录: 信号里的新路径对"), newSeen);
    }

    // ============================ F. 删除 ============================
    {
        std::printf("---- F. removePath ----\n");
        int removedSignals = 0;
        QString removedPath;
        QObject::connect(&view, &FileTreeView::fileRemoved, [&](const QString &path) {
            ++removedSignals;
            removedPath = path;
        });

        const QString doomed = base + QStringLiteral("/spaced.md");
        QString error;
        check(view.removePath(doomed, &error), QStringLiteral("删除文件: 成功"), error);
        check(!QFileInfo::exists(doomed), QStringLiteral("删除文件: 磁盘上真没了"));
        check(removedSignals == 1 && samePath(removedPath, doomed),
              QStringLiteral("删除文件: 发了 fileRemoved(路径)"), removedPath);

        check(!view.removePath(doomed, &error), QStringLiteral("再删一次: 失败（本来就不存在）"));
        check(error.contains(QStringLiteral("不存在")), QStringLiteral("删除: 说清是文件不存在"), error);
        check(removedSignals == 1, QStringLiteral("删除失败: 不发信号"));

        check(!view.removePath(QString(), &error), QStringLiteral("删除空路径: 失败而不是崩"));

        // 目录是**递归**删的：这就是为什么右键菜单里必须再确认一次
        const QString deepDir = base + QStringLiteral("/deep");
        QDir().mkpath(deepDir + QStringLiteral("/inner"));
        touch(deepDir + QStringLiteral("/inner/keep.md"));
        check(view.removePath(deepDir, &error), QStringLiteral("删除目录: 成功"), error);
        check(!QFileInfo::exists(deepDir) && !QFileInfo::exists(deepDir + QStringLiteral("/inner/keep.md")),
              QStringLiteral("删除目录: 里面的东西也一起没了（递归）"));
        check(removedSignals == 2, QStringLiteral("删除目录: 也发信号"));
    }

    // ============================ G. 右键菜单 ============================
    {
        std::printf("---- G. 右键菜单 ----\n");
        const QString filePath = base + QStringLiteral("/renamed.md");
        const QString dirPath = base + QStringLiteral("/sub2");

        QMenu *fileMenu = view.createContextMenu(filePath);
        const QList<QAction *> fileActions = fileMenu->actions();
        check(fileActions.size() == 6, QStringLiteral("菜单结构: 4 个动作 + 2 条分隔线"),
              actionTexts(fileMenu));
        check(fileActions.at(0)->text().startsWith(QStringLiteral("新建")),
              QStringLiteral("菜单: 第一项是「新建 Markdown 文件…」"), fileActions.at(0)->text());
        check(fileActions.at(1)->isSeparator() && fileActions.at(4)->isSeparator(),
              QStringLiteral("菜单: 分隔线在预期位置"));
        check(fileActions.at(2)->text().startsWith(QStringLiteral("重命名")) && fileActions.at(2)->isEnabled(),
              QStringLiteral("点在文件上: 「重命名…」可用"));
        check(fileActions.at(3)->text() == QStringLiteral("删除文件…") && fileActions.at(3)->isEnabled(),
              QStringLiteral("点在文件上: 「删除文件…」可用"), fileActions.at(3)->text());
        check(fileActions.at(5)->text().startsWith(QStringLiteral("在文件管理器中显示"))
                  && fileActions.at(5)->isEnabled(),
              QStringLiteral("菜单: 「在文件管理器中显示」可用"));
        delete fileMenu;

        QMenu *dirMenu = view.createContextMenu(dirPath);
        const QList<QAction *> dirActions = dirMenu->actions();
        check(dirActions.at(3)->text() == QStringLiteral("删除目录…"),
              QStringLiteral("点在目录上: 第三项变成「删除目录…」"), dirActions.at(3)->text());
        check(dirActions.at(2)->isEnabled(), QStringLiteral("点在目录上: 也能重命名"));
        delete dirMenu;

        // 点在空白处（没有目标）：能新建，但重命名/删除/显示都没有对象
        QMenu *blankMenu = view.createContextMenu(QString());
        const QList<QAction *> blankActions = blankMenu->actions();
        check(blankActions.at(0)->isEnabled(), QStringLiteral("空白处: 「新建」仍可用（落在根目录）"));
        check(!blankActions.at(2)->isEnabled() && !blankActions.at(3)->isEnabled()
                  && !blankActions.at(5)->isEnabled(),
              QStringLiteral("空白处: 重命名/删除/显示都禁用（没有目标）"));
        check(blankActions.at(3)->text() == QStringLiteral("删除文件…"),
              QStringLiteral("空白处: 删除项按「文件」提示（没有目标可判断）"));
        delete blankMenu;
    }

    // ============================ H. 双击打开 ============================
    {
        std::printf("---- H. fileActivated（双击/回车）----\n");
        QFileSystemModel *model = view.fileSystemModel();

        QString activatedPath;
        int activatedSignals = 0;
        QObject::connect(&view, &FileTreeView::fileActivated, [&](const QString &path) {
            ++activatedSignals;
            activatedPath = path;
        });

        // onActivated 是私有槽，但它照样在元对象系统里，可以直接按名字调用 ——
        // 这样测的是"树发了 activated 之后本类怎么处理"，不用去造真实鼠标事件。
        const QModelIndex fileIndex = indexOf(model, base + QStringLiteral("/a.md"));
        check(fileIndex.isValid(), QStringLiteral("准备: 拿到 a.md 的 index"));
        check(QMetaObject::invokeMethod(&view, "onActivated", Q_ARG(QModelIndex, fileIndex)),
              QStringLiteral("invokeMethod: 私有槽能被元对象系统调到"));
        check(activatedSignals == 1 && samePath(activatedPath, base + QStringLiteral("/a.md")),
              QStringLiteral("双击文件: 发 fileActivated(路径)"), activatedPath);

        // 目录用根目录本身来试：它一定有效，不用等模型刷新
        const QModelIndex dirIndex = view.rootIndex();
        check(dirIndex.isValid() && model->isDir(dirIndex), QStringLiteral("准备: 根目录的 index 是个目录"));
        QMetaObject::invokeMethod(&view, "onActivated", Q_ARG(QModelIndex, dirIndex));
        check(activatedSignals == 1,
              QStringLiteral("双击目录: **不发** fileActivated（目录是展开/折叠，不该开成标签）"));

        const QModelIndex invalidIndex;
        QMetaObject::invokeMethod(&view, "onActivated", Q_ARG(QModelIndex, invalidIndex));
        check(activatedSignals == 1, QStringLiteral("激活无效 index: 什么都不做，更不会崩"));
    }

    // 收尾：先把根目录摘掉再删临时目录，否则 QFileSystemModel 的监听线程
    // 会在目录消失时报一句 "FindNextChangeNotification failed"（无害，但输出难看）
    view.setRootPath(QString());
    QDir(base).removeRecursively();

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
