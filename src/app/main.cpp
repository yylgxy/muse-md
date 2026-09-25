#include "logger.h"
#include "mainwindow.h"

#include <QApplication>

using markdown_editor::infrastructure::Logger;

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // ---- 产品名统一（D3）----
    // 对外显示的名字统一成 **muse-md**：和仓库名、README 标题、安装包名（MuseMD-setup）一致。
    // 面试官点开 GitHub 再跑一次程序，对不上是很掉分的细节。
    a.setApplicationDisplayName(QStringLiteral("muse-md"));

    // 但**存储标识**故意不动，仍然是 MarkdownEditor：
    // QStandardPaths::AppDataLocation 是用 applicationName 拼出来的
    //   （<AppData>/Dev/MarkdownEditor/），这里有用户的配置、全文索引、版本历史快照。
    // 一改名，这些数据不会消失，但程序再也找不到了 —— 在用户眼里就是"配置和历史都没了"。
    // 所以：**产品名与存储标识解耦**，显示叫 muse-md，落盘路径保持稳定。
    // （哪天要真的迁移，得写一次性的目录搬移 + 回滚方案，不能靠改这一行搞定。）
    a.setApplicationName("MarkdownEditor");
    a.setOrganizationName("Dev");
#ifdef APP_VERSION
    a.setApplicationVersion(QStringLiteral(APP_VERSION));
#endif

    // 初始化日志：控制台（Qt Creator「应用程序输出」面板）+ 文件，格式统一
    Logger::init();
    LOG_INFO("MarkdownEditor 启动");

    MainWindow w;
    w.show();
    LOG_INFO("主窗口已显示, 尺寸 %1x%2", w.width(), w.height());

    // 命令行可以带一个 .md 路径直接打开：MarkdownEditor.exe D:\notes\a.md
    // 用 QCoreApplication::arguments() 而不是 argv：Windows 上它能正确解出中文路径
    const QStringList args = QCoreApplication::arguments();
    if (args.size() > 1) {
        w.openFile(args.at(1));
    }

    const int exitCode = a.exec();
    LOG_INFO("事件循环结束, 返回码=%1", exitCode);

    Logger::shutdown();
    return exitCode;
}