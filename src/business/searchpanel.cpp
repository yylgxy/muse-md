#include "searchpanel.h"

#include "logger.h"
#include "searchindexworker.h"
#include "searchresultdelegate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPoint>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

SearchPanel::SearchPanel(QWidget *parent) : QWidget(parent)
{
    // ---------------- 上半部分：搜索框 + 目录 + 两个按钮 ----------------
    m_query = new QLineEdit(this);
    m_query->setPlaceholderText(QStringLiteral("输入关键词，回车搜索"));
    m_query->setClearButtonEnabled(true);

    m_searchButton = new QPushButton(QStringLiteral("搜索"), this);
    m_searchButton->setDefault(true);

    m_dirEdit = new QLineEdit(this);
    m_dirEdit->setPlaceholderText(QStringLiteral("要索引的目录"));

    m_browseButton = new QPushButton(QStringLiteral("选择…"), this);
    m_indexButton = new QPushButton(QStringLiteral("建立索引"), this);

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->addWidget(m_query, 1);
    searchRow->addWidget(m_searchButton);

    auto *dirRow = new QHBoxLayout;
    dirRow->setContentsMargins(0, 0, 0, 0);
    dirRow->addWidget(m_dirEdit, 1);
    dirRow->addWidget(m_browseButton);
    dirRow->addWidget(m_indexButton);

    // ---------------- 结果列表：两级（文件 → 命中行）----------------
    m_results = new QTreeWidget(this);
    m_results->setColumnCount(2);
    m_results->setHeaderLabels({QStringLiteral("位置"), QStringLiteral("内容")});
    m_results->setRootIsDecorated(true);
    m_results->setUniformRowHeights(true);
    m_results->setAlternatingRowColors(true);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_results->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    // C1：内容列要能只高亮"命中的那几个字"，而 QTreeWidgetItem 只能给整格上色，
    // 所以这一列交给自绘的 delegate（理由见 searchresultdelegate.h 顶部）。
    m_results->setItemDelegate(new SearchResultDelegate(m_results));
    // ★ 关掉省略号：结果文本已经由 displayTextFor() 截断过一次（最多 maxChars + 两个…），
    //   再让视图自己 elide 一次的话，delegate 算出来的"命中词横向偏移"就和实际画出来的
    //   文字对不上了（视图省略后前面那段变短，高亮块会往右偏）。
    //   关掉之后超宽的内容是**裁剪**而不是省略 —— 对已经截断过的短文本没有实际影响。
    m_results->setTextElideMode(Qt::ElideNone);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    layout->addLayout(searchRow);
    layout->addLayout(dirRow);
    layout->addWidget(m_results, 1);
    layout->addWidget(m_status);

    // ---------------- 接线 ----------------
    connect(m_searchButton, &QPushButton::clicked, this, &SearchPanel::onSearchRequested);
    connect(m_query, &QLineEdit::returnPressed, this, &SearchPanel::onSearchRequested);
    connect(m_browseButton, &QPushButton::clicked, this, &SearchPanel::onBrowseClicked);
    connect(m_indexButton, &QPushButton::clicked, this, &SearchPanel::onIndexClicked);
    // 单击（不是双击）就跳转：需求的原文是"点击结果跳转到对应文件的对应行"
    connect(m_results, &QTreeWidget::itemClicked, this, &SearchPanel::onItemClicked);

    // ---------------- 索引的工作线程（A1）----------------
    // m_engine 从此只当**读侧**（搜索、看索引现状），它的进度/结束信号不再由面板直接接：
    // 扫描已经改由工作线程里的另一个实例负责，面板只跟 worker 说话。
    m_worker = new SearchIndexWorker;        // 主线程创建
    m_worker->moveToThread(&m_indexThread);  // 之后它的槽都在工作线程里跑

    // ★ 顺序很重要：先 shutdown（DirectConnection，立刻在当前线程执行完），
    //   再 deleteLater（排进事件队列）。反过来的话对象可能已经被删，shutdown 就成了野指针调用。
    //   finished 是在"正在结束的那个线程"里发出的 —— 所以 DirectConnection 让它跑在工作线程里，
    //   这正是 QSqlDatabase::removeDatabase 要求的线程。
    connect(&m_indexThread, &QThread::finished, m_worker,
            [this] { m_worker->shutdown(); }, Qt::DirectConnection);
    connect(&m_indexThread, &QThread::finished, m_worker, &QObject::deleteLater);

    // 跨线程 → 自动排队连接；参数已在 fulltextsearch.h 里注册过元类型（IndexStats）
    connect(m_worker, &SearchIndexWorker::progress, this, &SearchPanel::onIndexProgress);
    connect(m_worker, &SearchIndexWorker::finished, this, &SearchPanel::onIndexFinished);
    connect(m_worker, &SearchIndexWorker::cancelled, this, &SearchPanel::onIndexCancelled);
    connect(m_worker, &SearchIndexWorker::failed, this, &SearchPanel::onIndexFailed);

    m_indexThread.setObjectName(QStringLiteral("search-index"));
    m_indexThread.start();

    setStatus(QStringLiteral("选一个目录，点「建立索引」，然后输入关键词搜索"));
}

