#include "searchindexworker.h"

#include "logger.h"

SearchIndexWorker::SearchIndexWorker(QObject *parent) : QObject(parent) {}

SearchIndexWorker::~SearchIndexWorker()
{
    // 兜底：正常路径上 shutdown() 已经跑过，这里再调一次是幂等的（close() 本身幂等）。
    // 走到这里说明对象正在被析构，而它归属的线程已经是工作线程（见类注释的生命周期）。
    shutdown();
}

void SearchIndexWorker::shutdown()
{
    if (m_engine) {
        // ★ 必须在工作线程里执行：QSqlDatabase::removeDatabase 要求连接不再被任何线程持有，
        //   而这条连接是在工作线程里创建的。
        m_engine->close();
        m_engine.reset();
    }
}

void SearchIndexWorker::startIndex(const QString &dir, const QString &indexPath)
{
    if (!m_engine) {
        // ★ 关键的一行：引擎在这里（= 工作线程）被创建，所以它的数据库连接属于工作线程。
        m_engine = std::make_unique<FullTextSearch>();
        // 引擎自己的进度信号原地转出去。同线程 = 直接调用，零开销。
        connect(m_engine.get(), &FullTextSearch::indexProgress, this, &SearchIndexWorker::progress);
        connect(m_engine.get(), &FullTextSearch::errorOccurred, this, &SearchIndexWorker::failed);
    }

    if (!indexPath.isEmpty() && m_engine->indexPath() != indexPath) {
        m_engine->close();  // 换库：先关（close 幂等），下次 open 会用新路径
        m_engine->setIndexPath(indexPath);
    }

    QString error;
    if (!m_engine->isOpen() && !m_engine->open(&error)) {
        emit failed(error);
        return;
    }

    const IndexStats stats = m_engine->indexDirectory(dir, &error);
    if (!error.isEmpty()) {
        // indexDirectory 自己也会发 errorOccurred，这里保证**一定有**一个终止信号
        //（界面靠它把按钮恢复可用，漏了这一步界面会永远卡在"正在索引"）。
        emit failed(error);
        return;
    }
    if (stats.cancelled) {
        emit cancelled();
        return;
    }
    emit finished(stats);
}

void SearchIndexWorker::cancel()
{
    if (m_engine) {
        m_engine->requestCancel();
    }
    // m_engine 还没建（第一次 startIndex 之前就点了取消）：什么都不用做 ——
    // 没有正在跑的索引，也就没有需要取消的东西。
}
