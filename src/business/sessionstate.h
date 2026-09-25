#ifndef SESSIONSTATE_H
#define SESSIONSTATE_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

// 窗口/会话状态的存取（6.2 的"窗口记忆"）。
//
// 存五样东西：
//   * 窗口几何（大小 + 位置 + 是否最大化）—— 用 QWidget::saveGeometry() 的字节块，
//     平台差异（多显示器、DPI）由 Qt 自己处理，比自己记 x/y/宽/高可靠
//   * 上次打开的那些文件（顺序也算）
//   * 当时正在看的是第几个（索引）
//   * 两个停靠面板是否可见
//   * C2：每个文件"上次看到哪"（光标行列 + 垂直滚动值）
//
// 为什么单独一个类而不是塞进 MainWindow：
//   * MainWindow 没法在测试里实例化（里面有 QWebEngineView，构造就要拉起 Chromium），
//     而"能不能正确存下来、读回来"是必须自动验证的东西；
//   * 这个类只依赖 ConfigManager（键值配置），是纯数据搬运，放 business 层正合适；
//   * 它**不认识 QWidget**：几何是一段不透明字节，交给窗口自己去 save/restoreGeometry；
//     光标位置也是三个整数，由窗口自己去读编辑器。
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
        // C4：大纲面板的可见性。默认 false（和搜索面板一致，不默认占地方）——
        // 而且它和文件树是同一个停靠区里的两页，默认停"文件"那一页。
        bool outlinePanelVisible = false;

        // C2：每个标签"上次看到哪"。key = 文件绝对路径，
        // value = "行|列|垂直滚动值"（见 makeViewState / parseViewState）。
        // 只记有路径的文件 —— 没保存过的新标签下次也开不出来，记了没意义。
        QHash<QString, QString> viewState;
    };

    // ---- 配置键名（写出来是为了让测试和"手改配置"的人有据可查）----
    static QString geometryKey();
    static QString openFilesKey();
    static QString currentIndexKey();
    static QString fileTreeVisibleKey();
    static QString searchPanelVisibleKey();
    static QString outlinePanelVisibleKey();
    static QString viewStateKey();

    // 从配置读回来。没有存过任何东西时返回默认值（geometry 为空、文件列表为空）。
    static Data load();

    // 写进配置。openFiles 为空时会把那个键删掉（不留空壳，和 ConfigManager 的约定一致）。
    // 为了让"下次启动"真的读到新值，最后会显式 sync() 一次 —— 关窗口时进程马上就要退出了。
    static void save(const Data &data);

    // 只把"这次要打开哪些文件"挑出来：丢掉空路径、去重（同一个文件只留一次）。
    // 抽成纯函数是因为"去重 + 保序 + 去空"正是最容易写错、又最该测的那点逻辑。
    static QStringList usableFiles(const QStringList &paths);

    // ---- C2：光标/滚动位置的编解码（纯函数，单独测）----
    //
    // 为什么存成字符串而不是"三个 int 的数组"：
    //   * QHash 不能直接进 INI；一条 QStringList（每项 "路径<TAB>行|列|滚动"）可以，
    //     而且**人在配置文件里能看懂**——出问题时能直接手改验证；
    //   * 三个数字放进一个字段，读回来时一次解析，不用在配置里占三个键。
    static QString makeViewState(int line, int column, int scrollValue);

    // 解析 makeViewState() 写出来的那串。**任何一项坏掉都返回 false**，
    // 由调用方当作"这个文件没有记过位置"处理 —— 一个存坏的坐标不该让启动失败。
    static bool parseViewState(const QString &value, int *line, int *column, int *scrollValue);

    // QHash <-> QStringList 的换算。坏行（没有分隔符、字段数不对）直接丢掉，
    // 不报错也不抛 —— 配置文件的容错原则和 load() 一致。
    static QStringList encodeViewState(const QHash<QString, QString> &state);
    static QHash<QString, QString> decodeViewState(const QStringList &lines);

    // 往里放一条：这三个数都取自编辑器，负数一律夹成 0（滚动条的值不会是负的）。
    static void setViewState(QHash<QString, QString> *state,
                             const QString &path,
                             int line,
                             int column,
                             int scrollValue);
};

#endif // SESSIONSTATE_H