SearchPanel::~SearchPanel()
{
    // ★ 必须 wait()：不 wait 的话线程可能还在跑，而面板的成员已经开始析构了。
    // 索引最多几百毫秒就结束了（取消点只在文件之间），所以这里不会卡住界面太久。
    m_indexThread.quit();
    m_indexThread.wait();
    // quit() 之后 Qt 仍然会处理 deferred deletion（QThread::finished 的文档明确写了这一点），
    // 所以 worker 会被上面那条 deleteLater 正常回收。这里清掉指针只是防止误用。
    m_worker = nullptr;
}

// ============================ 机制 ============================

void SearchPanel::setIndexPath(const QString &path)
{
    m_engine.setIndexPath(path);
}

FullTextSearch *SearchPanel::engine()
{
    return &m_engine;
}

void SearchPanel::setDirectory(const QString &dir)
{
    m_dirEdit->setText(QDir::toNativeSeparators(dir));
    refreshIndexSummary();
}

QString SearchPanel::directory() const
{
    return m_dirEdit->text().trimmed();
}

bool SearchPanel::buildIndex()
{
    if (m_indexing) {
        return false;  // 正在索引：别重入（界面上按钮此刻的角色是"取消索引"）
    }

    const QString dir = directory();
    if (dir.isEmpty()) {
        setStatus(QStringLiteral("先选一个要索引的目录"));
        return false;
    }

    // 读侧连接：确认索引库可用（也让"索引完就能搜"这件事成立）
    QString error;
    if (!m_engine.isOpen() && !m_engine.open(&error)) {
        setStatus(error);
        return false;
    }

    setBusy(true);
    setStatus(QStringLiteral("正在索引 %1 …").arg(QDir::toNativeSeparators(dir)));

    // ★ 从这里开始不再同步执行：投给工作线程，函数立刻返回。
    //   返回 true 的语义是"任务已提交"，不是"索引已完成" —— 见 onIndexFinished。
    const QString indexPath = m_engine.indexPath();
    QMetaObject::invokeMethod(m_worker, "startIndex", Qt::QueuedConnection,
                              Q_ARG(QString, dir), Q_ARG(QString, indexPath));
    return true;
}

bool SearchPanel::isIndexing() const
{
    return m_indexing;
}

int SearchPanel::runSearch(const QString &query)
{
    clearResults();

    const QString keyword = FullTextSearch::normalizeQuery(query);
    if (keyword.isEmpty()) {
        setStatus(QStringLiteral("输入关键词再搜"));
        return 0;
    }

    QString error;
    if (!m_engine.isOpen() && !m_engine.open(&error)) {
        setStatus(error);
        return 0;
    }
    if (m_engine.indexedFileCount() == 0) {
        setStatus(QStringLiteral("索引是空的：先选好目录并点「建立索引」"));
        return 0;
    }

    m_hits = m_engine.search(keyword, kMaxResults, &error);
    if (!error.isEmpty()) {
        setStatus(error);
        clearResults();
        return 0;
    }

    m_keyword = keyword;
    fillResults(keyword);

    if (m_hits.isEmpty()) {
        setStatus(QStringLiteral("没有找到「%1」（已索引 %2 个文件 / %3 行）")
                      .arg(keyword)
                      .arg(m_engine.indexedFileCount())
                      .arg(m_engine.indexedLineCount()));
        return 0;
    }

    QString text = QStringLiteral("找到 %1 条结果，涉及 %2 个文件").arg(m_hits.size()).arg(fileCount());
    if (m_hits.size() >= kMaxResults) {
        text += QStringLiteral("（只显示前 %1 条）").arg(kMaxResults);
    }
    setStatus(text);
    return m_hits.size();
}

