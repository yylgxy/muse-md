#include "logger.h"
#include "mainwindow.h"

#include <QApplication>

using markdown_editor::infrastructure::Logger;

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
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

    const int exitCode = a.exec();
    LOG_INFO("事件循环结束, 返回码=%1", exitCode);

    Logger::shutdown();
    return exitCode;
}