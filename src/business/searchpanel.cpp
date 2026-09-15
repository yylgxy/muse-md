#include "searchpanel.h"

#include "logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
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

    // 索引进度和结果由引擎发信号回来，面板只负责"表达"
    connect(&m_engine, &FullTextSearch::indexProgress, this, &SearchPanel::onIndexProgress);
    connect(&m_engine, &FullTextSearch::indexFinished, this, &SearchPanel::onIndexFinished);
    connect(&m_engine, &FullTextSearch::errorOccurred, this, [this](const QString &message) {
        setStatus(message);
    });

    setStatus(QStringLiteral("选一个目录，点「建立索引」，然后输入关键词搜索"));
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
        return false;  // 正在索引：别重入（界面上按钮也是禁用的）
    }

    const QString dir = directory();
    if (dir.isEmpty()) {
        setStatus(QStringLiteral("先选一个要索引的目录"));
        return false;
    }

    QString error;
    if (!m_engine.isOpen() && !m_engine.open(&error)) {
        setStatus(error);
        return false;
    }

    setBusy(true);
    setStatus(QStringLiteral("正在索引 %1 …").arg(QDir::toNativeSeparators(dir)));
    // 索引是同步跑的（小目录毫秒级）。为了界面不假死，进度信号里会转一下事件循环
    //（见 onIndexProgress：只允许重绘，不处理用户输入，所以不会重入）。
    const IndexStats stats = m_engine.indexDirectory(dir, &error);
    setBusy(false);

    if (!error.isEmpty()) {
        setStatus(error);
        return false;
    }

    // 统计里每一项都说清楚：用户最想知道的是"到底索引了多少"
    setStatus(QStringLiteral("索引完成：%1 个文件 / %2 行（新建 %3，跳过 %4，清理 %5，用时 %6 ms）")
                  .arg(stats.filesFound)
                  .arg(stats.linesIndexed)
                  .arg(stats.filesIndexed)
                  .arg(stats.filesSkipped)
                  .arg(stats.filesRemoved)
                  .arg(stats.elapsedMs));
    return true;
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
    const SearchHit hit = m_hits.at(index);
    emit resultActivated(hit.filePath, hit.line);
    return true;
}

QString SearchPanel::displayTextFor(const SearchHit &hit, int maxChars)
{
    const QString line = hit.text.trimmed();
    if (line.size() <= maxChars) {
        return line;
    }

    // 命中在很后面的时候，从命中前面一点点开始截 —— 否则用户看到的是"一片没用的前缀 + …"，
    // 还得自己猜命中的是哪个词。
    const int head = 20;  // 命中前留一点上下文
    int start = 0;
    if (hit.matchStart > maxChars - head) {
        start = qMax(0, hit.matchStart - head);
    }

    QString shown = line.mid(start, maxChars);
    if (start > 0) {
        shown.prepend(QStringLiteral("…"));
    }
    if (start + maxChars < line.size()) {
        shown.append(QStringLiteral("…"));
    }
    return shown;
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
    // 同步索引时界面要能重绘，否则用户看到的是"卡死"。
    // 只允许重绘、**不处理用户输入**：这样不会在索引途中被点进另一个槽（重入）。
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void SearchPanel::onIndexFinished(const IndexStats &stats)
{
    LOG_INFO("面板收到索引结束：%1 个文件 / %2 行", stats.filesFound, stats.linesIndexed);
    refreshIndexSummary();
}

void SearchPanel::setStatus(const QString &text)
{
    m_status->setText(text);
    emit statusMessage(text);
}

void SearchPanel::setBusy(bool busy)
{
    m_indexing = busy;
    m_indexButton->setEnabled(!busy);
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