void SearchPanel::clearResults()
{
    if (m_results != nullptr) {
        m_results->clear();
    }
    m_hits.clear();
    m_keyword.clear();
}

// ============================ 结果 ============================

int SearchPanel::hitCount() const
{
    return m_hits.size();
}

int SearchPanel::fileCount() const
{
    return resultFiles().size();
}

QStringList SearchPanel::resultFiles() const
{
    QStringList files;
    for (const SearchHit &hit : m_hits) {
        if (!files.contains(hit.filePath)) {
            files << hit.filePath;
        }
    }
    return files;
}

SearchHit SearchPanel::hitAt(int index) const
{
    if (index < 0 || index >= m_hits.size()) {
        return SearchHit();
    }
    return m_hits.at(index);
}

QString SearchPanel::statusText() const
{
    return m_status->text();
}

bool SearchPanel::activateResult(int index)
{
    if (index < 0 || index >= m_hits.size()) {
        return false;
    }
    // C1：整条命中发出去（不只是路径 + 行号）—— 主窗口要靠 matchStart/matchLength
    // 把命中的那几个字选中。只给行号的话，跳过去光标只能落在行首。
    emit resultActivated(m_hits.at(index));
    return true;
}

namespace {

// ---------------------------------------------------------------------------
// C1 的唯一实现：一次算出"显示哪一段文本"和"命中词在显示文本里的位置"。
//
// displayTextFor() 和 displaySpanFor() 都只是它的两个出口 —— 分成两份实现必然漂移，
// 而漂移的表现是"高亮块错位"，属于最难查的那类 bug（颜色是对的、位置差几个字，
// 单看截图会以为是字体渲染问题）。
//
// 有两处平移很容易漏，这里都在同一个函数里做掉：
//   ① **trim 平移**：hit.text 是原始行（可能带前导空白），而显示时用 trimmed()，
//      所以 matchStart 要先减去被去掉的前导空白数；
//   ② **截断平移**：长行会围绕命中处截一段，命中下标要再减去起始偏移，
//      而且前面补的那个"…"本身占 1 个字符位，也要算进去。
// ---------------------------------------------------------------------------
struct ShownText
{
    QString text;
    int spanStart = 0;
    int spanLength = 0;  // 0 = 这段没有可高亮的内容
};

ShownText buildShownText(const SearchHit &hit, int maxChars)
{
    // ---- ① 去掉两边的空白，并把命中下标同步平移 ----
    const QString raw = hit.text;
    int leading = 0;
    while (leading < raw.size() && raw.at(leading).isSpace()) {
        ++leading;
    }
    int trailing = raw.size();
    while (trailing > leading && raw.at(trailing - 1).isSpace()) {
        --trailing;
    }
    const QString line = raw.mid(leading, trailing - leading);

    // matchStart == -1 表示"没能定位到命中"（比如大小写差异），此时不做高亮。
    const int hitStart = (hit.matchStart >= 0) ? hit.matchStart - leading : -1;
    const int hitLength = hit.matchLength;

    ShownText out;

    // ---- 不需要截断：直接用整行 ----
    if (line.size() <= maxChars) {
        out.text = line;
        if (hitStart >= 0 && hitStart < line.size()) {
            out.spanStart = hitStart;
            out.spanLength = qBound(0, hitLength, line.size() - hitStart);
        }
        return out;
    }

    // ---- 需要截断：围绕命中处取一段 ----
    // 命中在很后面的时候，从命中前面一点点开始截 —— 否则用户看到的是"一片没用的前缀 + …"，
    // 还得自己猜命中的是哪个词。
    constexpr int kHead = 20;  // 命中前留一点上下文
    int start = 0;
    if (hitStart > maxChars - kHead) {
        start = qMax(0, hitStart - kHead);
    }

    QString shown = line.mid(start, maxChars);
    int prefix = 0;
    if (start > 0) {
        shown.prepend(QStringLiteral("…"));
        prefix = 1;  // ② 这个省略号也要算进偏移
    }
    if (start + maxChars < line.size()) {
        shown.append(QStringLiteral("…"));
    }

    out.text = shown;
    if (hitStart >= 0) {
        const int spanStart = hitStart - start + prefix;
        if (spanStart >= 0 && spanStart < shown.size()) {
            out.spanStart = spanStart;
            out.spanLength = qBound(0, hitLength, shown.size() - spanStart);
        }
    }
    return out;
}

}  // namespace

