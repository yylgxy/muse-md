#ifndef SESSIONSTATE_H
#define SESSIONSTATE_H

#include <QByteArray>
#include <QString>
#include <QStringList>

// 窗口/会话状态的存取（6.2 的"窗口记忆"）。
//
// 存四样东西：
//   * 窗口几何（大小 + 位置 + 是否最大化）—— 用 QWidget::saveGeometry() 的字节块，
//     平台差异（多显示器、DPI）由 Qt 自己处理，比自己记 x/y/宽/高可靠
//   * 上次打开的那些文件（顺序也算）
//   * 当时正在看的是第几个（索引）
//   * 两个停靠面板是否可见
//
// 为什么单独一个类而不是塞进 MainWindow：
//   * MainWindow 没法在测试里实例化（里面有 QWebEngineView，构造就要拉起 Chromium），
//     而"能不能正确存下来、读回来"是必须自动验证的东西；
//   * 这个类只依赖 ConfigManager（键值配置），是纯数据搬运，放 business 层正合适；
//   * 它**不认识 QWidget**：几何是一段不透明字节，交给窗口自己去 save/restoreGeometry。
//
// 容错原则：读回来的东西一律要能用。缺键就是默认值；上次打开的文件在磁盘上没了，
// 由调用方（主窗口）在打开时跳过 —— 这里只负责"如实读回来"。
class SessionState
{
public:
    struct Data
    {
        QByteArray geometry;        // 空 = 没有存过，窗口用默认大小
        QStringList openFiles;      // 上次打开的文件（绝对路径，按标签顺序）
        int currentIndex = 0;       // 当时正在看第几个
        bool fileTreeVisible = true;
        bool searchPanelVisible = false;
    };

    // ---- 配置键名（写出来是为了让测试和"手改配置"的人有据可查）----
    static QString geometryKey();
    static QString openFilesKey();
    static QString currentIndexKey();
    static QString fileTreeVisibleKey();
    static QString searchPanelVisibleKey();

    // 从配置读回来。没有存过任何东西时返回默认值（geometry 为空、文件列表为空）。
    static Data load();

    // 写进配置。openFiles 为空时会把那个键删掉（不留空壳，和 ConfigManager 的约定一致）。
    // 为了让"下次启动"真的读到新值，最后会显式 sync() 一次 —— 关窗口时进程马上就要退出了。
    static void save(const Data &data);

    // 只把"这次要打开哪些文件"挑出来：丢掉空路径、去重（同一个文件只留一次）。
    // 抽成纯函数是因为"去重 + 保序 + 去空"正是最容易写错、又最该测的那点逻辑。
    static QStringList usableFiles(const QStringList &paths);
};

#endif // SESSIONSTATE_H
