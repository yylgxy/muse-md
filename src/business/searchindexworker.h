#ifndef SEARCHINDEXWORKER_H
#define SEARCHINDEXWORKER_H

#include <QObject>
#include <QString>

#include <memory>

#include "fulltextsearch.h"  // IndexStats 是信号参数，需要完整定义

// 索引工作线程的"躯干"（A1）。
//
// ---- 为什么单独一个类，而不是让 SearchPanel 继承 QThread ----
// 继承 QThread 重写 run() 时，run() 里的代码跑在新线程，但**对象本身仍归属创建它的线程**：
// 成员、信号槽、deleteLater 全在旧线程 —— 这是 Qt 线程编程最常踩的坑。
// 这里用 Worker + moveToThread：对象的归属线程和执行线程是同一个，语义清楚。
//
// ---- 为什么 FullTextSearch 是懒创建（unique_ptr + 第一次调用才 new）----
// QSqlDatabase 的连接**绑定在创建它的线程**上。如果本对象在主线程 new 出引擎、
// 再把引擎带到工作线程用，连接就属于主线程，工作线程里用会告警且不可用。
// 所以：引擎必须在**工作线程里**创建，而"第一次 startIndex() 被调用时"正好就是那个时机
//（moveToThread 之后，槽函数在工作线程里执行）。
//
// ---- 生命周期（和 SearchPanel 的约定，改之前先看这里）----
//   * 构造：主线程；立刻 moveToThread(工作线程)
//   * startIndex()：工作线程（排队调用）
//   * cancel()：任意线程（内部只是置一个原子标志，不碰数据库）
//   * shutdown()：工作线程（SearchPanel 用 DirectConnection 接 QThread::finished ——
//                  finished 是在"正在结束的那个线程"里发出的，所以它一定跑在工作线程）
//   * 析构：工作线程（QThread::finished → deleteLater）
//
// ---- 为什么它不依赖 QWidget ----
// 本项目的既有惯例：需要自动验证的逻辑都不依赖界面（见 docs/PROJECT_STUDY_GUIDE.md 第 8 节）。
// 这个类能在测试里直接实例化、配一个 QThread 就跑起来，不需要起 Chromium。
class SearchIndexWorker : public QObject
{
    Q_OBJECT

public:
    explicit SearchIndexWorker(QObject *parent = nullptr);
    ~SearchIndexWorker() override;

public slots:
    // 在工作线程里跑一次完整索引。indexPath 为空的用默认路径。
    // 这是**唯一**会碰索引库写路径的地方。
    void startIndex(const QString &dir, const QString &indexPath);

    // 请求取消正在跑的那次索引（线程安全：内部只是置一个原子标志）
    void cancel();

    // 线程结束前释放数据库连接。必须在**工作线程**里执行（见类注释的生命周期）。
    void shutdown();

signals:
    // 进度（按文件数）。跨线程发回 GUI，Qt 自动用排队连接。
    void progress(int done, int total);
    // 索引正常完成
    void finished(const IndexStats &stats);
    // 索引被取消（事务已回滚，索引保持上一次的完整状态）
    void cancelled();
    // 失败（原因已经是人话了）
    void failed(const QString &message);

private:
    // 懒创建：第一次 startIndex() 时在**工作线程**里 new（见类注释）
    std::unique_ptr<FullTextSearch> m_engine;
};

#endif // SEARCHINDEXWORKER_H