QString SearchPanel::displayTextFor(const SearchHit &hit, int maxChars)
{
    return buildShownText(hit, maxChars).text;
}

SearchPanel::DisplaySpan SearchPanel::displaySpanFor(const SearchHit &hit, int maxChars)
{
    const ShownText shown = buildShownText(hit, maxChars);
    DisplaySpan span;
    span.start = shown.spanStart;
    span.length = shown.spanLength;
    return span;
}

// ============================ 内部 ============================

void SearchPanel::fillResults(const QString &keyword)
{
    // 按文件分组：引擎已经按「文件 → 行号」排好序了，所以顺序遇到新文件就开一个新分组
    QTreeWidgetItem *group = nullptr;
    QString currentPath;
    int index = 0;

    for (const SearchHit &hit : m_hits) {
        if (group == nullptr || hit.filePath != currentPath) {
            currentPath = hit.filePath;
            group = new QTreeWidgetItem(m_results);
            // 显示相对路径（相对索引目录）：绝对路径太长，把"内容"列挤没了
            const QString dir = directory();
            const QString shown = dir.isEmpty() ? hit.filePath : QDir(dir).relativeFilePath(hit.filePath);
            group->setText(0, shown.isEmpty() ? hit.filePath : shown);
            group->setText(1, QString());
            QFont bold = group->font(0);
            bold.setBold(true);
            group->setFont(0, bold);
            group->setToolTip(0, hit.filePath);
            group->setFlags(Qt::ItemIsEnabled);  // 分组行只是分组：不选中、点了也不跳
            group->setExpanded(true);
        }

        const int matchesInGroup = group->childCount() + 1;
        group->setText(1, QStringLiteral("%1 处匹配").arg(matchesInGroup));

        auto *row = new QTreeWidgetItem(group);
        row->setText(0, QStringLiteral("行 %1").arg(hit.line));
        row->setText(1, displayTextFor(hit));
        // C1：把"命中词在显示文本里的区间"记在条目上，交给 SearchResultDelegate 上色。
        // 位置是**算好的结果**而不是原始下标 —— 原始 matchStart 是相对原文的，
        // 经过 trim + 截断之后已经对不上了（见 buildShownText）。
        const DisplaySpan span = displaySpanFor(hit);
        row->setData(1, SearchResultDelegate::kMatchSpanRole, QPoint(span.start, span.length));
        row->setToolTip(0, QStringLiteral("%1:%2").arg(QDir::toNativeSeparators(hit.filePath)).arg(hit.line));
        row->setToolTip(1, hit.text.trimmed());
        // 行下标存在条目上：单击时靠它回到 m_hits 里的那一条（不靠"数第几行"）
        row->setData(0, Qt::UserRole, index);
        ++index;
    }

    if (m_results->topLevelItemCount() > 0) {
        m_results->expandAll();
    }
}

void SearchPanel::onSearchRequested()
{
    runSearch(m_query->text());
}

void SearchPanel::onBrowseClicked()
{
    const QString start = directory().isEmpty() ? QDir::homePath() : directory();
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择要索引的目录"), start);
    if (!dir.isEmpty()) {
        setDirectory(dir);
    }
}

void SearchPanel::onIndexClicked()
{
    if (m_indexing) {
        // 索引中：这个按钮此刻的角色是"取消"（见 setBusy：只有它保持可用）。
        //
        // ★ 这里必须**直接调用**，不能用 invokeMethod(..., Qt::QueuedConnection)。
        //   原因：工作线程此刻正卡在 startIndex() → indexDirectory() 里面，
        //   它的事件循环根本没在跑。排队投过去的调用要等 startIndex() 返回才会被处理 ——
        //   那时候索引已经结束了，"取消"就永远不会生效。
        //   cancel() 内部只是 m_cancelRequested.store(true)（原子写），
        //   从 GUI 线程直接调是线程安全的，也正是它被设计成原子标志的原因。
        if (m_worker != nullptr) {
            m_worker->cancel();
        }
        setStatus(QStringLiteral("正在取消…"));
        return;
    }
    buildIndex();
}

