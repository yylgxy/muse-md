#ifndef SEARCHPANEL_H
#define SEARCHPANEL_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QWidget>

#include "fulltextsearch.h"  // 面板直接持有搜索服务（值成员），所以需要完整定义

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class SearchIndexWorker;

// 全文搜索面板（5.5）：选目录 → 建立索引 → 输入关键词 → 结果列表 → 点击跳转。
//
// 它和 FileTreeView 是同一个套路（业务层的界面部件）：
//   * **机制是公开函数**（setDirectory / buildIndex / runSearch / activateResult），
//     不弹任何对话框也能把整条流程跑通 —— 所以测试可以直接驱动它，不用模拟点击和弹窗。
//   * 只有"选择目录…"那个按钮会弹 QFileDialog（它本质是界面动作，不是机制）。
//   * 面板**不认识主窗口**：点到某条结果只发 resultActivated(命中)，
//     "打开文件、把光标移到那一行并把命中词选中"是主窗口的事。
//
// 结果列表是两级的：文件一行（粗体 + 命中数），它下面挂这个文件里的每条命中。
// 这样"列出匹配的文件和行号"这件事在界面上是一眼可读的。
// **单击某条命中即跳转**（需求原文就是"点击结果跳转"）。
class SearchPanel : public QWidget
{
    Q_OBJECT

public:
    // 结果条数上限：再多也没人一条条看，而且能防止"搜个空格"把界面撑死。
    static constexpr int kMaxResults = 500;

    explicit SearchPanel(QWidget *parent = nullptr);
    ~SearchPanel() override;

    // 索引库路径（测试用临时库；要在第一次建立索引/搜索之前调）
    void setIndexPath(const QString &path);

    // 搜索服务本体。主窗口/测试可以拿它看索引现状（文件数、行数）。
    FullTextSearch *engine();

    // ============================ 机制（都不弹窗）============================

    void setDirectory(const QString &dir);
    QString directory() const;

    // 建立/更新索引。**返回 true 的语义是"任务已提交"，不是"索引已完成"**
    //（A1 之后索引跑在工作线程里）。要等它结束，请看 isIndexing() 或 statusMessage 信号。
    // 失败（目录为空 / 索引库打不开）时返回 false，原因会同时进状态标签和 statusMessage。
    bool buildIndex();

    // 是否正在索引。抽出来是给测试用的：断言"状态"比断言"界面文案"稳 ——
    // 文案会改，状态不会（路线图 A1 Step 6 的建议做法）。
    bool isIndexing() const;

    // 搜索并填充结果列表，返回命中条数（0 = 没命中或关键词为空/索引为空）。
    int runSearch(const QString &query);
    void clearResults();

    // ============================ 结果（给界面和测试用）============================

    int hitCount() const;              // 命中条数
    int fileCount() const;             // 这些命中涉及几个文件
    QStringList resultFiles() const;   // 去重后的文件列表（按结果顺序）
    SearchHit hitAt(int index) const;  // 越界返回默认构造的空结果
    QString statusText() const;        // 底部状态标签上的字

    // "点开第 index 条结果"：发出 resultActivated。越界返回 false。
    // 单击结果行、回车、以及测试都走这一个入口 —— 行为只有一份。
    bool activateResult(int index);

    // 一行结果在列表里怎么显示：太长就围绕命中处截一段，两头加省略号。
    // 抽成 static 纯函数是为了能单独测"截断规则"（它属于显示规则，不该藏进槽函数里）。
    static QString displayTextFor(const SearchHit &hit, int maxChars = 120);

    // C1：命中词在**显示出来的那段文本**里的位置（[start, start+length)）。
    //
    // 为什么必须单独一个函数：displayTextFor() 会围绕命中处截断，截断之后
    // `hit.matchStart` 这个**原文里的下标**就不再对应显示文本的下标了。
    // 直接拿 matchStart 去高亮，长行（被截断的那些）会整体错位 —— 这是 C1
    // 最容易出的 bug，所以把"换算"抽成一个纯函数并单独测（见 test_fulltextsearch）。
    //
    // 返回值的 length 为 0 表示"这一段没有高亮可画"（没定位到命中，
    // 比如大小写差异导致 matchStart == -1）。
    struct DisplaySpan
    {
        int start = 0;
        int length = 0;
    };
    static DisplaySpan displaySpanFor(const SearchHit &hit, int maxChars = 120);

signals:
    // 用户点了一条结果：请打开这个文件的这一行，并把命中词选中。
    // C1 改成传整个 SearchHit（而不是原来的 (路径, 行号)）：命中词的位置
    // （matchStart / matchLength）也在里面，主窗口才能把单词选中并高亮 ——
    // 只给行号的话，跳过去光标只能落在行首，用户还得自己找那几个字。
    // 这一族结构需要能进信号槽，所以 fulltextsearch.h 里给它加了 Q_DECLARE_METATYPE。
    void resultActivated(const SearchHit &hit);
    // 状态标签上的字，顺便给主窗口显示到状态栏
    void statusMessage(const QString &text);

private slots:
    void onSearchRequested();
    void onBrowseClicked();
    void onIndexClicked();
    void onItemClicked(QTreeWidgetItem *item, int column);
    void onIndexProgress(int done, int total);
    void onIndexFinished(const IndexStats &stats);
    void onIndexCancelled();
    void onIndexFailed(const QString &message);

private:
    void fillResults(const QString &keyword);
    void setStatus(const QString &text);
    void setBusy(bool busy);
    void refreshIndexSummary();

    FullTextSearch m_engine;

    QLineEdit *m_query = nullptr;
    QLineEdit *m_dirEdit = nullptr;
    QPushButton *m_searchButton = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_indexButton = nullptr;
    QTreeWidget *m_results = nullptr;
    QLabel *m_status = nullptr;

    QList<SearchHit> m_hits;  // 与结果列表里的叶子行一一对应（item 的 UserRole 存下标）
    QString m_keyword;
    bool m_indexing = false;  // 正在索引：挡住重入（这时界面上的按钮是禁用的）

    // ---- 索引的写路径：工作线程（A1）----
    // m_engine 保留为**读侧**（GUI 线程）：搜索、看索引现状走它；
    // 写侧是 m_worker 内部的另一个 FullTextSearch 实例（活在 m_indexThread 里）。
    // 两个连接指向同一个库文件，靠 WAL 做到"边索引边搜索"。
    QThread m_indexThread;
    SearchIndexWorker *m_worker = nullptr;  // 归属 m_indexThread，**不是**面板的子对象
};

#endif // SEARCHPANEL_H
