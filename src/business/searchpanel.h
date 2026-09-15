#ifndef SEARCHPANEL_H
#define SEARCHPANEL_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "fulltextsearch.h"  // 面板直接持有搜索服务（值成员），所以需要完整定义

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// 全文搜索面板（5.5）：选目录 → 建立索引 → 输入关键词 → 结果列表 → 点击跳转。
//
// 它和 FileTreeView 是同一个套路（业务层的界面部件）：
//   * **机制是公开函数**（setDirectory / buildIndex / runSearch / activateResult），
//     不弹任何对话框也能把整条流程跑通 —— 所以测试可以直接驱动它，不用模拟点击和弹窗。
//   * 只有"选择目录…"那个按钮会弹 QFileDialog（它本质是界面动作，不是机制）。
//   * 面板**不认识主窗口**：点到某条结果只发 resultActivated(路径, 行号)，
//     "打开文件、把光标移到那一行"是主窗口的事。
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

    // 索引库路径（测试用临时库；要在第一次建立索引/搜索之前调）
    void setIndexPath(const QString &path);

    // 搜索服务本体。主窗口/测试可以拿它看索引现状（文件数、行数）。
    FullTextSearch *engine();

    // ============================ 机制（都不弹窗）============================

    void setDirectory(const QString &dir);
    QString directory() const;

    // 建立/更新索引。失败时返回 false，原因会同时进状态标签和 statusMessage 信号。
    bool buildIndex();

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

signals:
    // 用户点了一条结果：请打开这个文件的这一行（行号 1 起算）
    void resultActivated(const QString &filePath, int line);
    // 状态标签上的字，顺便给主窗口显示到状态栏
    void statusMessage(const QString &text);

private slots:
    void onSearchRequested();
    void onBrowseClicked();
    void onIndexClicked();
    void onItemClicked(QTreeWidgetItem *item, int column);
    void onIndexProgress(int done, int total);
    void onIndexFinished(const IndexStats &stats);

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
};

#endif // SEARCHPANEL_H