void SearchPanel::onItemClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (item == nullptr || item->parent() == nullptr) {
        return;  // 文件分组行：不跳转
    }
    const QVariant index = item->data(0, Qt::UserRole);
    if (!index.isValid()) {
        return;
    }
    activateResult(index.toInt());
}

void SearchPanel::onIndexProgress(int done, int total)
{
    setStatus(QStringLiteral("正在索引 %1/%2 …").arg(done).arg(total));
    // ★ 这里原来有一句 QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents)。
    //   索引移入工作线程之后它必须删掉：
    //   * 已经不需要 —— GUI 线程本来就是空闲的，事件循环正常运转；
    //   * 而且有害 —— 它会在一个信号处理里嵌套地跑事件循环，是重入的温床。
}

void SearchPanel::onIndexFinished(const IndexStats &stats)
{
    LOG_INFO("面板收到索引结束：%1 个文件 / %2 行", stats.filesFound, stats.linesIndexed);
    setBusy(false);

    // ★ 顺序不能反：先刷"索引概况"，再写"索引完成"那句。
    //   refreshIndexSummary() 自己也会写状态栏（"索引：N 个文件 / M 行"），
    //   放在后面就会把下面那句精心拼好的话**整个覆盖掉** ——
    //   用户永远看不到"新建 / 跳过 / 清理 / 用时多少"，而那正是这个信号存在的意义。
    //   这个顺序被 tests/test_fulltextsearch.cpp 钉住（它断言状态栏里有「索引完成」），
    //   以后谁再把两行调过来，测试会立刻红。
    refreshIndexSummary();

    // 统计里每一项都说清楚：用户最想知道的是"到底索引了多少"
    setStatus(QStringLiteral("索引完成：%1 个文件 / %2 行（新建 %3，跳过 %4，清理 %5，用时 %6 ms）")
                  .arg(stats.filesFound)
                  .arg(stats.linesIndexed)
                  .arg(stats.filesIndexed)
                  .arg(stats.filesSkipped)
                  .arg(stats.filesRemoved)
                  .arg(stats.elapsedMs));
}

void SearchPanel::onIndexCancelled()
{
    setBusy(false);

    // 同上：先刷概况，再写取消那句（不然"索引已取消"会被概况覆盖掉，
    // 用户会以为索引还在跑或者跑完了）。
    refreshIndexSummary();
    setStatus(QStringLiteral("索引已取消（索引保持在取消前的状态）"));
}

void SearchPanel::onIndexFailed(const QString &message)
{
    setBusy(false);
    setStatus(message);
}

void SearchPanel::setStatus(const QString &text)
{
    m_status->setText(text);
    emit statusMessage(text);
}

void SearchPanel::setBusy(bool busy)
{
    m_indexing = busy;
    // ★ 索引中**不能**禁用 m_indexButton：它此刻的角色是"取消索引"（见 onIndexClicked）。
    //   其余控件禁掉 —— 索引期间搜索是可以的（读侧走 WAL），但为了不给用户
    //   "边索引边搜会不会读到半个文件"的疑虑，这里保持简单：索引中只允许取消。
    m_indexButton->setEnabled(true);
    m_indexButton->setText(busy ? QStringLiteral("取消索引") : QStringLiteral("建立索引"));
    m_browseButton->setEnabled(!busy);
    m_searchButton->setEnabled(!busy);
    m_query->setEnabled(!busy);
}

void SearchPanel::refreshIndexSummary()
{
    if (m_indexing) {
        return;
    }
    if (!m_engine.isOpen()) {
        setStatus(QStringLiteral("还没打开索引库（第一次建立索引时会自动打开）"));
        return;
    }
    const int files = m_engine.indexedFileCount();
    const int lines = m_engine.indexedLineCount();
    if (files == 0) {
        setStatus(QStringLiteral("索引是空的：点「建立索引」把当前目录扫一遍"));
    } else {
        setStatus(QStringLiteral("索引：%1 个文件 / %2 行").arg(files).arg(lines));
    }
}
